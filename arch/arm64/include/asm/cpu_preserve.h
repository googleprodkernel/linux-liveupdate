/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef __ASM_ARM64_CPU_PRESERVE_H
#define __ASM_ARM64_CPU_PRESERVE_H

#include <asm/tlbflush.h>

asmlinkage void arch_cpupr_dcache_clean(unsigned long start, unsigned long end)
	__cpupr_sym_asm(arch_cpupr_dcache_clean);
#define arch_cpupr_dcache_clean arch_cpupr_dcache_clean

static __always_inline void arm64_flush_host_tlb_local(void)
{
	local_flush_tlb_all();
}

#ifdef CONFIG_LIVEUPDATE_CPU
void gicv3_cpupr_clear_active_priorities(void)
	__cpupr_sym_asm(gicv3_cpupr_clear_active_priorities);
void gicv3_cpupr_enable_sgi(void)
	__cpupr_sym_asm(gicv3_cpupr_enable_sgi);
void gicv3_cpupr_kick_mpidr(u64 mpidr)
	__cpupr_sym_asm(gicv3_cpupr_kick_mpidr);
#else
static inline void gicv3_cpupr_clear_active_priorities(void) {}
static inline void gicv3_cpupr_enable_sgi(void) {}
static inline void gicv3_cpupr_kick_mpidr(u64 mpidr) {}
#endif

#endif /* __ASM_ARM64_CPU_PRESERVE_H */
