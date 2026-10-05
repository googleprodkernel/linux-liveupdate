// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Google LLC.
 *
 * KVM selftest for vCPU architectural state preservation across kexec (Live
 * Update) via LUO using regular memfd-backed guest memory.
 *
 * Stage 1:
 *   1. Creates a 2-vCPU VM with Slot 0 backed by a regular memfd.
 *   2. Runs guest code on each vCPU to configure known GPRs, MSRs/sysregs,
 *      FPU/FPSIMD registers, and architectural timer state.
 *   3. Runs a 3-iteration preserve/cancel loop without kexec, verifying both
 *      host-side and in-guest state across unpreserve/resume.
 *   4. Preserves the VM fd, memfd, and vCPU fds in a LUO session and
 *      daemonizes awaiting kexec.
 *
 * Stage 2 (after kexec):
 *   1. Retrieves the preserved VM fd, memfd, and vCPU fds from LUO.
 *   2. Reconstructs the VM and binds the retrieved memfd to Slot 0.
 *   3. Verifies preserved GPR, MSR/sysreg, FPU/FPSIMD, and timer state from
 *      the host before resuming guest execution.
 *   4. Resumes both vCPUs and verifies in-guest register, MSR/sysreg,
 *      FPU/FPSIMD, timer, and memory state to completion (GUEST_DONE).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/sizes.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "ucall_common.h"
#include "../kselftest.h"

#ifdef __x86_64__
#include "apic.h"
#elif defined(__aarch64__)
#include "arch_timer.h"
#include "gic.h"
#include "gic_v3.h"
#include "vgic.h"
#endif

#include <libliveupdate.h>

#define NR_TEST_VCPUS		2
#define NR_CANCEL_ITERS		3
#define NR_SLOT0_PAGES		2048ULL /* 8 MB for Slot 0 */
#define DATA_PATTERN_SIZE	4096

#define VCPU_TEST_MAGIC		0x564350554c554f31ULL /* "VCPULUO1" */

#define SESSION_NAME		"vcpu_preservation_session"
#define CANCEL_SESSION_NAME	"vcpu_cancel_session"
#define STATE_SESSION_NAME	"vcpu_preservation_state"

#define STATE_TOKEN		0x999
#define VM_TOKEN		0x2001
#define MEMFD_TOKEN		0x2002
#define VCPU_TOKEN_BASE		0x2100

/* Stored at Page 0 of Slot 0 memfd (GPA 0, unused by kvm_util since KVM_UTIL_MIN_PFN == 2). */
struct vcpu_test_meta {
	uint64_t magic;
	uint64_t slot0_hva;
	uint64_t slot0_size;
	uint64_t ucall_mmio_gpa;
	uint64_t ucalls_offset;
	uint64_t sh_gpa;
	uint32_t has_gic;
	uint32_t reserved;
};

struct vcpu_test_shared {
	uint64_t magic;
	struct ucall uc[NR_TEST_VCPUS];
	uint64_t ucall_mmio_gva;
	uint64_t sync_stage[NR_TEST_VCPUS];
	uint64_t stage2_verified[NR_TEST_VCPUS];
	uint64_t irq_count[NR_TEST_VCPUS];
	uint32_t has_gic;
	uint32_t reserved;
	uint8_t data_pattern[DATA_PATTERN_SIZE];
};

static inline uint8_t pattern_byte(uint32_t vcpu_id, size_t idx)
{
	return (uint8_t)((idx * 31U) ^ (0xA5U + vcpu_id));
}

#ifdef __x86_64__

#ifndef MSR_TSC_AUX
#define MSR_TSC_AUX		0xc0000103
#endif
#ifndef MSR_FS_BASE
#define MSR_FS_BASE		0xc0000100
#endif
#ifndef MSR_KERNEL_GS_BASE
#define MSR_KERNEL_GS_BASE	0xc0000102
#endif

static inline uint64_t exp_rbx(uint32_t id) { return 0x1111222233330000ULL + id; }
static inline uint64_t exp_r12(uint32_t id) { return 0x4444555566660000ULL + id; }
static inline uint64_t exp_r13(uint32_t id) { return 0x7777888899990000ULL + id; }
static inline uint64_t exp_r14(uint32_t id) { return 0xaaaabbbbcccc0000ULL + id; }
static inline uint64_t exp_r15(uint32_t id) { return 0xddddeeeeffff0000ULL + id; }
static inline uint64_t exp_dr0(uint32_t id) { return 0x00000000dead0000ULL + id; }

static inline uint64_t exp_tsc_aux(uint32_t id) { return 0xcafe0000ULL | id; }
static inline uint64_t exp_fs_base(uint32_t id)
{
	return 0x0000123456780000ULL + ((uint64_t)id << 16);
}

static inline uint64_t exp_gs_base(uint32_t id)
{
	return 0x0000432187650000ULL + ((uint64_t)id << 16);
}

static inline uint64_t exp_xmm0_lo(uint32_t id) { return 0x0102030405060708ULL + id; }
static inline uint64_t exp_xmm0_hi(uint32_t id) { return 0x1112131415161718ULL + id; }

#define EXP_APIC_TDCR		0x3U
#define EXP_APIC_LVTT		(APIC_LVT_MASKED | 0xefU)
static inline uint32_t exp_apic_tmict(uint32_t id) { return 0x00abcdefU + id; }

