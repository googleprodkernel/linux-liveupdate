// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Preserved-CPU runtime for ARM64.
 */
#include <linux/arm-smccc.h>
#include <linux/cpu_preserve.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/pgtable.h>
#include <linux/psci.h>
#include <linux/sizes.h>
#include <uapi/linux/psci.h>

#include <asm/barrier.h>
#include <asm/cputype.h>
#include <asm/daifflags.h>
#include <asm/processor.h>
#include <asm/sysreg.h>
#include <asm/tlbflush.h>

#include "cpu_preserve_internal.h"

#define CPUPR_RWP_TIMEOUT_COUNT		1000000
#define CPUPR_SGI_MASK			GENMASK(15, 0)
#define CPUPR_HYP_TIMER_PPI		26
#define CPUPR_HYP_VIRT_TIMER_PPI	30
#define GICR_INT_PRIORITY(intid)	(GICR_IPRIORITYR0 + (intid))

#define MPIDR_TO_SGI_AFFINITY(cluster_id, level) \
	(MPIDR_AFFINITY_LEVEL(cluster_id, level) \
		<< ICC_SGI1R_AFFINITY_## level ##_SHIFT)
#define MPIDR_TO_SGI_CLUSTER_ID(mpidr)	((mpidr) & ~0xFUL)
#define MPIDR_RS(mpidr)			(((mpidr) & 0xf0ULL) >> 4)
#define MPIDR_TO_SGI_RS(mpidr)		(MPIDR_RS(mpidr) << ICC_SGI1R_RS_SHIFT)

/*
 * The partial link moves .data..ro_after_init to .preserved.rodata: the
 * host writes these before any CPU parks, the preserved CPUs map them
 * read-only.
 */
enum arm_smccc_conduit arm64_psci_conduit __ro_after_init;
struct cpupr_gic_state cpupr_gic __ro_after_init;

/*
 * Low-power wait in parking loop.
 */
void arch_cpupr_park_wait(void)
{
	wfe();
}

void __iomem *gicv3_get_rdist_for_mpidr(u64 mpidr)
{
	u32 cpu_aff = (MPIDR_AFFINITY_LEVEL(mpidr, 3) << 24) |
		      (MPIDR_AFFINITY_LEVEL(mpidr, 2) << 16) |
		      (MPIDR_AFFINITY_LEVEL(mpidr, 1) << 8) |
		      MPIDR_AFFINITY_LEVEL(mpidr, 0);
	int i;

	for (i = 0; i < cpupr_gic.nr_regions; i++) {
		void __iomem *va = cpupr_gic.regions[i].va;
		size_t map_size = cpupr_gic.regions[i].size;
		u64 stride = cpupr_gic.regions[i].stride;
		void __iomem *ptr = va;

		if (!va)
			continue;

		do {
			u64 typer = __raw_readq(ptr + GICR_TYPER);
			u32 aff = typer >> 32;
			bool last = !!(typer & GICR_TYPER_LAST);

			if (aff == cpu_aff)
				return ptr;

			if (stride) {
				ptr += stride;
			} else {
				ptr += SZ_64K * 2;
				if (typer & GICR_TYPER_VLPIS)
					ptr += SZ_64K * 2;
			}
			if (last)
				break;
		} while ((ptr - va) < map_size);
	}
	return NULL;
}

static __always_inline void
gicv3_cpupr_wait_for_rwp(void __iomem *base, u32 bit)
{
	int count = CPUPR_RWP_TIMEOUT_COUNT;

	while (count-- > 0) {
		if (!(__raw_readl(base + GICR_CTLR) & bit))
			return;
		cpu_relax();
	}
}

void gicv3_cpupr_clear_active_priorities(void)
{
	u32 ctlr = read_sysreg_s(SYS_ICC_CTLR_EL1);
	u32 pribits = ((ctlr & ICC_CTLR_EL1_PRI_BITS_MASK) >>
		       ICC_CTLR_EL1_PRI_BITS_SHIFT) + 1;

	switch (pribits) {
	case 8:
	case 7:
		write_sysreg_s(0, SYS_ICC_AP1R3_EL1);
		write_sysreg_s(0, SYS_ICC_AP1R2_EL1);
		fallthrough;
	case 6:
		write_sysreg_s(0, SYS_ICC_AP1R1_EL1);
		fallthrough;
	case 5:
	case 4:
	default:
		write_sysreg_s(0, SYS_ICC_AP1R0_EL1);
		break;
	}
	isb();
}

void gicv3_cpupr_enable_sgi(void)
{
	void __iomem *ptr = gicv3_get_rdist_for_mpidr(read_sysreg(mpidr_el1));

	if (ptr) {
		void __iomem *rbase = ptr + SZ_64K;

		__raw_writel(~0U, rbase + GICR_IGROUPR0);
		__raw_writel(0, rbase + GICR_IGRPMODR0);
		__raw_writeb(0x00, rbase + GICR_INT_PRIORITY(0));
		__raw_writeb(0x00, rbase + GICR_INT_PRIORITY(CPUPR_HYP_TIMER_PPI));
		__raw_writeb(0x00, rbase + GICR_INT_PRIORITY(CPUPR_HYP_VIRT_TIMER_PPI));
		__raw_writel(CPUPR_SGI_MASK |
			     BIT(CPUPR_HYP_TIMER_PPI) |
			     BIT(CPUPR_HYP_VIRT_TIMER_PPI),
			     rbase + GICR_ISENABLER0);
		__raw_writel(~0U, rbase + GICR_ICACTIVER0);
		gicv3_cpupr_wait_for_rwp(ptr, GICR_CTLR_RWP);
	}

	write_sysreg_s(0, SYS_ICC_BPR1_EL1);
	gicv3_cpupr_clear_active_priorities();
}

void gicv3_cpupr_kick_mpidr(u64 mpidr)
{
	u64 cluster_id, sgi1r;
	u16 tlist;

	if (mpidr == INVALID_HWID)
		return;
	if ((mpidr & MPIDR_HWID_BITMASK) == (read_sysreg(mpidr_el1) & MPIDR_HWID_BITMASK))
		return;

	cluster_id = MPIDR_TO_SGI_CLUSTER_ID(mpidr);
	tlist = 1 << (mpidr & 0xf);

	dsb(ishst);
	sgi1r = (MPIDR_TO_SGI_AFFINITY(cluster_id, 3) |
		 MPIDR_TO_SGI_AFFINITY(cluster_id, 2) |
		 (0ULL << ICC_SGI1R_SGI_ID_SHIFT) |
		 MPIDR_TO_SGI_AFFINITY(cluster_id, 1) |
		 MPIDR_TO_SGI_RS(cluster_id) |
		 ((u64)tlist << ICC_SGI1R_TARGET_LIST_SHIFT));
	write_sysreg_s(sgi1r, SYS_ICC_SGI1R_EL1);
	isb();
}

static __always_inline u64 arm64_pgd_to_ttbr1(phys_addr_t pgd_pa)
{
	u64 ttbr = phys_to_ttbr(pgd_pa);

#if defined(CONFIG_ARM64_VA_BITS_52) && !defined(CONFIG_ARM64_LPA2)
	if ((read_sysreg(tcr_el1) & TCR_EL1_T1SZ_MASK) == TCR_T1SZ(VA_BITS_MIN))
		ttbr |= TTBR1_BADDR_4852_OFFSET;
#endif
	return ttbr;
}

void arch_cpupr_switch_pgd(phys_addr_t pgd_pa)
{
	if (pgd_pa) {
		write_sysreg(arm64_pgd_to_ttbr1(pgd_pa), ttbr1_el1);
		isb();
		arm64_flush_host_tlb_local();
	}
}

/*
 * Masks DAIF interrupts and enables GIC CPU interface for WFx wakeups.
 */
void arch_cpupr_park_init(int cpu)
{
	struct cpupr_stack_context *sctx = cpupr_get_stack_context();

	if (!sctx || !sctx->session_pgd_pa)
		return;

	local_daif_mask();
	write_sysreg((unsigned long)arm64_preserved_vectors, vbar_el1);
	isb();

	if ((read_sysreg(ttbr1_el1) & TTBRx_EL1_BADDR) !=
	    arm64_pgd_to_ttbr1(sctx->session_pgd_pa))
		gicv3_cpupr_enable_sgi();

	sysreg_clear_set(tcr_el1, 0, TCR_EPD0_MASK);
	write_sysreg(0, ttbr0_el1);
	arch_cpupr_switch_pgd(sctx->session_pgd_pa);
	asm volatile("msr daifclr, #4" ::: "memory");

	write_sysreg_s(ICC_CTLR_EL1_EOImode_drop, SYS_ICC_CTLR_EL1);
	write_sysreg_s(ICC_SRE_EL1_SRE, SYS_ICC_SRE_EL1);
	write_sysreg_s(0, SYS_ICC_BPR1_EL1);
	gicv3_cpupr_clear_active_priorities();
	write_sysreg_s(ICC_PMR_EL1_MASK, SYS_ICC_PMR_EL1);
	write_sysreg_s(ICC_IGRPEN1_EL1_MASK, SYS_ICC_IGRPEN1_EL1);
	isb();
}

static void arm64_preserved_cpu_quiesce(void)
{
	local_daif_mask();
	gicv3_cpupr_clear_active_priorities();
	write_sysreg_s(0, SYS_ICC_IGRPEN1_EL1);
	write_sysreg_s(0, SYS_ICC_PMR_EL1);
	write_sysreg_s(0, SYS_ICC_BPR1_EL1);
	arm64_flush_host_tlb_local();
}

static void __noreturn arm64_preserved_cpu_off(void)
{
	u32 state = PSCI_POWER_STATE_TYPE_POWER_DOWN <<
		    PSCI_0_2_POWER_STATE_TYPE_SHIFT;

	switch (READ_ONCE(arm64_psci_conduit)) {
	case SMCCC_CONDUIT_HVC:
		arm_smccc_1_1_hvc(PSCI_0_2_FN_CPU_OFF, state, NULL);
		break;
	case SMCCC_CONDUIT_SMC:
		arm_smccc_1_1_smc(PSCI_0_2_FN_CPU_OFF, state, NULL);
		break;
	default:
		break;
	}

	if (read_sysreg(tcr_el1) & TCR_EPD0_MASK) {
		struct cpupr_stack_context *sctx =
			cpupr_get_stack_context();

		if (sctx && sctx->ser) {
			/* Pairs with the acquire in cpupr_read_state() */
			smp_store_release(&sctx->ser->state,
					  CPUPR_FAULTED);
			cpupr_clean(sctx->ser);
		}
	}

	for (;;) {
		wfi();
		wfe();
	}
}

void arch_cpupr_park_finish(int cpu)
{
	arm64_preserved_cpu_quiesce();
	if (read_sysreg(tcr_el1) & TCR_EPD0_MASK)
		cpupr_set_dead();
	arm64_preserved_cpu_off();
}

asmlinkage unsigned long arm64_preserved_stack_top(unsigned long sp)
{
	return ALIGN_DOWN(sp, CPUPR_STACK_ALIGN) + CPUPR_STACK_SIZE;
}

/*
 * An unexpected exception leaves the CPU in an unknown state: record the first
 * one and turn the CPU off.  The host sees CPUPR_FAULTED and can bring
 * the CPU back online.
 */
asmlinkage void arm64_preserved_handle_exception(unsigned long kind)
{
	struct cpupr_stack_context *sctx = cpupr_get_stack_context();

	local_daif_mask();
	if (sctx && !sctx->fault.count++) {
		struct arm64_preserved_fault *f = &sctx->fault;

		f->kind = kind;
		f->esr = read_sysreg(esr_el1);
		f->elr = read_sysreg(elr_el1);
		f->far = read_sysreg(far_el1);
		f->spsr = read_sysreg(spsr_el1);
		cpupr_clean(f);

		arm64_preserved_cpu_quiesce();
		if (sctx->ser) {
			u32 old = READ_ONCE(sctx->ser->state);

			/* Pairs with the acquire in cpupr_read_state() */
			do {
				if (old == CPUPR_DEAD ||
				    old == CPUPR_FAULTED)
					break;
			} while (!try_cmpxchg_release(&sctx->ser->state, &old,
						      CPUPR_FAULTED));
			cpupr_clean(sctx->ser);
		}
		arm64_preserved_cpu_off();
	}

	for (;;)
		wfi();
}
