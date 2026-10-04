// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Physical CPU Preservation Framework for Live Update
 */

/**
 * DOC: Preserved CPU Subsystem
 *
 * Keeps physical CPUs running across a kexec-based live update, without a
 * hardware reset and without going through firmware or the secondary CPU boot
 * path of the incoming kernel.
 *
 * Design Overview
 * ===============
 *
 * A preserved CPU is taken out of Linux through CPU hotplug and waits in a
 * small runtime that lives in KHO-preserved memory and runs on its own stack
 * and page tables, so that it keeps running while the kernel is replaced.
 *
 * The preservation mechanism operates in four phases:
 *
 * 1. **Preparation:** cpupr_preserve() takes the CPU down with
 *    device_offline().  When the dying CPU reports dead
 *    (cpuhp_ap_report_dead()), it enters cpupr_park() instead of the
 *    architecture's dead loop, switches to its preserved stack and to the
 *    isolated page tables of its session, and waits in cpupr_park_loop().
 *
 * 2. **KHO Registration:** The runtime text and data, the preserved stacks,
 *    the descriptors and the isolated page tables are preserved with KHO.
 *    When LUO freezes the file of a CPU, the descriptor of the CPU, with its
 *    hardware identifier, joins the list of CPUs handed over in the FLB data,
 *    in O(1); unfreezing the file takes it off the list again.
 *
 * 3. **Handover:** At kexec, the reboot notifier stops every preserved CPU
 *    that is not handed over.  The incoming kernel boots while the handed-over
 *    CPUs keep waiting in their park loop.
 *
 * 4. **Retrieval and Reclamation:** In an early initcall, before secondary
 *    CPU bringup, the incoming kernel validates the list in the FLB data,
 *    rebuilds the sessions of the listed CPUs and marks the CPUs preserved,
 *    so that smp_init() does not bring them up.  It matches the hardware
 *    identifier of each entry against its present CPUs, and fails the restore
 *    via luo_restore_fail() if an entry does not match an offline CPU.
 *    Retrieving a CPU file leaves the CPU parked.  Finishing the session stops
 *    the CPU and brings it online with device_online().
 *
 * This subsystem provides the generic, hypervisor-agnostic foundation for
 * physical CPU preservation.
 *
 * Lifecycle
 * =========
 *
 * CPU lifecycle state progression::
 *
 *     +-------------------------------------------------------------+
 *     |                          ONLINE                             |
 *     |               (Normal host task scheduling)                 |
 *     +-------------------------------------------------------------+
 *                                    |
 *                                    | preserve (via LUO fd)
 *                                    v
 *     +-------------------------------------------------------------+
 *     |                     PRESERVED_PARKED                        |
 *     |         (Offline, waits in cpupr_park_loop())               |
 *     +-------------------------------------------------------------+
 *                                    |
 *                                    | [Live Update: kexec]
 *                                    v
 *     +-------------------------------------------------------------+
 *     |                     INCOMING PRESERVED                      |
 *     |         (Parked on-core, skipped in secondary boot)         |
 *     |          (Stays parked when its file is retrieved)          |
 *     +-------------------------------------------------------------+
 *                                    |
 *                                    | finish (via LUO session)
 *                                    v
 *     +-------------------------------------------------------------+
 *     |                          OFFLINE                            |
 *     |               (Park loop exited, CPU stopped)               |
 *     +-------------------------------------------------------------+
 *                                    |
 *                                    | device_online()
 *                                    v
 *     +-------------------------------------------------------------+
 *     |                          ONLINE                             |
 *     |                (Rejoined host scheduling)                   |
 *     +-------------------------------------------------------------+
 *
 * Unpreserving a CPU before kexec takes it from PRESERVED_PARKED through
 * OFFLINE back to ONLINE in the same way.
 *
 * File Descriptor Binding
 * =======================
 *
 * 1. **Sysfs control file:** Each hotpluggable CPU device has a read-only
 *    ``/sys/devices/system/cpu/cpu<N>/preserve`` attribute, in its attribute
 *    groups.  The file descriptor of this file handles the lifecycle of the
 *    preserved CPU.  Only hotpluggable CPUs can be preserved.
 *
 * 2. **Preservation via LUO:** Userspace opens this file and preserves the fd
 *    in a LUO session.  Preserving the file takes the CPU offline, which
 *    migrates its interrupts and tasks, parks it (cpupr_park()), and adds it
 *    to the preserved CPU session of the LUO session.
 *
 * 3. **KHO and memory preservation:** The parking loop, dedicated preserved
 *    CPU stacks, runtime execution buffers outside Scratch memory, and
 *    preserved CPU state reside in memory preserved across kexec via KHO.
 *
 * 4. **Incoming boot:** During early boot, the incoming kernel reads and
 *    checks the list of preserved CPUs in the FLB data before secondary SMP
 *    bringup, and does not bring those CPUs online.
 *
 * 5. **Retrieval and finish:** When userspace retrieves the file in the
 *    incoming kernel, it receives a new ``preserve`` file descriptor, opened
 *    under the root directory of init, and the CPU stays parked.  Finishing
 *    the session (also done when the session fd is closed) signals the CPU to
 *    exit the parking loop, waits for it to stop, and brings it back online
 *    with device_online().  Before kexec, closing the session fd unpreserves
 *    the file, which does the same.  Closing the CPU file descriptor itself
 *    has no effect.
 *
 * Architecture Requirements
 * =========================
 *
 * In addition to CPU hotplug with dead-state synchronization
 * (``CONFIG_HOTPLUG_CORE_SYNC_DEAD``: cpuhp_ap_report_dead() hands a dying
 * preserved CPU to cpupr_park()), an architecture selecting
 * ``ARCH_SUPPORTS_LIVEUPDATE_CPU`` must provide:
 *
 * - **Linker script:** Include ``CPUPR_TEXT`` in the executable text
 *   section and ``CPUPR_DATA`` in the data section of
 *   ``arch/<arch>/kernel/vmlinux.lds.S`` (RW_DATA() includes the latter).
 *
 * - **Preserved runtime objects:** Code and data used by a parked core must
 *   be compiled into isolated ``*.preserved.o`` objects, so that they reside
 *   in the KHO-preserved ``.preserved.*`` sections and their symbols are
 *   prefixed with ``__preserved_``.  These include the ``arch_cpupr_*()``
 *   hooks declared with __cpupr_sym_asm() in ``include/linux/cpu_preserve.h``.
 *
 * - **Address-space mapping hook:** arch_cpupr_as_map() maps a range into the
 *   isolated page tables of a session, and allocates the page-table pages it
 *   needs with cpupr_as_alloc_page().
 *
 * - **Runtime copy hooks:** Preserved CPUs run a copy of the preserved text
 *   and data, allocated outside KHO Scratch memory so that the incoming
 *   kernel can unpack safely, and mapped at the address of the sections only
 *   in the isolated address spaces.  arch_cpupr_early_init() initializes the
 *   runtime data at boot, before the copy is made, and
 *   arch_cpupr_setup_buffer() prepares the copy.
 *
 * - **Parking hooks:** arch_cpupr_park_on_stack() and arch_cpupr_park_init()
 *   move a dying CPU onto its preserved stack and page tables;
 *   arch_cpupr_park_wait() waits in a low-power state until arch_cpupr_kick()
 *   wakes the CPU; arch_cpupr_wait_dead() completes stopping it; and
 *   arch_cpupr_hwid() returns its hardware identifier, which the incoming
 *   kernel checks.
 *
 * Isolated Address Space
 * ======================
 *
 * A preserved core does not run on the kernel's own page tables.  Before it is
 * handed over, an isolated page table (struct cpupr_as_ser) is created
 * per-session containing only what the park loop needs, so that a parked core
 * cannot touch memory the new kernel has taken ownership of:
 *
 * - Preserved text, ``PAGE_KERNEL_ROX`` (``.preserved.text``) -- the
 *   park loop and exception stubs;
 * - Preserved read-only data, ``PAGE_KERNEL_RO`` (``.preserved.rodata``
 *   and ``.preserved.ex_table``);
 * - Preserved writable globals, ``PAGE_KERNEL`` NX (``.preserved.data``
 *   and ``.preserved.bss``);
 * - The per-CPU preserved stack area, ``PAGE_KERNEL`` NX: the context page and
 *   the stack, with an unmapped guard page between them;
 * - The per-CPU descriptor (struct cpupr_ser), ``PAGE_KERNEL`` NX;
 * - The memory of the workloads of the session, such as On-Core jobs and the
 *   vCPUs of the KVM Caretaker, ``PAGE_KERNEL`` NX at its direct-map address.
 *
 * Deliberately absent: the rest of the linear direct map, all user address
 * ranges, the kernel heap, vmalloc, modules, and BPF JIT.
 *
 * The address space owns all the memory mapped into it, except the preserved
 * text and data, which every session maps:
 *
 * - The memory comes from cpupr_as_alloc_buffer(), or, for the page
 *   tables and the stack areas, from the folio allocator behind it.  A page
 *   that belongs to another owner and must keep its physical address, such
 *   as a register page that the hardware uses, is adopted with
 *   cpupr_as_adopt_page().
 * - Each folio is preserved with KHO, recorded in struct cpupr_as_ser
 *   and freed only with the address space, once every CPU of the session has
 *   left it: by cpupr_as_unpreserve() in the outgoing kernel, and by
 *   cpupr_as_restore_free() in the incoming kernel, which adds no mappings to
 *   address spaces it did not create.  Nothing mapped is freed on its own, so
 *   no mapping is ever removed and no TLB is flushed.
 * - A session lives as long as its CPU mask is non-empty.  LUO unpreserves and
 *   finishes the files of a session in the reverse order of preservation, so a
 *   file preserved after the file of a CPU of its session, such as the file of
 *   a workload that runs on that CPU, can still use the memory of the address
 *   space when it is unpreserved or finished.
 *
 * Sessions
 * ========
 *
 * Physical cores preserved across live update are grouped per LUO session in a
 * &struct cpupr_session, which owns the session's isolated address space and
 * the mask of its CPUs:
 *
 * - The first CPU file preserved in a LUO session creates the session, and
 *   preserving or unpreserving a CPU file updates its CPU mask.  The session
 *   is released when its last CPU leaves it.
 * - The descriptor of each CPU points to the address space of its session.
 *   At boot, the incoming kernel rebuilds the sessions of the CPUs handed
 *   over to it by grouping the CPUs by address space.
 */

