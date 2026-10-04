/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ARCH_X86_KERNEL_CPU_PRESERVE_INTERNAL_H
#define _ARCH_X86_KERNEL_CPU_PRESERVE_INTERNAL_H

#include <linux/cpu_preserve.h>
#include <linux/types.h>
#include <asm/desc_defs.h>
#include <asm/segment.h>

extern const char x86_preserved_exc_handler_array[NUM_EXCEPTION_VECTORS][EARLY_IDT_HANDLER_SIZE]
	__cpupr_sym_asm(x86_preserved_exc_handler_array);

extern bool x86_preserved_mwait __cpupr_sym_asm(x86_preserved_mwait);
extern u64 x86_preserved_sme_mask
	__cpupr_sym_asm(x86_preserved_sme_mask);
void x86_preserved_idle(u32 *kicked, u32 *monitor, u32 state)
	__cpupr_sym_asm(x86_preserved_idle);
extern const char x86_preserved_idle_window[]
	__cpupr_sym_asm(x86_preserved_idle_window);
extern const char x86_preserved_idle_end[]
	__cpupr_sym_asm(x86_preserved_idle_end);

struct pt_regs;
asmlinkage void x86_preserved_handle_exception(struct pt_regs *regs, int vector)
	__cpupr_sym_asm(x86_preserved_handle_exception);
void arch_cpupr_park_worker(int cpu)
	__cpupr_sym_asm(arch_cpupr_park_worker);
asmlinkage void arch_cpupr_call_on_stack(int cpu, unsigned long stack,
					 void (*fn)(int cpu))
	__cpupr_sym_asm(arch_cpupr_call_on_stack);

#endif /* _ARCH_X86_KERNEL_CPU_PRESERVE_INTERNAL_H */