static void guest_setup_arch_state(struct vcpu_test_shared *sh, uint32_t vcpu_id)
{
	uint64_t dr0 = exp_dr0(vcpu_id);

	(void)sh;
	x2apic_enable();
	x2apic_write_reg(APIC_TDCR, EXP_APIC_TDCR);
	x2apic_write_reg(APIC_LVTT, EXP_APIC_LVTT);
	x2apic_write_reg(APIC_TMICT, exp_apic_tmict(vcpu_id));

	wrmsr(MSR_TSC_AUX, exp_tsc_aux(vcpu_id));
	wrmsr(MSR_FS_BASE, exp_fs_base(vcpu_id));
	wrmsr(MSR_KERNEL_GS_BASE, exp_gs_base(vcpu_id));
	asm volatile("mov %0, %%dr0" : : "r"(dr0));
}

static void guest_verify_arch_msrs_and_timer(struct vcpu_test_shared *sh,
					     uint32_t vcpu_id)
{
	uint64_t dr0;

	(void)sh;
	__GUEST_ASSERT(rdmsr(MSR_TSC_AUX) == exp_tsc_aux(vcpu_id),
		       "vCPU %u MSR_TSC_AUX mismatch: got 0x%lx",
		       vcpu_id, rdmsr(MSR_TSC_AUX));
	__GUEST_ASSERT(rdmsr(MSR_FS_BASE) == exp_fs_base(vcpu_id),
		       "vCPU %u MSR_FS_BASE mismatch: got 0x%lx",
		       vcpu_id, rdmsr(MSR_FS_BASE));
	__GUEST_ASSERT(rdmsr(MSR_KERNEL_GS_BASE) == exp_gs_base(vcpu_id),
		       "vCPU %u MSR_KERNEL_GS_BASE mismatch: got 0x%lx",
		       vcpu_id, rdmsr(MSR_KERNEL_GS_BASE));

	asm volatile("mov %%dr0, %0" : "=r"(dr0));
	__GUEST_ASSERT(dr0 == exp_dr0(vcpu_id),
		       "vCPU %u DR0 mismatch: got 0x%lx", vcpu_id, dr0);

	__GUEST_ASSERT(x2apic_read_reg(APIC_TDCR) == EXP_APIC_TDCR,
		       "vCPU %u APIC_TDCR mismatch: got 0x%lx",
		       vcpu_id, x2apic_read_reg(APIC_TDCR));
	__GUEST_ASSERT(x2apic_read_reg(APIC_LVTT) == EXP_APIC_LVTT,
		       "vCPU %u APIC_LVTT mismatch: got 0x%lx",
		       vcpu_id, x2apic_read_reg(APIC_LVTT));
	__GUEST_ASSERT(x2apic_read_reg(APIC_TMICT) == exp_apic_tmict(vcpu_id),
		       "vCPU %u APIC_TMICT mismatch: got 0x%lx",
		       vcpu_id, x2apic_read_reg(APIC_TMICT));
}

static noinline void guest_sync_with_regs(struct vcpu_test_shared *sh,
					  uint32_t vcpu_id, uint64_t stage)
{
	uint64_t in_xmm[2] __aligned(16) = {
		exp_xmm0_lo(vcpu_id),
		exp_xmm0_hi(vcpu_id),
	};
	uint64_t out_xmm[2] __aligned(16) = {};
	uint64_t in_rbx = exp_rbx(vcpu_id);
	uint64_t in_r12 = exp_r12(vcpu_id);
	uint64_t in_r13 = exp_r13(vcpu_id);
	uint64_t in_r14 = exp_r14(vcpu_id);
	uint64_t in_r15 = exp_r15(vcpu_id);
	uint64_t out_rbx, out_r12, out_r13, out_r14, out_r15;
	struct ucall *uc = &sh->uc[vcpu_id];
	uint64_t uc_hva;

	memset(uc->args, 0, sizeof(uc->args));
	WRITE_ONCE(uc->cmd, UCALL_SYNC);
	WRITE_ONCE(uc->args[0], stage);
	WRITE_ONCE(sh->sync_stage[vcpu_id], stage);
	uc_hva = (uint64_t)READ_ONCE(uc->hva);

	asm volatile(
		"movdqu %[in_xmm], %%xmm0\n\t"
		"movq %[in_rbx], %%rbx\n\t"
		"movq %[in_r12], %%r12\n\t"
		"movq %[in_r13], %%r13\n\t"
		"movq %[in_r14], %%r14\n\t"
		"movq %[in_r15], %%r15\n\t"
		"movq %[uc_hva], %%rdi\n\t"
		"movw $0x1000, %%dx\n\t"
		"in %%dx, %%al\n\t"
		"movq %%rbx, %[out_rbx]\n\t"
		"movq %%r12, %[out_r12]\n\t"
		"movq %%r13, %[out_r13]\n\t"
		"movq %%r14, %[out_r14]\n\t"
		"movq %%r15, %[out_r15]\n\t"
		"movdqu %%xmm0, %[out_xmm]\n\t"
		: [out_rbx] "=m" (out_rbx),
		  [out_r12] "=m" (out_r12),
		  [out_r13] "=m" (out_r13),
		  [out_r14] "=m" (out_r14),
		  [out_r15] "=m" (out_r15),
		  [out_xmm] "=m" (out_xmm)
		: [in_rbx] "m" (in_rbx),
		  [in_r12] "m" (in_r12),
		  [in_r13] "m" (in_r13),
		  [in_r14] "m" (in_r14),
		  [in_r15] "m" (in_r15),
		  [in_xmm] "m" (in_xmm),
		  [uc_hva] "m" (uc_hva)
		: "rax", "rdx", "rdi", "rbx", "r12", "r13", "r14", "r15",
		  "xmm0", "memory"
	);

	__GUEST_ASSERT(out_rbx == in_rbx, "vCPU %u RBX mismatch: 0x%lx != 0x%lx",
		       vcpu_id, out_rbx, in_rbx);
	__GUEST_ASSERT(out_r12 == in_r12, "vCPU %u R12 mismatch: 0x%lx != 0x%lx",
		       vcpu_id, out_r12, in_r12);
	__GUEST_ASSERT(out_r13 == in_r13, "vCPU %u R13 mismatch: 0x%lx != 0x%lx",
		       vcpu_id, out_r13, in_r13);
	__GUEST_ASSERT(out_r14 == in_r14, "vCPU %u R14 mismatch: 0x%lx != 0x%lx",
		       vcpu_id, out_r14, in_r14);
	__GUEST_ASSERT(out_r15 == in_r15, "vCPU %u R15 mismatch: 0x%lx != 0x%lx",
		       vcpu_id, out_r15, in_r15);
	__GUEST_ASSERT(out_xmm[0] == in_xmm[0] && out_xmm[1] == in_xmm[1],
		       "vCPU %u XMM0 mismatch: 0x%lx:0x%lx != 0x%lx:0x%lx",
		       vcpu_id, out_xmm[1], out_xmm[0], in_xmm[1], in_xmm[0]);

	guest_verify_arch_msrs_and_timer(sh, vcpu_id);
}