#define pr_fmt(fmt) "cpu_preserve: " fmt

#include <linux/cpu.h>
#include <linux/cpu_preserve.h>
#include <linux/kexec.h>
#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/reboot.h>

/**
 * struct cpupr_session - The preserved CPUs of one LUO session
 * @as: Isolated address space of the CPUs.
 * @incoming: Whether the previous kernel created the session.
 * @as_lock: Serializes the allocations and mappings of an outgoing @as.
 */
struct cpupr_session {
	struct cpupr_as_ser *as;
	bool incoming;
	struct mutex as_lock;
};

static DEFINE_MUTEX(cpupr_lock);

/*
 * A preserved CPU is either incoming, handed over by the previous kernel, or
 * outgoing, preserved by this kernel: cpupr_preserve() rejects preserved and
 * offline CPUs.
 */
static struct cpumask cpupr_mask;
static struct cpumask cpupr_incoming_mask;
static struct cpumask cpupr_outgoing_mask;
static DEFINE_PER_CPU(struct cpupr_ser *, cpupr_cpu_ser);

/* The session whose address space adopted each page, indexed by PFN */
static DEFINE_XARRAY(cpupr_adopted);

static struct folio *cpupr_text_folio;
static struct folio *cpupr_data_folio;

/*
 * Record the folio at @pa as owned by the address space of @ps.  The caller
 * holds @ps->as_lock.
 */
