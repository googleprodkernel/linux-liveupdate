/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Preserved CPU across Live Update
 */
#ifndef _LINUX_CPU_PRESERVE_H
#define _LINUX_CPU_PRESERVE_H

#include <linux/align.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/cpumask.h>
#include <linux/types.h>

#ifndef __CPUPR_RUNTIME__
#define cpupr_sym(sym)		__preserved_##sym
#define __cpupr_sym_asm(sym)	__asm__("__preserved_" #sym)
#else
#define cpupr_sym(sym)		sym
#define __cpupr_sym_asm(sym)
#endif

#ifdef CONFIG_LIVEUPDATE_CPU

#include <linux/kho/abi/cpu.h>
#include <asm/cpu_preserve.h>
#include <asm/page.h>

/*
 * The preserved stack area holds the context block, one unmapped guard page,
 * then the stack.  It is allocated as a folio of CPUPR_STACK_ORDER, which is
 * naturally aligned to CPUPR_STACK_ALIGN, so the context is found by masking
 * the stack pointer.
 */
#define CPUPR_STACK_CTX_SIZE	ALIGN(sizeof(struct cpupr_stack_context), PAGE_SIZE)
#define CPUPR_STACK_GUARD	PAGE_SIZE
#define CPUPR_STACK_BASE	(CPUPR_STACK_CTX_SIZE + CPUPR_STACK_GUARD)
#define CPUPR_STACK_SIZE	(CPUPR_STACK_BASE + THREAD_SIZE)
#define CPUPR_STACK_ORDER	get_order(CPUPR_STACK_SIZE)
#define CPUPR_STACK_ALIGN	(PAGE_SIZE << CPUPR_STACK_ORDER)
#define CPUPR_STACK_MAGIC	0x435055505354414bULL	/* "CPUPSTAK" */

struct cpupr_ser;

/**
 * struct cpupr_stack_context - Context header at base of preserved CPU stack
 * @magic: Validation signature (%CPUPR_STACK_MAGIC).
 * @session_pgd_pa: Session root page table physical address, or 0.
 * @ser: Preserved CPU descriptor in isolated address space.
 *
 * This structure lives at the base of a preserved CPU's dedicated stack and is
 * accessed by the preserved CPU during parking. It is private to the preserved
 * CPU execution context of the kernel that allocated it.
 */
struct cpupr_stack_context {
	u64 magic;
	u64 session_pgd_pa;
	struct cpupr_ser *ser;
};

/**
 * cpupr_get_stack_context - Return the context block at the base of the preserved stack
 *
 * Masks the current stack pointer to %CPUPR_STACK_ALIGN alignment.  The
 * preserved stack and any architecture exception stacks inside the context
 * block share the same %CPUPR_STACK_ALIGN-aligned folio, so masking the stack
 * pointer always yields the context block.
 *
 * Return: Pointer to &struct cpupr_stack_context.
 */
#ifdef __CPUPR_RUNTIME__
static __always_inline struct cpupr_stack_context *cpupr_get_stack_context(void)
{
	return (void *)ALIGN_DOWN(current_stack_pointer, CPUPR_STACK_ALIGN);
}
#endif

static __always_inline bool cpupr_state_is_stopped(u32 state)
{
	return state == CPUPR_DEAD || state == CPUPR_FAULTED;
}

bool cpu_is_preserved(int cpu);
void cpupr_set_dead(void) __cpupr_sym_asm(cpupr_set_dead);
void cpupr_park_loop(int cpu) __cpupr_sym_asm(cpupr_park_loop);

/**
 * arch_cpupr_park_wait - Wait in a low-power state until kicked
 *
 * Returns after arch_cpupr_kick(), or spuriously, so callers re-check
 * their condition.  A kick sent before the call is not lost.
 */
void arch_cpupr_park_wait(void);
void arch_cpupr_park_init(int cpu);

/*
 * Clean and invalidate the data cache over [start, end) to the Point of
 * Coherency on architectures where kexec disables caches during relocation.
 */
#ifndef arch_cpupr_dcache_clean
static inline void arch_cpupr_dcache_clean(unsigned long start, unsigned long end) { }
#endif

static __always_inline void cpupr_clean_sz(const void *p, size_t sz)
{
	arch_cpupr_dcache_clean((unsigned long)p, (unsigned long)p + sz);
}

#define cpupr_clean(p)		cpupr_clean_sz(p, sizeof(*(p)))

const struct cpumask *cpu_get_preserved_mask(void);
struct cpupr_stack_context *cpupr_get_sctx(int cpu);

struct page;
struct cpupr_session;

/**
 * arch_cpupr_setup_buffer - Prepare the copy of the preserved runtime
 * @text_page: Head page of allocated preserved text memory.
 * @text_nr_pages: Number of pages in the preserved text buffer.
 * @data_page: Head page of allocated preserved data memory.
 * @data_nr_pages: Number of pages in the preserved data buffer.
 *
 * Architecture backend hook called once the .preserved.text and
 * .preserved.data sections have been copied to these pages, outside
 * Scratch memory, before any isolated address space maps them.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
int arch_cpupr_setup_buffer(struct page *text_page, unsigned int text_nr_pages,
			    struct page *data_page, unsigned int data_nr_pages);

void *cpupr_as_alloc_page(void *arg);
void *cpupr_as_alloc_buffer(struct cpupr_session *ps, size_t size);
int cpupr_as_adopt_page(struct cpupr_session *ps, struct page *page);

/**
 * arch_cpupr_as_map - Add one range to a preserved address space
 * @ps: Session whose isolated address space to map into.
 * @pgd: Root page table of the address space.
 * @pa: Physical address of the range.
 * @va: Virtual address the range must appear at.
 * @size: Size of the range in bytes.
 * @prot: Protection to apply.
 *
 * Architecture backend of the core layer, which holds the address-space lock
 * of @ps.  Page-table pages must be obtained from cpupr_as_alloc_page() with
 * @ps as its argument, so that the address space owns them.  Mapping a range
 * again with the same physical address and protection must succeed.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
int arch_cpupr_as_map(struct cpupr_session *ps, void *pgd, phys_addr_t pa,
		      unsigned long va, size_t size, pgprot_t prot);

#else /* !CONFIG_LIVEUPDATE_CPU */

static inline bool cpu_is_preserved(int cpu) { return false; }
static inline const struct cpumask *cpu_get_preserved_mask(void)
{
	return cpu_none_mask;
}

#endif /* CONFIG_LIVEUPDATE_CPU */

#endif /* _LINUX_CPU_PRESERVE_H */
