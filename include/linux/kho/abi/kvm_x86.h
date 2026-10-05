/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef _LINUX_KHO_ABI_KVM_X86_H
#define _LINUX_KHO_ABI_KVM_X86_H

#ifdef CONFIG_X86_64

#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/kho/abi/kvm.h>
#include <uapi/linux/kvm.h>
#include <uapi/asm/kvm.h>

/**
 * DOC: x86 KVM Live Update ABI
 *
 * x86 KVM uses the ABI defined below for preserving architectural VM and vCPU
 * state across a kexec reboot using LUO.
 *
 * The vCPU-level architectural state contains the compact register sets and
 * CPUID table for each vCPU.
 *
 * All sub-structures (struct kvm_regs, struct kvm_sregs2, struct kvm_mp_state,
 * struct kvm_xcrs, struct kvm_lapic_state, struct kvm_xsave,
 * struct kvm_vcpu_events, struct kvm_debugregs, struct kvm_clock_data,
 * struct kvm_msrs, and struct kvm_cpuid2) are uAPI contracts.
 */

#define KVM_X86_SER_MAX_MSRS	512

/**
 * struct kvm_vcpu_arch_ser - Preserved x86 architectural vCPU state in RAM.
 * @regs:          General-purpose registers (uAPI struct kvm_regs).
 * @sregs:         Segment, control, and PDPTR registers (uAPI struct kvm_sregs2).
 * @mp_state:      Multiprocessor state (uAPI struct kvm_mp_state).
 * @pad:           Padding to maintain 64-bit alignment after mp_state.
 * @xcrs:          Extended control registers including XCR0 (uAPI struct kvm_xcrs).
 * @lapic:         In-kernel local APIC register state (uAPI struct kvm_lapic_state).
 * @pad_xsave:     Padding that aligns @xsave to 64 bytes; see the static_assert
 *                 below.  Must be zero.
 * @xsave:         Extended processor state (uAPI struct kvm_xsave).  This is the
 *                 only copy of the guest FPU state; there is no kvm_fpu twin.
 * @events:        vCPU exception, interrupt, NMI and SMI events (uAPI struct kvm_vcpu_events).
 * @debugregs:     Hardware debug registers DR0-DR7 (uAPI struct kvm_debugregs).
 * @clock:         VM kvmclock state (uAPI struct kvm_clock_data).
 * @msrs:          Preservation pointer to architectural and paravirtual MSR
 *                 entries (uAPI struct kvm_msrs).
 * @cpuid:         Preservation pointer to guest CPUID table
 *                 (uAPI struct kvm_cpuid2).
 */
struct kvm_vcpu_arch_ser {
	struct kvm_regs regs;
	struct kvm_sregs2 sregs;
	struct kvm_mp_state mp_state;
	u32 pad;
	struct kvm_xcrs xcrs;
	struct kvm_lapic_state lapic;
	u8 pad_xsave[32];
	struct kvm_xsave xsave;
	struct kvm_vcpu_events events;
	struct kvm_debugregs debugregs;
	struct kvm_clock_data clock;
	DECLARE_KHOSER_PTR(msrs, struct kvm_msrs *);
	DECLARE_KHOSER_PTR(cpuid, struct kvm_cpuid2 *);
} __packed;

static_assert(offsetof(struct kvm_vcpu_arch_ser, msrs) % sizeof(u64) == 0,
	      "msrs pointer must be 64-bit aligned");

/*
 * @xsave must be a legal XSAVE destination.  The structure is __packed, so
 * without @pad_xsave the member lands at offset 1880, which is not even
 * 16-byte aligned, and the XSAVE/XRSTOR family faults on anything less than
 * 64.  The allocation itself is fine: kho_alloc_preserve() returns a folio
 * address, so the base is page aligned.
 *
 * If this assert fires, a member above @xsave changed size; adjust
 * @pad_xsave rather than deleting the assert.
 */
static_assert(offsetof(struct kvm_vcpu_arch_ser, xsave) % 64 == 0,
	      "xsave must be 64-byte aligned to be XSAVE-able in place");

#endif /* CONFIG_X86_64 */

#endif /* _LINUX_KHO_ABI_KVM_X86_H */