static int cpupr_as_record(struct cpupr_session *ps, phys_addr_t pa)
{
	struct cpupr_as_ser *as = ps->as;
	struct cpupr_folio_block_ser *blk = KHOSER_LOAD_PTR(as->folios);

	lockdep_assert_held(&ps->as_lock);

	if (!blk || blk->count == CPUPR_FOLIOS_PER_BLOCK) {
		blk = kho_alloc_preserve(sizeof(*blk));
		if (IS_ERR(blk))
			return PTR_ERR(blk);

		blk->size = sizeof(*blk);
		blk->next.phys = as->folios.phys;
		KHOSER_STORE_PTR(as->folios, blk);
	}

	blk->pa[blk->count++] = pa;
	return 0;
}

static void *__cpupr_as_alloc(struct cpupr_session *ps, unsigned int order)
{
	void *ptr;
	int ret;

	lockdep_assert_held(&ps->as_lock);

	if (WARN_ON_ONCE(ps->incoming || !ps->as))
		return ERR_PTR(-EINVAL);

	ptr = kho_alloc_preserve(PAGE_SIZE << order);
	if (IS_ERR(ptr))
		return ptr;

	ret = cpupr_as_record(ps, virt_to_phys(ptr));
	if (ret) {
		kho_unpreserve_free(ptr);
		return ERR_PTR(ret);
	}

	cpupr_clean_sz(ptr, PAGE_SIZE << order);
	return ptr;
}