static void host_verify_vcpu_state(struct kvm_vcpu *vcpu, uint32_t id)
{
	struct kvm_vcpu_events events;
	struct kvm_debugregs dbgregs;
	struct kvm_lapic_state lapic;
	struct kvm_mp_state mp_state;
	struct kvm_sregs2 sregs2;
	struct kvm_regs regs;
	struct kvm_xcrs xcrs;
	struct kvm_fpu fpu;
	uint64_t xmm0_lo, xmm0_hi;
	uint32_t tdcr, lvtt, tmict;

	vcpu_regs_get(vcpu, &regs);
	TEST_ASSERT_EQ(regs.rbx, exp_rbx(id));
	TEST_ASSERT_EQ(regs.r12, exp_r12(id));
	TEST_ASSERT_EQ(regs.r13, exp_r13(id));
	TEST_ASSERT_EQ(regs.r14, exp_r14(id));
	TEST_ASSERT_EQ(regs.r15, exp_r15(id));

	vcpu_ioctl(vcpu, KVM_GET_SREGS2, &sregs2);
	TEST_ASSERT(sregs2.cr0 & X86_CR0_PE, "Expected CR0.PE set");
	TEST_ASSERT(sregs2.cr0 & X86_CR0_PG, "Expected CR0.PG set");
	TEST_ASSERT(sregs2.efer & EFER_LMA, "Expected EFER.LMA set");

	vcpu_ioctl(vcpu, KVM_GET_XCRS, &xcrs);
	TEST_ASSERT(xcrs.nr_xcrs >= 1, "Expected at least 1 XCR");
	TEST_ASSERT(xcrs.xcrs[0].value & 0x3, "Expected XCR0 FP/SSE bits set");

	vcpu_ioctl(vcpu, KVM_GET_DEBUGREGS, &dbgregs);
	TEST_ASSERT_EQ(dbgregs.db[0], exp_dr0(id));

	vcpu_mp_state_get(vcpu, &mp_state);
	TEST_ASSERT_EQ(mp_state.mp_state, KVM_MP_STATE_RUNNABLE);

	vcpu_events_get(vcpu, &events);
	TEST_ASSERT_EQ(events.exception.injected, 0);

	TEST_ASSERT_EQ(vcpu_get_msr(vcpu, MSR_TSC_AUX), exp_tsc_aux(id));
	TEST_ASSERT_EQ(vcpu_get_msr(vcpu, MSR_FS_BASE), exp_fs_base(id));
	TEST_ASSERT_EQ(vcpu_get_msr(vcpu, MSR_KERNEL_GS_BASE), exp_gs_base(id));

	vcpu_fpu_get(vcpu, &fpu);
	memcpy(&xmm0_lo, &fpu.xmm[0][0], sizeof(xmm0_lo));
	memcpy(&xmm0_hi, &fpu.xmm[0][8], sizeof(xmm0_hi));
	TEST_ASSERT_EQ(xmm0_lo, exp_xmm0_lo(id));
	TEST_ASSERT_EQ(xmm0_hi, exp_xmm0_hi(id));

	vcpu_ioctl(vcpu, KVM_GET_LAPIC, &lapic);
	memcpy(&tdcr, lapic.regs + APIC_TDCR, sizeof(tdcr));
	memcpy(&lvtt, lapic.regs + APIC_LVTT, sizeof(lvtt));
	memcpy(&tmict, lapic.regs + APIC_TMICT, sizeof(tmict));
	TEST_ASSERT_EQ(tdcr, EXP_APIC_TDCR);
	TEST_ASSERT_EQ(lvtt, EXP_APIC_LVTT);
	TEST_ASSERT_EQ(tmict, exp_apic_tmict(id));
}

#elif defined(__aarch64__)

extern gva_t *ucall_exit_mmio_addr;

static uint64_t guest_irq_count[NR_TEST_VCPUS];

static inline uint64_t exp_x19(uint32_t id) { return 0x1111222233330000ULL + id; }
static inline uint64_t exp_x20(uint32_t id) { return 0x4444555566660000ULL + id; }
static inline uint64_t exp_x21(uint32_t id) { return 0x7777888899990000ULL + id; }
static inline uint64_t exp_x22(uint32_t id) { return 0xaaaabbbbcccc0000ULL + id; }
static inline uint64_t exp_x23(uint32_t id) { return 0xddddeeeeffff0000ULL + id; }

static inline uint64_t exp_tpidr_el0(uint32_t id)
{
	return 0x0000123456780000ULL + ((uint64_t)id << 16);
}

