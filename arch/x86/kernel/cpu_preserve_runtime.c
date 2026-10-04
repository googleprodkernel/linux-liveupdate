// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Preserved-CPU runtime for x86.
 */
#include <linux/bitfield.h>
#include <linux/cpu_preserve.h>

#include <asm/desc.h>
#include <asm/extable_handlers.h>
#include <asm/irqflags.h>
#include <asm/mce.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/special_insns.h>

#include "cpu_preserve_internal.h"

extern const struct exception_table_entry cpupr_sym(ex_table_start)[];
extern const struct exception_table_entry cpupr_sym(ex_table_end)[];

/*
 * The partial link moves .data..ro_after_init to .preserved.rodata: the
 * host writes these at boot, before the runtime is copied, and the preserved
 * CPUs map them read-only.
 */
gate_desc x86_preserved_idt[IDT_ENTRIES] __aligned(PAGE_SIZE) __ro_after_init;
struct desc_ptr x86_preserved_idt_desc __ro_after_init;
bool x86_preserved_mwait __ro_after_init;
u64 x86_preserved_sme_mask __ro_after_init;

static bool x86_preserved_fixup_exception(struct pt_regs *regs, int trapnr)
{
	const struct exception_table_entry *e;
	int type, reg, imm;

	for (e = cpupr_sym(ex_table_start);
	     e < cpupr_sym(ex_table_end); e++) {
		if (ex_insn_addr(e) != regs->ip)
			continue;

		type = FIELD_GET(EX_DATA_TYPE_MASK, e->data);
		reg  = FIELD_GET(EX_DATA_REG_MASK, e->data);
		imm  = FIELD_GET_SIGNED(EX_DATA_IMM_MASK, e->data);

		if (ex_fixup_basic(e, regs, type, trapnr, reg, imm))
			return true;

		switch (type) {
		case EX_TYPE_UACCESS:
			return ex_handler_default(e, regs);
		case EX_TYPE_CLEAR_FS:
			asm volatile("mov %0, %%fs" : : "rm" (0));
			return ex_handler_default(e, regs);
		case EX_TYPE_RDMSR:
			return ex_handler_msr_common(e, regs, false, false, reg);
		case EX_TYPE_WRMSR:
			return ex_handler_msr_common(e, regs, true, false, reg);
		case EX_TYPE_RDMSR_SAFE:
			return ex_handler_msr_common(e, regs, false, true, reg);
		case EX_TYPE_WRMSR_SAFE:
			return ex_handler_msr_common(e, regs, true, true, reg);
		default:
			return false;
		}
	}

	return false;
}

/*
 * Like mce_check_crashing_cpu() for an offline CPU: clear MCG_STATUS, so that
 * the next machine check does not find MCIP set and shut the platform down, and
 * resume if the interrupted context is still valid.
 */
static bool x86_preserved_handle_mce(struct cpupr_stack_context *sctx)
{
	u64 mcgstatus = native_rdmsrq(MSR_IA32_MCG_STATUS);

	native_wrmsrq(MSR_IA32_MCG_STATUS, 0);
	if (!(mcgstatus & MCG_STATUS_RIPV))
		return false;

	sctx->fault.nr_mce++;
	cpupr_clean(&sctx->fault);
	return true;
}

/*
 * An unexpected fault leaves the CPU in an unknown state: record the first one
 * and stop for good.  The host sees CPUPR_FAULTED and may reset the CPU.
 */
static void __noreturn x86_preserved_fault(struct cpupr_stack_context *sctx,
					   struct pt_regs *regs, int vector)
{
	if (!sctx->fault.count++) {
		struct x86_preserved_fault *f = &sctx->fault;

		f->vector = vector;
		f->error_code = regs->orig_ax;
		f->ip = regs->ip;
		f->sp = regs->sp;
		f->cr2 = native_read_cr2();
		f->cr3 = __native_read_cr3();
		cpupr_clean(f);

		if (sctx->ser) {
			/* Pairs with the acquire in cpupr_read_state() */
			smp_store_release(&sctx->ser->state, CPUPR_FAULTED);
			cpupr_clean(sctx->ser);
		}
	}

	for (;;) {
		native_irq_disable();
		asm volatile("hlt");
	}
}

/*
 * The NMI kick: wake up x86_preserved_idle(), also when the NMI arrives after
 * the kick flag was tested but before the CPU went to sleep.
 */
static void x86_preserved_handle_nmi(struct cpupr_stack_context *sctx,
				     struct pt_regs *regs)
{
	if (regs->ip >= (unsigned long)x86_preserved_idle_window &&
	    regs->ip <= (unsigned long)x86_preserved_idle_end)
		regs->ip = (unsigned long)x86_preserved_idle_end;
	else
		WRITE_ONCE(sctx->x86.kicked, 1);
}

asmlinkage void x86_preserved_handle_exception(struct pt_regs *regs, int vector)
{
	struct cpupr_stack_context *sctx = cpupr_get_stack_context();

	if (vector == X86_TRAP_NMI) {
		x86_preserved_handle_nmi(sctx, regs);
		return;
	}

	if (vector == X86_TRAP_MC && x86_preserved_handle_mce(sctx))
		return;

	if (vector != X86_TRAP_MC && vector != X86_TRAP_DF &&
	    x86_preserved_fixup_exception(regs, vector))
		return;

	x86_preserved_fault(sctx, regs, vector);
}

