/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ARCH_ARM64_KERNEL_CPU_PRESERVE_INTERNAL_H
#define _ARCH_ARM64_KERNEL_CPU_PRESERVE_INTERNAL_H

#include <linux/arm-smccc.h>
#include <linux/cpu_preserve.h>
#include <linux/types.h>

#define CPUPR_MAX_RDIST_REGIONS	8

struct cpupr_rdist_region {
	phys_addr_t	pa;
	void __iomem	*va;
	size_t		size;
	u64		stride;
};

struct cpupr_gic_state {
	struct cpupr_rdist_region	regions[CPUPR_MAX_RDIST_REGIONS];
	int				nr_regions;
};

extern enum arm_smccc_conduit arm64_psci_conduit
	__cpupr_sym_asm(arm64_psci_conduit);
extern struct cpupr_gic_state cpupr_gic
	__cpupr_sym_asm(cpupr_gic);

void __iomem *gicv3_get_rdist_for_mpidr(u64 mpidr)
	__cpupr_sym_asm(gicv3_get_rdist_for_mpidr);
void __arch_cpupr_park_on_stack(int cpu, unsigned long stack_top)
	__cpupr_sym_asm(__arch_cpupr_park_on_stack);

#endif /* _ARCH_ARM64_KERNEL_CPU_PRESERVE_INTERNAL_H */