static inline uint64_t exp_tpidrro_el0(uint32_t id)
{
	return 0x0000876543210000ULL + ((uint64_t)id << 16);
}
static inline uint64_t exp_contextidr_el1(uint32_t id) { return 0xabcd0000ULL | id; }
static inline uint64_t exp_d8(uint32_t id) { return 0x0102030405060708ULL + id; }

static inline uint64_t exp_cntv_cval(uint32_t id) { return 0x7fff000012340000ULL + id; }
#define EXP_CNTV_CTL		(CTL_ENABLE | CTL_IMASK)
#define EXP_ICC_PMR		0xf0ULL

static void guest_irq_handler(struct ex_regs *regs)
{
	uint32_t vcpu_id = guest_get_vcpuid();
	uint64_t iar = read_sysreg_s(SYS_ICC_IAR1_EL1);

	(void)regs;
	write_sysreg(EXP_CNTV_CTL, cntv_ctl_el0);
	isb();
	if (iar != ICC_IAR1_EL1_SPURIOUS) {
		write_sysreg_s(iar, SYS_ICC_EOIR1_EL1);
		isb();
	}
	WRITE_ONCE(guest_irq_count[vcpu_id], READ_ONCE(guest_irq_count[vcpu_id]) + 1);
}

static void guest_setup_arch_state(struct vcpu_test_shared *sh, uint32_t vcpu_id)
{
	write_sysreg(exp_tpidr_el0(vcpu_id), tpidr_el0);
	write_sysreg(exp_tpidrro_el0(vcpu_id), tpidrro_el0);
	write_sysreg(exp_contextidr_el1(vcpu_id), contextidr_el1);
	write_sysreg(exp_cntv_cval(vcpu_id), cntv_cval_el0);
	write_sysreg(EXP_CNTV_CTL, cntv_ctl_el0);
	if (sh->has_gic) {
		write_sysreg_s(EXP_ICC_PMR, SYS_ICC_PMR_EL1);
		write_sysreg_s(ICC_IGRPEN1_EL1_MASK, SYS_ICC_IGRPEN1_EL1);
	}
	isb();
}

static void guest_verify_arch_sysregs_and_timer(struct vcpu_test_shared *sh,
						uint32_t vcpu_id)
{
	uint64_t ctl;

	__GUEST_ASSERT(read_sysreg(tpidr_el0) == exp_tpidr_el0(vcpu_id),
		       "vCPU %u TPIDR_EL0 mismatch: got 0x%lx",
		       vcpu_id, read_sysreg(tpidr_el0));
	__GUEST_ASSERT(read_sysreg(tpidrro_el0) == exp_tpidrro_el0(vcpu_id),
		       "vCPU %u TPIDRRO_EL0 mismatch: got 0x%lx",
		       vcpu_id, read_sysreg(tpidrro_el0));
	__GUEST_ASSERT(read_sysreg(contextidr_el1) == exp_contextidr_el1(vcpu_id),
		       "vCPU %u CONTEXTIDR_EL1 mismatch: got 0x%lx",
		       vcpu_id, read_sysreg(contextidr_el1));
	__GUEST_ASSERT(read_sysreg(cntv_cval_el0) == exp_cntv_cval(vcpu_id),
		       "vCPU %u CNTV_CVAL_EL0 mismatch: got 0x%lx",
		       vcpu_id, read_sysreg(cntv_cval_el0));
	ctl = read_sysreg(cntv_ctl_el0) & (CTL_ENABLE | CTL_IMASK);
	__GUEST_ASSERT(ctl == EXP_CNTV_CTL,
		       "vCPU %u CNTV_CTL_EL0 mismatch: got 0x%lx",
		       vcpu_id, read_sysreg(cntv_ctl_el0));
	if (sh->has_gic) {
		__GUEST_ASSERT(read_sysreg_s(SYS_ICC_PMR_EL1) == EXP_ICC_PMR,
			       "vCPU %u ICC_PMR_EL1 mismatch: got 0x%lx",
			       vcpu_id, read_sysreg_s(SYS_ICC_PMR_EL1));
		__GUEST_ASSERT((read_sysreg_s(SYS_ICC_IGRPEN1_EL1) &
				ICC_IGRPEN1_EL1_MASK) == ICC_IGRPEN1_EL1_MASK,
			       "vCPU %u ICC_IGRPEN1_EL1 mismatch: got 0x%lx",
			       vcpu_id, read_sysreg_s(SYS_ICC_IGRPEN1_EL1));
	}
}