/*
 * Sleep until kicked.  MWAIT also wakes up on a store to ser->state, HLT only
 * on the NMI kick.
 */
void arch_cpupr_park_wait(void)
{
	struct cpupr_stack_context *sctx = cpupr_get_stack_context();
	u32 *monitor = NULL;
	u32 state = 0;

	if (sctx->ser && x86_preserved_mwait) {
		state = READ_ONCE(sctx->ser->state);
		if (state != CPUPR_PARKED && state != CPUPR_DEAD)
			return;
		monitor = &sctx->ser->state;
	}
	x86_preserved_idle(&sctx->x86.kicked, monitor, state);
}

/*
 * Only the kernel code and data segments and the TSS: IRET reloads CS and SS,
 * and the TSS provides the IST stacks.
 */
static void x86_preserved_init_desc(struct x86_preserved_cpu *x)
{
	tss_desc tss;
	int i;

	x->gdt[GDT_ENTRY_KERNEL_CS] =
		(struct desc_struct)GDT_ENTRY_INIT(DESC_CODE64, 0, 0xfffff);
	x->gdt[GDT_ENTRY_KERNEL_DS] =
		(struct desc_struct)GDT_ENTRY_INIT(DESC_DATA64, 0, 0xfffff);

	for (i = 0; i < X86_PRESERVED_NR_IST; i++)
		x->tss.ist[i] = (unsigned long)x->ist[i] + X86_PRESERVED_IST_SIZE;
	x->tss.io_bitmap_base = sizeof(x->tss);

	set_tssldt_descriptor(&tss, (unsigned long)&x->tss, DESC_TSS,
			      sizeof(x->tss) - 1);
	native_write_gdt_entry(x->gdt, GDT_ENTRY_TSS, &tss, DESC_TSS);
}

/*
 * Load the preserved GDT, TSS and IDT.  Exposed as a separate helper so
 * preserved workload runtimes can reload the preserved descriptors after a
 * context switch; clear the TSS busy bit first because LTR faults if the
 * descriptor was already loaded.
 */
void arch_cpupr_load_desc(void)
{
	struct cpupr_stack_context *sctx = cpupr_get_stack_context();
	struct desc_ptr gdt_desc;

	gdt_desc.size = sizeof(sctx->x86.gdt) - 1;
	gdt_desc.address = (unsigned long)sctx->x86.gdt;

	((struct ldttss_desc *)&sctx->x86.gdt[GDT_ENTRY_TSS])->type = DESC_TSS;
	native_load_gdt(&gdt_desc);
	asm volatile("ltr %w0" : : "q" (GDT_ENTRY_TSS * 8));
	native_load_idt(&x86_preserved_idt_desc);
}

void arch_cpupr_switch_pgd(phys_addr_t pgd_pa)
{
	unsigned long cr4;

	if (!pgd_pa)
		return;

	native_write_cr3(pgd_pa | x86_preserved_sme_mask);

	asm volatile("mov %%cr4, %0" : "=r" (cr4));
	if (cr4 & (X86_CR4_PGE | X86_CR4_CET)) {
		if (cr4 & X86_CR4_CET)
			native_wrmsrq(MSR_IA32_S_CET, 0);
		asm volatile("mov %0, %%cr4" : : "r" (cr4 & ~(X86_CR4_PGE | X86_CR4_CET)) : "memory");
	}
}

/*
 * Disables local interrupts on the physical core, loads the preserved GDT, TSS
 * and IDT, and switches to the session's isolated PGD.  The local APIC stays
 * software-disabled, as the CPU went offline: it still takes the NMI kick, but
 * no fixed interrupts.
 */
void arch_cpupr_park_init(int cpu)
{
	struct cpupr_stack_context *sctx = cpupr_get_stack_context();

	if (!sctx->session_pgd_pa)
		return;

	native_irq_disable();
	x86_preserved_init_desc(&sctx->x86);
	arch_cpupr_load_desc();
	arch_cpupr_switch_pgd(sctx->session_pgd_pa);
}

/*
 * Architecture cleanup on park loop exit.
 */
void arch_cpupr_park_finish(int cpu __maybe_unused)
{
}

void arch_cpupr_park_worker(int cpu)
{
	struct cpupr_stack_context *sctx = cpupr_get_stack_context();
	struct cpupr_ser *ser = sctx->ser;

	cpupr_park_loop(cpu);

	arch_cpupr_park_finish(cpu);
	native_irq_disable();
	WRITE_ONCE(sctx->x86.kicked, 0);
	cpupr_set_dead();
	for (;;) {
		u32 state;

		arch_cpupr_park_wait();
		state = ser ? smp_load_acquire(&ser->state) : 0;
		if (state >= PAGE_SIZE) {
			void (*start_ip)(void) = (void (*)(void))(uintptr_t)state;

			arch_cpupr_switch_pgd(__native_read_cr3());
			smp_store_release(&ser->state, CPUPR_DEAD);
			cpupr_clean(ser);
			start_ip();
		}
	}
}
