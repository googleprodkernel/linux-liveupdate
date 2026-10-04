/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef __ASM_X86_CPU_PRESERVE_H
#define __ASM_X86_CPU_PRESERVE_H

#include <asm/desc_defs.h>
#include <asm/page_types.h>
#include <asm/processor.h>
#include <asm/segment.h>
#include <asm/trapnr.h>

#ifdef CONFIG_LIVEUPDATE_CPU
/**
 * struct x86_preserved_fault - Exceptions taken by a preserved CPU
 * @count:      Number of unexpected faults; the first one stopped the CPU.
 * @vector:     Exception vector of the first fault.
 * @error_code: Its error code.
 * @ip:         Its instruction pointer.
 * @sp:         Its stack pointer.
 * @cr2:        CR2 when it was taken.
 * @cr3:        CR3 when it was taken.
 * @nr_mce:     Number of machine checks the CPU recovered from.
 *
 * Private to the runtime, reported by arch_cpupr_wait_dead().
 */
struct x86_preserved_fault {
	unsigned long count;
	unsigned long vector;
	unsigned long error_code;
	unsigned long ip;
	unsigned long sp;
	unsigned long cr2;
	unsigned long cr3;
	unsigned long nr_mce;
};

enum {
	X86_PRESERVED_IST_DF,
	X86_PRESERVED_IST_NMI,
	X86_PRESERVED_IST_MC,
	X86_PRESERVED_NR_IST,
};

#define X86_PRESERVED_IST_SIZE	1024

/**
 * struct x86_preserved_cpu - Descriptor state of a preserved CPU
 * @gdt:     GDT with the kernel code and data segments and @tss.
 * @tss:     TSS, which only provides the IST stacks.
 * @kicked:  Set by the NMI kick, consumed by x86_preserved_idle().
 * @ist:     Stacks for #DF, NMI and #MC, so that these work on any stack.
 */
struct x86_preserved_cpu {
	struct desc_struct gdt[GDT_ENTRIES] __aligned(16);
	struct x86_hw_tss tss;
	u32 kicked;
	u8 ist[X86_PRESERVED_NR_IST][X86_PRESERVED_IST_SIZE] __aligned(16);
};

extern gate_desc x86_preserved_idt[IDT_ENTRIES]
	__cpupr_sym_asm(x86_preserved_idt);
extern struct desc_ptr x86_preserved_idt_desc
	__cpupr_sym_asm(x86_preserved_idt_desc);

void arch_cpupr_load_desc(void) __cpupr_sym_asm(arch_cpupr_load_desc);
int arch_cpupr_wakeup_ap(unsigned int cpu);
#else
static inline void arch_cpupr_load_desc(void) {}
static inline int arch_cpupr_wakeup_ap(unsigned int cpu) { return -ENODEV; }
#endif

#endif /* __ASM_X86_CPU_PRESERVE_H */