/*
 * Allocate a zeroed, KHO-preserved folio of 2^@order pages, naturally aligned,
 * that the address space of @ps owns.  Return it, or an ERR_PTR().
 */
static void *cpupr_as_alloc_folio(struct cpupr_session *ps, unsigned int order)
{
	guard(mutex)(&ps->as_lock);
	return __cpupr_as_alloc(ps, order);
}

/**
 * cpupr_as_alloc_page - Allocate a page-table page of an address space
 * @arg: The &struct cpupr_session that arch_cpupr_as_map() received.
 *
 * The page-table allocator of arch_cpupr_as_map(), whose caller holds
 * the address-space lock of the session.  This is the order-0 case of the
 * folio allocator: the address space owns the page.
 *
 * Return: A zeroed, preserved page, or %NULL.
 */
void *cpupr_as_alloc_page(void *arg)
{
	void *ptr = __cpupr_as_alloc(arg, 0);

	return IS_ERR(ptr) ? NULL : ptr;
}

static int cpupr_as_map_locked(struct cpupr_session *ps,
			       phys_addr_t pa, unsigned long va,
			       size_t size, pgprot_t prot)
{
	lockdep_assert_held(&ps->as_lock);

	if (WARN_ON_ONCE(ps->incoming || !ps->as))
		return -EINVAL;

	return arch_cpupr_as_map(ps, phys_to_virt(ps->as->pgd_pa), pa,
				 va, size, prot);
}

/* Map one range into the address space of the outgoing session @ps. */
static int cpupr_as_map(struct cpupr_session *ps, phys_addr_t pa,
			unsigned long va, size_t size, pgprot_t prot)
{
	guard(mutex)(&ps->as_lock);
	return cpupr_as_map_locked(ps, pa, va, size, prot);
}

/**
 * cpupr_as_alloc_buffer - Allocate a buffer mapped into a session
 * @ps:   Outgoing preserved CPU session.
 * @size: Size of the buffer in bytes.
 *
 * Allocates zeroed, KHO-preserved memory that the isolated address space of
 * @ps owns, and maps it there at its direct-map address with %PAGE_KERNEL.
 * The buffer is a folio of the smallest power-of-two number of pages that
 * holds @size bytes, naturally aligned.  It is freed when the address space
 * is torn down, after every CPU of @ps has left it: callers never free it.
 *
 * Return: The buffer, or an ERR_PTR() on failure.
 */
void *cpupr_as_alloc_buffer(struct cpupr_session *ps, size_t size)
{
	unsigned int order = get_order(size);
	void *buf;
	int ret;

	if (!ps || !size)
		return ERR_PTR(-EINVAL);

	guard(mutex)(&ps->as_lock);
	buf = __cpupr_as_alloc(ps, order);
	if (IS_ERR(buf))
		return buf;

	/* On failure, the buffer stays with the address space */
	ret = cpupr_as_map_locked(ps, virt_to_phys(buf), (unsigned long)buf,
				  PAGE_SIZE << order, PAGE_KERNEL);
	return ret ? ERR_PTR(ret) : buf;
}

static int cpupr_as_adopt_locked(struct cpupr_session *ps, struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	int ret;

	ret = xa_insert(&cpupr_adopted, pfn, ps, GFP_KERNEL);
	if (ret)
		return ret;

	ret = kho_preserve_folio(page_folio(page));
	if (ret)
		goto err_erase;

	ret = cpupr_as_record(ps, page_to_phys(page));
	if (ret)
		goto err_unpreserve;

	/* Dropped by cpupr_as_unpreserve() */
	get_page(page);
	return 0;

err_unpreserve:
	kho_unpreserve_folio(page_folio(page));
err_erase:
	xa_erase(&cpupr_adopted, pfn);
	return ret;
}

