/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_EXTABLE_HANDLERS_H
#define _ASM_X86_EXTABLE_HANDLERS_H
/*
 * Exception fixup handlers for the simple fixup types, shared by
 * fixup_exception() and code that must resolve its own exception table
 * without calling into kernel text.
 */

#include <linux/bug.h>
#include <linux/errno.h>
#include <asm/extable.h>
#include <asm/insn-eval.h>
#include <asm/ptrace.h>

static __always_inline unsigned long *pt_regs_nr(struct pt_regs *regs, int nr)
{
	int reg_offset = pt_regs_offset(regs, nr);
	static unsigned long __dummy;

	if (WARN_ON_ONCE(reg_offset < 0))
		return &__dummy;

	return (unsigned long *)((unsigned long)regs + reg_offset);
}

static __always_inline bool
ex_handler_default(const struct exception_table_entry *e, struct pt_regs *regs)
{
	if (e->data & EX_FLAG_CLEAR_AX)
		regs->ax = 0;
	if (e->data & EX_FLAG_CLEAR_DX)
		regs->dx = 0;

	regs->ip = ex_fixup_addr(e);
	return true;
}

static __always_inline bool
ex_handler_fault(const struct exception_table_entry *fixup,
		 struct pt_regs *regs, int trapnr)
{
	regs->ax = trapnr;
	return ex_handler_default(fixup, regs);
}

static __always_inline bool
ex_handler_imm_reg(const struct exception_table_entry *fixup,
		   struct pt_regs *regs, int reg, int imm)
{
	*pt_regs_nr(regs, reg) = (long)imm;
	return ex_handler_default(fixup, regs);
}

static __always_inline bool
ex_handler_msr_common(const struct exception_table_entry *fixup,
		      struct pt_regs *regs, bool wrmsr, bool safe, int reg)
{
	if (!wrmsr) {
		/* Pretend that the read succeeded and returned 0. */
		regs->ax = 0;
		regs->dx = 0;
	}

	if (safe)
		*pt_regs_nr(regs, reg) = -EIO;

	return ex_handler_default(fixup, regs);
}

static __always_inline bool
ex_fixup_basic(const struct exception_table_entry *e, struct pt_regs *regs,
	       int type, int trapnr, int reg, int imm)
{
	switch (type) {
	case EX_TYPE_DEFAULT:
	case EX_TYPE_DEFAULT_MCE_SAFE:
		return ex_handler_default(e, regs);
	case EX_TYPE_FAULT:
	case EX_TYPE_FAULT_MCE_SAFE:
		return ex_handler_fault(e, regs, trapnr);
	case EX_TYPE_POP_REG:
		regs->sp += sizeof(long);
		fallthrough;
	case EX_TYPE_IMM_REG:
		return ex_handler_imm_reg(e, regs, reg, imm);
	default:
		return false;
	}
}

#endif /* _ASM_X86_EXTABLE_HANDLERS_H */
