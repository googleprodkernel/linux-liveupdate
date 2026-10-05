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
#include <linux/smp.h>
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
#include <asm/sections.h>

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
 * @workload_context: Opaque owning workload or session context.
 * @session_pgd_pa: Session root page table physical address, or 0.
 * @ser: Preserved CPU descriptor in isolated address space.
 * @entry_fn: Workload entry function to run.
 *
 * This structure lives at the base of a preserved CPU's dedicated stack and is
 * accessed by the preserved CPU during parking and workload execution. It is
 * private to the preserved CPU execution context of the kernel that allocated it.
 */
struct cpupr_stack_context {
	u64 magic;
	u64 workload_context;
	u64 session_pgd_pa;
	struct cpupr_ser *ser;
	void (*entry_fn)(void *data);
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

static inline bool cpupr_is_runtime_text(const void *fn)
{
	unsigned long addr = (unsigned long)fn;

	return addr >= (unsigned long)__preserved_text_start &&
	       addr < (unsigned long)__preserved_text_end;
}

bool cpu_is_preserved(int cpu);
u32 cpupr_state(int cpu);
bool cpupr_should_exit(void) __cpupr_sym_asm(cpupr_should_exit);
void cpupr_set_dead(void) __cpupr_sym_asm(cpupr_set_dead);
void cpupr_park_loop(int cpu) __cpupr_sym_asm(cpupr_park_loop);
void cpupr_park(int cpu);

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
int cpupr_attach_workload(int cpu,
			  void (*entry_fn)(void *data), void *data);
int cpupr_detach_workload(int cpu);
void cpupr_set_workload_context(int cpu, void *ctx);

struct attribute_group;
extern const struct attribute_group cpu_preserve_attr_group;
extern const struct attribute_group cpu_preserve_root_attr_group;

/**
 * cpupr_report_dead - Park preserved CPU when reporting dead in hotplug
 *
 * Invoked by cpuhp_ap_report_dead() after CPU hotplug offline synchronization
 * is complete. If the calling CPU is marked for preservation across live update,
 * transition it into the preserved parking loop instead of powering down.
 */
static inline void cpupr_report_dead(void)
{
	if (cpu_is_preserved(raw_smp_processor_id()))
		cpupr_park(raw_smp_processor_id());
}

/**
 * arch_cpupr_kick - Signal or wake up a preserved physical CPU
 * @cpu: Logical CPU identifier.
 *
 * Architecture backend hook to wake a preserved CPU from arch_cpupr_park_wait().
 *
 * Executed in normal text context.
 */
void arch_cpupr_kick(int cpu);

/**
 * arch_cpupr_hwid - Hardware identifier of a CPU
 * @cpu: Logical CPU identifier.
 *
 * Architecture backend hook that returns the hardware identifier of @cpu.
 * The incoming kernel matches the hardware identifier of each handed-over CPU
 * against its present CPUs, and refuses the handover otherwise.
 *
 * Return: The hardware identifier of @cpu.
 */
u64 arch_cpupr_hwid(unsigned int cpu);

/**
 * arch_cpupr_park_finish - Architecture cleanup on park loop exit
 * @cpu: Logical CPU identifier.
 *
 * Architecture backend hook invoked when @cpu exits the preserved parking loop
 * (or on the fallback path if parking fails). May power down the CPU and not
 * return.
 */
void arch_cpupr_park_finish(int cpu) __cpupr_sym_asm(arch_cpupr_park_finish);

/**
 * arch_cpupr_early_init - Initialize the arch data of the runtime
 *
 * Architecture backend hook that initializes the data the preserved runtime
 * reads, such as the x86 preserved IDT.  Called once at boot, before the
 * runtime is copied: preserved CPUs run on the copy, which later writes to
 * the sections do not reach.
 */
void arch_cpupr_early_init(void);

/**
 * arch_cpupr_mode - Modes of the kernel that preserved CPUs depend on
 *
 * Architecture backend hook.  The outgoing kernel records its value in the
 * handover data, and the incoming kernel refuses the handover unless it
 * returns the same value, such as the same x86 APIC mode.
 *
 * Return: Architecture mode descriptor.
 */
struct cpupr_arch_mode_ser arch_cpupr_mode(void);

/**
 * arch_cpupr_park_on_stack - Switch stack and enter park loop
 * @cpu: Logical CPU identifier.
 * @stack_top: Top of the dedicated preserved stack.
 *
 * Architecture backend hook to switch to the preserved CPU stack and
 * invoke cpupr_park_loop(). Does not return.
 *
 * Executed in normal text context during CPU teardown.
 */
void arch_cpupr_park_on_stack(int cpu, unsigned long stack_top);

/**
 * arch_cpupr_wait_dead - Finish stopping a preserved CPU
 * @cpu: Logical CPU identifier.
 * @ser: Preserved CPU descriptor, or %NULL on bringup fallback.
 * @bringup: True if @cpu is about to be brought back online via CPU hotplug.
 *
 * Architecture backend hook called once @cpu has reported %CPUPR_DEAD
 * or %CPUPR_FAULTED, to report what the runtime recorded and to prepare
 * @cpu for hotplug bringup or wait for it to be fully stopped.
 *
 * Executed in normal text context during CPU teardown.
 */
void arch_cpupr_wait_dead(int cpu, struct cpupr_ser *ser, bool bringup);

struct page;
struct cpupr_session;
struct liveupdate_session;

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

const struct cpumask *cpupr_session_cpus(struct cpupr_session *ps);

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
static inline void cpupr_report_dead(void) {}
static inline const struct cpumask *cpu_get_preserved_mask(void)
{
	return cpu_none_mask;
}

#endif /* CONFIG_LIVEUPDATE_CPU */

#endif /* _LINUX_CPU_PRESERVE_H */