/**
 * cpupr_as_adopt_page - Map a page of another owner into a session
 * @ps:   Outgoing preserved CPU session.
 * @page: Order-0 page to adopt.
 *
 * Makes the isolated address space of @ps a co-owner of @page, for a page
 * whose physical address must not change, and maps it at its direct-map
 * address with %PAGE_KERNEL.  The address space preserves @page with KHO,
 * takes a reference to it and records it with its own folios, so that @page
 * is unpreserved and the reference dropped only when the address space is
 * torn down.  The owner keeps and drops its own reference as usual.
 *
 * Adopting a page again into the same session only maps it again.  A page
 * belongs to the address space of at most one session at a time.
 *
 * Return: 0 on success, -EBUSY if the address space of another session owns
 *         @page, or another negative errno on failure.
 */
int cpupr_as_adopt_page(struct cpupr_session *ps, struct page *page)
{
	int ret;

	if (!ps || !page || folio_test_large(page_folio(page)))
		return -EINVAL;

	guard(mutex)(&ps->as_lock);
	if (xa_load(&cpupr_adopted, page_to_pfn(page)) != ps) {
		ret = cpupr_as_adopt_locked(ps, page);
		if (ret)
			return ret;
	}

	return cpupr_as_map_locked(ps, page_to_phys(page),
				   (unsigned long)page_address(page),
				   PAGE_SIZE, PAGE_KERNEL);
}

static struct folio *cpupr_alloc_section_copy(const void *start, const void *end)
{
	size_t size = end - start;
	struct folio *folio;

	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, get_order(size));
	if (!folio)
		return NULL;

	memcpy(folio_address(folio), start, size);
	return folio;
}

static int cpupr_init_runtime_buffer_locked(void)
{
	size_t text_size = __preserved_text_end - __preserved_text_start;
	size_t data_size = __preserved_data_end - __preserved_data_start;
	int ret = -ENOMEM;

	lockdep_assert_held(&cpupr_lock);
	/* There is only one runtime buffer per-system */
	if (cpupr_text_folio)
		return 0;

	cpupr_text_folio = cpupr_alloc_section_copy(__preserved_text_start,
						    __preserved_text_end);
	if (!cpupr_text_folio)
		return -ENOMEM;

	cpupr_data_folio = cpupr_alloc_section_copy(__preserved_data_start,
						    __preserved_data_end);
	if (!cpupr_data_folio)
		goto err_put_text_folio;

	ret = arch_cpupr_setup_buffer(folio_page(cpupr_text_folio, 0),
				      DIV_ROUND_UP(text_size, PAGE_SIZE),
				      folio_page(cpupr_data_folio, 0),
				      DIV_ROUND_UP(data_size, PAGE_SIZE));
	if (ret)
		goto err_put_data_folio;

	return 0;

err_put_data_folio:
	folio_put(cpupr_data_folio);
	cpupr_data_folio = NULL;
err_put_text_folio:
	folio_put(cpupr_text_folio);
	cpupr_text_folio = NULL;
	return ret;
}

/**
 * cpupr_init_runtime_buffer - Allocate execution buffer outside Scratch
 *
 * Return: 0 on success, or negative error code on allocation/setup failure.
 */
static int cpupr_init_runtime_buffer(void)
{
	guard(mutex)(&cpupr_lock);

	return cpupr_init_runtime_buffer_locked();
}

static int cpupr_as_map_runtime(struct cpupr_session *ps)
{
	unsigned long text_start = (unsigned long)__preserved_text_start;
	unsigned long data_start = (unsigned long)__preserved_data_start;
	unsigned long rodata_end = (unsigned long)__preserved_rodata_end;
	size_t text_sz = __preserved_text_end - __preserved_text_start;
	size_t ro_sz = __preserved_rodata_end - __preserved_data_start;
	size_t rw_sz = __preserved_data_end - __preserved_rodata_end;
	phys_addr_t text_pa = PFN_PHYS(folio_pfn(cpupr_text_folio));
	phys_addr_t data_pa = PFN_PHYS(folio_pfn(cpupr_data_folio));
	int ret = 0;

	if (text_sz)
		ret = cpupr_as_map(ps, text_pa, text_start, text_sz, PAGE_KERNEL_ROX);

	if (!ret && ro_sz)
		ret = cpupr_as_map(ps, data_pa, data_start, ro_sz, PAGE_KERNEL_RO);

	if (!ret && rw_sz)
		ret = cpupr_as_map(ps, data_pa + ro_sz, rodata_end, rw_sz, PAGE_KERNEL);

	return ret;
}