static noinline void guest_sync_with_regs(struct vcpu_test_shared *sh,
					  uint32_t vcpu_id, uint64_t stage)
{
	uint64_t in_x19 = exp_x19(vcpu_id);
	uint64_t in_x20 = exp_x20(vcpu_id);
	uint64_t in_x21 = exp_x21(vcpu_id);
	uint64_t in_x22 = exp_x22(vcpu_id);
	uint64_t in_x23 = exp_x23(vcpu_id);
	uint64_t in_d8 = exp_d8(vcpu_id);
	uint64_t out_x19, out_x20, out_x21, out_x22, out_x23, out_d8;
	struct ucall *uc = &sh->uc[vcpu_id];
	uint64_t mmio_gva = (uint64_t)ucall_exit_mmio_addr;
	uint64_t uc_hva;

	memset(uc->args, 0, sizeof(uc->args));
	WRITE_ONCE(uc->cmd, UCALL_SYNC);
	WRITE_ONCE(uc->args[0], stage);
	WRITE_ONCE(sh->sync_stage[vcpu_id], stage);
	uc_hva = (uint64_t)READ_ONCE(uc->hva);

	asm volatile(
		"fmov d8, %[in_d8]\n\t"
		"mov x19, %[in_x19]\n\t"
		"mov x20, %[in_x20]\n\t"
		"mov x21, %[in_x21]\n\t"
		"mov x22, %[in_x22]\n\t"
		"mov x23, %[in_x23]\n\t"
		"mov x0, %[uc_hva]\n\t"
		"mov x1, %[mmio_gva]\n\t"
		"str x0, [x1]\n\t"
		"mov %[out_x19], x19\n\t"
		"mov %[out_x20], x20\n\t"
		"mov %[out_x21], x21\n\t"
		"mov %[out_x22], x22\n\t"
		"mov %[out_x23], x23\n\t"
		"fmov %[out_d8], d8\n\t"
		: [out_x19] "=r" (out_x19),
		  [out_x20] "=r" (out_x20),
		  [out_x21] "=r" (out_x21),
		  [out_x22] "=r" (out_x22),
		  [out_x23] "=r" (out_x23),
		  [out_d8] "=r" (out_d8)
		: [in_x19] "r" (in_x19),
		  [in_x20] "r" (in_x20),
		  [in_x21] "r" (in_x21),
		  [in_x22] "r" (in_x22),
		  [in_x23] "r" (in_x23),
		  [in_d8] "r" (in_d8),
		  [uc_hva] "r" (uc_hva),
		  [mmio_gva] "r" (mmio_gva)
		: "x0", "x1", "x19", "x20", "x21", "x22", "x23", "v8", "memory"
	);

	__GUEST_ASSERT(out_x19 == in_x19, "vCPU %u X19 mismatch: 0x%lx != 0x%lx",
		       vcpu_id, out_x19, in_x19);
	__GUEST_ASSERT(out_x20 == in_x20, "vCPU %u X20 mismatch: 0x%lx != 0x%lx",
		       vcpu_id, out_x20, in_x20);
	__GUEST_ASSERT(out_x21 == in_x21, "vCPU %u X21 mismatch: 0x%lx != 0x%lx",
		       vcpu_id, out_x21, in_x21);
	__GUEST_ASSERT(out_x22 == in_x22, "vCPU %u X22 mismatch: 0x%lx != 0x%lx",
		       vcpu_id, out_x22, in_x22);
	__GUEST_ASSERT(out_x23 == in_x23, "vCPU %u X23 mismatch: 0x%lx != 0x%lx",
		       vcpu_id, out_x23, in_x23);
	__GUEST_ASSERT(out_d8 == in_d8, "vCPU %u D8 mismatch: 0x%lx != 0x%lx",
		       vcpu_id, out_d8, in_d8);

	guest_verify_arch_sysregs_and_timer(sh, vcpu_id);
}

static void host_verify_vcpu_state(struct kvm_vcpu *vcpu, uint32_t id)
{
	struct kvm_vcpu_events events;
	struct kvm_mp_state mp_state;
	__uint128_t v8_val = 0;
	uint64_t fp_reg_id;
	uint64_t ctl;

	TEST_ASSERT_EQ(vcpu_get_reg(vcpu, ARM64_CORE_REG(regs.regs[19])), exp_x19(id));
	TEST_ASSERT_EQ(vcpu_get_reg(vcpu, ARM64_CORE_REG(regs.regs[20])), exp_x20(id));
	TEST_ASSERT_EQ(vcpu_get_reg(vcpu, ARM64_CORE_REG(regs.regs[21])), exp_x21(id));
	TEST_ASSERT_EQ(vcpu_get_reg(vcpu, ARM64_CORE_REG(regs.regs[22])), exp_x22(id));
	TEST_ASSERT_EQ(vcpu_get_reg(vcpu, ARM64_CORE_REG(regs.regs[23])), exp_x23(id));

	vcpu_mp_state_get(vcpu, &mp_state);
	TEST_ASSERT_EQ(mp_state.mp_state, KVM_MP_STATE_RUNNABLE);

	vcpu_events_get(vcpu, &events);
	TEST_ASSERT_EQ(events.exception.serror_pending, 0);
	TEST_ASSERT_EQ(events.exception.ext_dabt_pending, 0);

	TEST_ASSERT(vcpu_get_reg(vcpu, KVM_REG_ARM_PSCI_VERSION) != 0,
		    "Unexpected PSCI version on vCPU %u", id);

	TEST_ASSERT_EQ(vcpu_get_reg(vcpu, KVM_ARM64_SYS_REG(SYS_TPIDR_EL0)),
		       exp_tpidr_el0(id));
	TEST_ASSERT_EQ(vcpu_get_reg(vcpu, KVM_ARM64_SYS_REG(SYS_TPIDRRO_EL0)),
		       exp_tpidrro_el0(id));
	TEST_ASSERT_EQ(vcpu_get_reg(vcpu, KVM_ARM64_SYS_REG(SYS_CONTEXTIDR_EL1)),
		       exp_contextidr_el1(id));

	fp_reg_id = KVM_REG_ARM64 | KVM_REG_SIZE_U128 | KVM_REG_ARM_CORE |
		    KVM_REG_ARM_CORE_REG(fp_regs.vregs[8]);
	TEST_ASSERT_EQ(__vcpu_get_reg(vcpu, fp_reg_id, &v8_val), 0);
	TEST_ASSERT_EQ((uint64_t)v8_val, exp_d8(id));

	TEST_ASSERT_EQ(vcpu_get_reg(vcpu, KVM_REG_ARM_TIMER_CVAL),
		       exp_cntv_cval(id));
	ctl = vcpu_get_reg(vcpu, KVM_REG_ARM_TIMER_CTL) & (CTL_ENABLE | CTL_IMASK);
	TEST_ASSERT_EQ(ctl, EXP_CNTV_CTL);
}

