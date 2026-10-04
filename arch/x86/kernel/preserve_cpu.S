/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#include <linux/linkage.h>
#include <asm/nospec-branch.h>
#include <asm/segment.h>
#include <asm/trapnr.h>
#include <asm/unwind_hints.h>

	.text

SYM_CODE_START(x86_preserved_exc_handler_array)
	i = 0
	.rept NUM_EXCEPTION_VECTORS
	.if ((EXCEPTION_ERRCODE_MASK >> i) & 1) == 0
		UNWIND_HINT_IRET_REGS
		ENDBR
		pushq	$0	/* Dummy error code */
	.else
		UNWIND_HINT_IRET_REGS offset=8
		ENDBR
	.endif
	pushq	$i		/* Vector number */
	jmp	x86_preserved_exc_handler_common
	UNWIND_HINT_IRET_REGS
	i = i + 1
	.fill x86_preserved_exc_handler_array + i * EARLY_IDT_HANDLER_SIZE - ., 1, 0xcc
	.endr
SYM_CODE_END(x86_preserved_exc_handler_array)
	ANNOTATE_NOENDBR

SYM_CODE_START_LOCAL(x86_preserved_exc_handler_common)
	UNWIND_HINT_IRET_REGS offset=16
	cld

	/* The vector number is currently in the pt_regs->di slot. */
	pushq	%rsi
	movq	8(%rsp), %rsi		/* RSI = vector number */
	movq	%rdi, 8(%rsp)		/* pt_regs->di = RDI */
	pushq	%rdx
	pushq	%rcx
	pushq	%rax
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11
	pushq	%rbx
	pushq	%rbp
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	UNWIND_HINT_REGS

	movq	%rsp, %rdi		/* RDI = pt_regs; RSI = vector */
	call	x86_preserved_handle_exception

	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbp
	popq	%rbx
	popq	%r11
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rax
	popq	%rcx
	popq	%rdx
	popq	%rsi
	popq	%rdi
	addq	$8, %rsp		/* pop orig_ax / error_code */
	UNWIND_HINT_IRET_REGS
	iretq
SYM_CODE_END(x86_preserved_exc_handler_common)

/*
 * x86_preserved_idle - Sleep until kicked
 * @kicked: Kick flag (%rdi), set by the NMI handler
 * @monitor: Address for MONITOR/MWAIT (%rsi), or NULL to use HLT
 * @state: Expected value at *@monitor (%edx)
 *
 * Interrupts are disabled, so only an NMI (the kick), or a store to @monitor,
 * ends the sleep.  The NMI handler moves an NMI that hits between the test of
 * @kicked and the end of the sleep to x86_preserved_idle_end, so a kick is
 * never lost.
 */
SYM_FUNC_START(x86_preserved_idle)
	testq	%rsi, %rsi
	jz	1f
	movl	%edx, %r8d
	movq	%rsi, %rax
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	monitor
	cmpl	%r8d, (%rsi)
	jne	x86_preserved_idle_end
1:	xorl	%eax, %eax
	xchgl	%eax, (%rdi)
SYM_INNER_LABEL(x86_preserved_idle_window, SYM_L_GLOBAL)
	ANNOTATE_NOENDBR
	testl	%eax, %eax
	jnz	x86_preserved_idle_end
	testq	%rsi, %rsi
	jz	2f
	xorl	%ecx, %ecx
	mwait
	jmp	x86_preserved_idle_end
2:	hlt
SYM_INNER_LABEL(x86_preserved_idle_end, SYM_L_GLOBAL)
	ANNOTATE_NOENDBR
	RET
SYM_FUNC_END(x86_preserved_idle)

/*
 * arch_cpupr_call_on_stack - Switch stack and call function
 * @cpu: CPU ID (%rdi)
 * @stack: Target stack top (%rsi)
 * @fn: Function pointer to invoke (%rdx)
 */
SYM_CODE_START(arch_cpupr_call_on_stack)
	UNWIND_HINT_END_OF_STACK
	movq	%rsi, %rsp	/* switch to target stack */
	call	*%rdx		/* call fn(cpu), does not return */
	ud2
SYM_CODE_END(arch_cpupr_call_on_stack)