/*
 * Free the address space of the outgoing session @ps, once no CPU runs on it,
 * with everything it owns.
 */
static void cpupr_as_unpreserve(struct cpupr_session *ps)
{
	struct cpupr_folio_block_ser *blk, *next;

	if (!ps->as)
		return;

	for (blk = KHOSER_LOAD_PTR(ps->as->folios); blk; blk = next) {
		next = KHOSER_LOAD_PTR(blk->next);
		for (u64 i = 0; i < blk->count; i++) {
			unsigned long pfn = PHYS_PFN(blk->pa[i]);

			if (xa_load(&cpupr_adopted, pfn) == ps)
				xa_erase(&cpupr_adopted, pfn);
			kho_unpreserve_free(phys_to_virt(blk->pa[i]));
		}
		kho_unpreserve_free(blk);
	}

	kho_unpreserve_free(ps->as);
	ps->as = NULL;
}

/*
 * Build the isolated address space of the new outgoing session @ps: allocate
 * its root page table and map the preserved text and data into it.
 */
static int cpupr_as_create(struct cpupr_session *ps)
{
	void *pgd;
	int ret;

	ret = cpupr_init_runtime_buffer();
	if (ret)
		return ret;

	ps->as = kho_alloc_preserve(sizeof(*ps->as));
	if (IS_ERR(ps->as)) {
		ret = PTR_ERR(ps->as);
		ps->as = NULL;
		return ret;
	}
	ps->as->size = sizeof(*ps->as);

	pgd = cpupr_as_alloc_folio(ps, 0);
	if (IS_ERR(pgd)) {
		ret = PTR_ERR(pgd);
		goto err_as_unpreserve;
	}

	ps->as->pgd_pa = virt_to_phys(pgd);
	ret = cpupr_as_map_runtime(ps);
	if (ret)
		goto err_as_unpreserve;
	return 0;

err_as_unpreserve:
	cpupr_as_unpreserve(ps);
	return ret;
}

static bool cpupr_is_incoming(int cpu)
{
	if ((unsigned int)cpu >= nr_cpu_ids)
		return false;
	return cpumask_test_cpu(cpu, &cpupr_incoming_mask);
}

/*
 * The descriptor of a preserved CPU.  It belongs to the address space of the
 * session of the CPU, and stays valid while the CPU holds its reference.
 */
static struct cpupr_ser *cpupr_get_ser(int cpu)
{
	if ((unsigned int)cpu >= nr_cpu_ids)
		return NULL;

	return READ_ONCE(per_cpu(cpupr_cpu_ser, cpu));
}

/**
 * cpupr_get_sctx - Return the stack context of a preserved CPU
 * @cpu: Logical CPU identifier.
 *
 * Return: The stack context of @cpu, or %NULL if @cpu is not preserved or was
 *         handed over by the previous kernel, which owns its stack context.
 */
struct cpupr_stack_context *cpupr_get_sctx(int cpu)
{
	struct cpupr_stack_context *sctx;
	struct cpupr_ser *ser;
	phys_addr_t pa;

	/* The stack of an incoming CPU is private to the previous kernel */
	if (cpupr_is_incoming(cpu))
		return NULL;

	ser = cpupr_get_ser(cpu);
	if (!ser)
		return NULL;

	pa = READ_ONCE(ser->stack_pa);
	if (!pa)
		return NULL;

	sctx = phys_to_virt(pa);
	return sctx->magic == CPUPR_STACK_MAGIC ? sctx : NULL;
}

/**
 * cpu_is_preserved - Check whether a CPU is currently preserved
 * @cpu: Logical CPU identifier.
 *
 * Return: True if @cpu is currently preserved, false otherwise.
 */
bool cpu_is_preserved(int cpu)
{
	return (unsigned int)cpu < nr_cpu_ids && cpumask_test_cpu(cpu, &cpupr_mask);
}

/**
 * cpu_get_preserved_mask - Get the mask of all currently preserved CPUs
 *
 * Return: Read-only pointer to the cpumask of preserved CPUs.
 */
const struct cpumask *cpu_get_preserved_mask(void)
{
	return &cpupr_mask;
}