#endif

static void guest_vcpu_code(uint32_t vcpu_id, uintptr_t sh_gva)
{
	struct vcpu_test_shared *sh = (struct vcpu_test_shared *)sh_gva;
	size_t chunk = DATA_PATTERN_SIZE / NR_TEST_VCPUS;
	size_t base = vcpu_id * chunk;
	int iter;

	/* Initial sync using standard ucall_pool so host can record ucalls_offset. */
	GUEST_SYNC(0);

	for (size_t i = 0; i < chunk; i++)
		sh->data_pattern[base + i] = pattern_byte(vcpu_id, base + i);

	guest_setup_arch_state(sh, vcpu_id);

	/*
	 * Sync stages 1..NR_CANCEL_ITERS are resumed in Stage 1 after each
	 * preserve/cancel iteration; stage (1 + NR_CANCEL_ITERS) is preserved
	 * across kexec and resumed in Stage 2!
	 */
	for (iter = 0; iter <= NR_CANCEL_ITERS; iter++)
		guest_sync_with_regs(sh, vcpu_id, 1 + iter);

	/* Resumed in Stage 2 (post-kexec): verify full memfd data pattern. */
	for (uint32_t v = 0; v < NR_TEST_VCPUS; v++) {
		size_t vbase = v * chunk;

		for (size_t i = 0; i < chunk; i++) {
			uint8_t got = sh->data_pattern[vbase + i];
			uint8_t exp = pattern_byte(v, vbase + i);

			__GUEST_ASSERT(got == exp,
				       "Data mismatch at %lu: got 0x%x, exp 0x%x",
				       vbase + i, got, exp);
		}
	}

#ifdef __aarch64__
	if (sh->has_gic) {
		WRITE_ONCE(guest_irq_count[vcpu_id], 0);
		write_sysreg(0, cntv_tval_el0);
		write_sysreg(CTL_ENABLE, cntv_ctl_el0);
		isb();
		local_irq_enable();
		while (!READ_ONCE(guest_irq_count[vcpu_id]))
			cpu_relax();
		local_irq_disable();
		WRITE_ONCE(sh->irq_count[vcpu_id], READ_ONCE(guest_irq_count[vcpu_id]));
	}
#endif

	WRITE_ONCE(sh->stage2_verified[vcpu_id], 1);
	GUEST_DONE();
}

static void relocate_ucall_hvas(void *slot0_hva, const struct vcpu_test_meta *meta,
				struct vcpu_test_shared *sh)
{
	struct ucall *pool_ucalls;
	int i;

	for (i = 0; i < NR_TEST_VCPUS; i++)
		sh->uc[i].hva = &sh->uc[i];

	if (meta->ucalls_offset &&
	    meta->ucalls_offset + sizeof(struct ucall) * KVM_MAX_VCPUS <= meta->slot0_size) {
		pool_ucalls = (struct ucall *)((uintptr_t)slot0_hva + meta->ucalls_offset);
		for (i = 0; i < KVM_MAX_VCPUS; i++)
			pool_ucalls[i].hva = &pool_ucalls[i];
	}
}

static struct kvm_vm *create_vm_with_shared_slot0(struct kvm_vcpu *vcpus[],
						  int *out_memfd,
						  struct vcpu_test_meta **out_meta,
						  struct vcpu_test_shared **out_sh)
{
	struct userspace_mem_region *slot0;
	struct vcpu_test_shared *sh;
	struct vcpu_test_meta *meta;
	struct kvm_vm *vm;
	gva_t sh_gva;
	int i;

	vm = ____vm_create(VM_SHAPE_DEFAULT);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_SHMEM, 0, 0, NR_SLOT0_PAGES, 0);
	for (i = 0; i < NR_MEM_REGIONS; i++)
		vm->memslots[i] = 0;

	kvm_vm_elf_load(vm, program_invocation_name);

	slot0 = memslot2region(vm, 0);
	ucall_init(vm, slot0->region.guest_phys_addr + slot0->region.memory_size);
	kvm_arch_vm_post_create(vm, NR_TEST_VCPUS);

	sh_gva = vm_alloc_pages(vm, 2);
	sh = addr_gva2hva(vm, sh_gva);
	memset(sh, 0, sizeof(*sh));
	sh->magic = VCPU_TEST_MAGIC;
	for (i = 0; i < NR_TEST_VCPUS; i++)
		sh->uc[i].hva = &sh->uc[i];

	meta = (struct vcpu_test_meta *)slot0->host_mem;
	memset(meta, 0, sizeof(*meta));
	meta->magic = VCPU_TEST_MAGIC;
	meta->slot0_hva = (uintptr_t)slot0->host_mem;
	meta->slot0_size = slot0->region.memory_size;
	meta->ucall_mmio_gpa = slot0->region.guest_phys_addr + slot0->region.memory_size;
	meta->sh_gpa = (uintptr_t)sh - (uintptr_t)slot0->host_mem;
#ifdef __aarch64__
	meta->has_gic = vm->arch.has_gic ? 1 : 0;
	sh->has_gic = meta->has_gic;
#endif

	for (i = 0; i < NR_TEST_VCPUS; i++) {
		vcpus[i] = vm_vcpu_add(vm, i, guest_vcpu_code);
		vcpu_args_set(vcpus[i], 2, i, sh_gva);
	}
#ifdef __aarch64__
	vm_init_descriptor_tables(vm);
	vm_install_exception_handler(vm, VECTOR_IRQ_CURRENT, guest_irq_handler);
	for (i = 0; i < NR_TEST_VCPUS; i++)
		vcpu_init_descriptor_tables(vcpus[i]);
#endif
	kvm_arch_vm_finalize_vcpus(vm);

	*out_memfd = slot0->fd;
	*out_meta = meta;
	*out_sh = sh;
	return vm;
}

static void run_stage_1(int luo_fd)
{
	struct kvm_vcpu *vcpus[NR_TEST_VCPUS];
	struct vcpu_test_shared *sh;
	struct vcpu_test_meta *meta;
	int memfd, session_fd, i, iter;
	struct kvm_vm *vm;
	struct ucall uc;

	ksft_print_msg("[STAGE 1] Creating VM with memfd-backed Slot 0 and %d vCPUs...\n",
		       NR_TEST_VCPUS);

	vm = create_vm_with_shared_slot0(vcpus, &memfd, &meta, &sh);

	/* Step each vCPU to GUEST_SYNC(0) and record ucalls_offset from vCPU 0. */
	for (i = 0; i < NR_TEST_VCPUS; i++) {
		vcpu_run(vcpus[i]);
		if (i == 0) {
			void *uc0 = ucall_arch_get_ucall(vcpus[0]);

			TEST_ASSERT(uc0 != NULL, "Failed to get ucall0 HVA");
			meta->ucalls_offset = (uintptr_t)uc0 - meta->slot0_hva;
		}
		TEST_ASSERT_EQ(get_ucall(vcpus[i], &uc), UCALL_SYNC);
		TEST_ASSERT_EQ(uc.args[1], 0);
	}

	/* Advance each vCPU to sync stage 1 with known architectural state loaded. */
	for (i = 0; i < NR_TEST_VCPUS; i++) {
		vcpu_run(vcpus[i]);
		TEST_ASSERT_EQ(get_ucall(vcpus[i], &uc), UCALL_SYNC);
		TEST_ASSERT_EQ(uc.args[0], 1);
		host_verify_vcpu_state(vcpus[i], i);
	}

	/*
	 * Subtest 1: Preserve / cancel loop without kexec (NR_CANCEL_ITERS iterations).
	 */
	ksft_print_msg("[STAGE 1] Running %d-iteration preserve/cancel loop without kexec...\n",
		       NR_CANCEL_ITERS);
	for (iter = 0; iter < NR_CANCEL_ITERS; iter++) {
		int cancel_fd = luo_create_session(luo_fd, CANCEL_SESSION_NAME);

		TEST_ASSERT(cancel_fd >= 0, "Failed to create cancel session");
		TEST_ASSERT_EQ(luo_session_preserve_fd(cancel_fd, vm->fd, VM_TOKEN), 0);
		TEST_ASSERT_EQ(luo_session_preserve_fd(cancel_fd, memfd, MEMFD_TOKEN), 0);
		for (i = 0; i < NR_TEST_VCPUS; i++) {
			struct kvm_mp_state mp_state;
			int r;

			TEST_ASSERT_EQ(luo_session_preserve_fd(cancel_fd, vcpus[i]->fd,
							       VCPU_TOKEN_BASE + i), 0);
			r = __vcpu_ioctl(vcpus[i], KVM_GET_MP_STATE, &mp_state);
			TEST_ASSERT(r == -1 && errno == EBUSY,
				    "Expected EBUSY on preserved vCPU ioctl, got r=%d errno=%d",
				    r, errno);
		}

		/* Cancel the session by closing cancel_fd without kexec. */
		close(cancel_fd);

		/* Verify state is still intact on host and resume each vCPU to next sync. */
		for (i = 0; i < NR_TEST_VCPUS; i++) {
			host_verify_vcpu_state(vcpus[i], i);
			vcpu_run(vcpus[i]);
			TEST_ASSERT_EQ(get_ucall(vcpus[i], &uc), UCALL_SYNC);
			TEST_ASSERT_EQ(uc.args[0], 2 + iter);
			host_verify_vcpu_state(vcpus[i], i);
		}
		ksft_print_msg("[STAGE 1] Preserve/cancel iteration %d/%d PASSED\n",
			       iter + 1, NR_CANCEL_ITERS);
	}

	/*
	 * Subtest 2: Preserve VM, memfd, and vCPUs across kexec.
	 */
	ksft_print_msg("[STAGE 1] Creating state file and preserving VM, memfd, and vCPUs for kexec...\n");
	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_TOKEN, 2);

	session_fd = luo_create_session(luo_fd, SESSION_NAME);
	TEST_ASSERT(session_fd >= 0, "Failed to create LUO session");

	TEST_ASSERT_EQ(luo_session_preserve_fd(session_fd, vm->fd, VM_TOKEN), 0);
	TEST_ASSERT_EQ(luo_session_preserve_fd(session_fd, memfd, MEMFD_TOKEN), 0);
	for (i = 0; i < NR_TEST_VCPUS; i++) {
		TEST_ASSERT_EQ(luo_session_preserve_fd(session_fd, vcpus[i]->fd,
						       VCPU_TOKEN_BASE + i), 0);
	}

	ksft_print_msg("[STAGE 1] Preservation complete; daemonizing for kexec...\n");
	close(luo_fd);
	daemonize_and_wait();
}

static void setup_retrieved_irqchip(struct kvm_vm *vm, const struct vcpu_test_meta *meta)
{
#ifdef __x86_64__
	vm_create_irqchip(vm);
#elif defined(__aarch64__)
	if (meta->has_gic) {
		uint32_t nr_irqs = 64;
		uint64_t attr;
		int gic_fd;

		gic_fd = __kvm_create_device(vm, KVM_DEV_TYPE_ARM_VGIC_V3);
		TEST_ASSERT(gic_fd >= 0, "Failed to create VGICv3 on retrieved VM");

		kvm_device_attr_set(gic_fd, KVM_DEV_ARM_VGIC_GRP_NR_IRQS, 0, &nr_irqs);
		attr = GICD_BASE_GPA;
		kvm_device_attr_set(gic_fd, KVM_DEV_ARM_VGIC_GRP_ADDR,
				    KVM_VGIC_V3_ADDR_TYPE_DIST, &attr);
		attr = REDIST_REGION_ATTR_ADDR(NR_TEST_VCPUS, GICR_BASE_GPA, 0, 0);
		kvm_device_attr_set(gic_fd, KVM_DEV_ARM_VGIC_GRP_ADDR,
				    KVM_VGIC_V3_ADDR_TYPE_REDIST_REGION, &attr);

		vm->arch.gic_fd = gic_fd;
		vm->arch.has_gic = true;
	}
#endif
}

static void run_stage_2(int luo_fd, int state_session_fd)
{
	int retrieved_vm_fd, retrieved_memfd, vcpu_fds[NR_TEST_VCPUS];
	struct kvm_vcpu *vcpus[NR_TEST_VCPUS];
	struct vcpu_test_meta meta_snap, *meta;
	struct vcpu_test_shared *sh;
	int session_fd, stage, i;
	struct kvm_vm *vm;
	void *slot0_hva;

	ksft_print_msg("[STAGE 2] Starting post-kexec vCPU preservation verification...\n");

	restore_and_read_stage(state_session_fd, STATE_TOKEN, &stage);
	TEST_ASSERT_EQ(stage, 2);

	session_fd = luo_retrieve_session(luo_fd, SESSION_NAME);
	TEST_ASSERT(session_fd >= 0, "Failed to retrieve LUO session '%s'", SESSION_NAME);

	retrieved_vm_fd = luo_session_retrieve_fd(session_fd, VM_TOKEN);
	TEST_ASSERT(retrieved_vm_fd >= 0, "Failed to retrieve VM fd");

	retrieved_memfd = luo_session_retrieve_fd(session_fd, MEMFD_TOKEN);
	TEST_ASSERT(retrieved_memfd >= 0, "Failed to retrieve Slot 0 memfd");

	/* Read Page 0 metadata from retrieved_memfd to get Stage-1 HVA and size. */
	TEST_ASSERT_EQ(pread(retrieved_memfd, &meta_snap, sizeof(meta_snap), 0),
		       (ssize_t)sizeof(meta_snap));
	TEST_ASSERT_EQ(meta_snap.magic, VCPU_TEST_MAGIC);

	slot0_hva = mmap((void *)(uintptr_t)meta_snap.slot0_hva, meta_snap.slot0_size,
			 PROT_READ | PROT_WRITE, MAP_SHARED, retrieved_memfd, 0);
	TEST_ASSERT(slot0_hva != MAP_FAILED, "Failed to mmap retrieved Slot 0 memfd");

	meta = (struct vcpu_test_meta *)slot0_hva;
	sh = (struct vcpu_test_shared *)((uintptr_t)slot0_hva + meta->sh_gpa);
	TEST_ASSERT_EQ(sh->magic, VCPU_TEST_MAGIC);
	relocate_ucall_hvas(slot0_hva, meta, sh);

	vm = vm_create_from_fd(retrieved_vm_fd, VM_SHAPE_DEFAULT);
	vm->ucall_mmio_addr = meta->ucall_mmio_gpa;
	vm_set_user_memory_region(vm, 0, 0, 0, meta->slot0_size, slot0_hva);
	setup_retrieved_irqchip(vm, meta);

	for (i = 0; i < NR_TEST_VCPUS; i++) {
		vcpu_fds[i] = luo_session_retrieve_fd(session_fd, VCPU_TOKEN_BASE + i);
		TEST_ASSERT(vcpu_fds[i] >= 0, "Failed to retrieve vCPU %d fd", i);
		vcpus[i] = vm_vcpu_add_from_fd(vm, i, vcpu_fds[i]);
	}
	kvm_arch_vm_finalize_vcpus(vm);

	ksft_print_msg("[STAGE 2] Verifying preserved vCPU state from host before KVM_RUN...\n");
	for (i = 0; i < NR_TEST_VCPUS; i++) {
		TEST_ASSERT_EQ(sh->sync_stage[i], 1 + NR_CANCEL_ITERS);
		host_verify_vcpu_state(vcpus[i], i);
	}
	ksft_print_msg("[STAGE 2] Host-side register, MSR/sysreg, FPU/SIMD, and timer checks PASSED\n");

	ksft_print_msg("[STAGE 2] Resuming vCPUs in guest mode to verify in-guest state...\n");
	for (i = 0; i < NR_TEST_VCPUS; i++) {
		vcpu_run(vcpus[i]);
		TEST_ASSERT_EQ(get_ucall(vcpus[i], NULL), UCALL_DONE);
		TEST_ASSERT_EQ(READ_ONCE(sh->stage2_verified[i]), 1);
	}
	ksft_print_msg("[STAGE 2] In-guest state and memfd data verification PASSED for all %d vCPUs\n",
		       NR_TEST_VCPUS);

	TEST_ASSERT_EQ(luo_session_finish(session_fd), 0);
	close(session_fd);

	TEST_ASSERT_EQ(luo_session_finish(state_session_fd), 0);
	close(state_session_fd);

	kvm_vm_free(vm);
	munmap(slot0_hva, meta_snap.slot0_size);
	close(retrieved_memfd);
}

int main(int argc, char *argv[])
{
#ifdef __aarch64__
	/* vCPU preservation does not support nested virtualization (EL2). */
	setenv("NV", "0", 1);
#endif
	return luo_test(argc, argv, STATE_SESSION_NAME, run_stage_1, run_stage_2);
}
