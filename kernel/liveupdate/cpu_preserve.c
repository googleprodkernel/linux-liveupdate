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
 *
 * Workloads
 * =========
 *
 * A kernel subsystem can run code on a parked CPU of the outgoing kernel with
 * cpupr_attach_workload().  The CPU moves from PARKED to the WORKLOAD
 * state and calls the entry function, which must be preserved-runtime code
 * that uses only memory mapped into the address space of the session.
 * The CPU is PARKED again when the function returns, which it does when it is
 * done or when cpupr_should_exit() tells it to, for example after
 * cpupr_detach_workload().  A workload keeps running across kexec;
 * retrieving the CPU file in the incoming kernel detaches it.
 */

#define pr_fmt(fmt) "cpu_preserve: " fmt

#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/cpu_preserve.h>
#include "luo_internal.h"
#include <linux/file.h>
#include <linux/fs_struct.h>
#include <linux/iopoll.h>
#include <linux/kexec.h>
#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/pagemap.h>
#include <linux/reboot.h>
#include <linux/suspend.h>

/**
 * struct cpupr_session - The preserved CPUs of one LUO session
 * @node: Entry on cpupr_sessions while @lsession can find it.
 * @lsession: The outgoing LUO session, or %NULL.
 * @cpus: CPUs of the session that are preserved and parked.
 * @as: Isolated address space of the CPUs.
 * @incoming: Whether the previous kernel created the session.
 * @as_lock: Serializes the allocations and mappings of an outgoing @as.
 * @mapped_mem_file: Last job mem_file mapped into @as.
 * @cleaned_mem_file: Last job mem_file cache-cleaned on freeze.
 */
struct cpupr_session {
	struct list_head node;
	struct liveupdate_session *lsession;
	struct cpumask cpus;
	struct cpupr_as_ser *as;
	bool incoming;
	struct mutex as_lock;
	struct file *mapped_mem_file;
	struct file *cleaned_mem_file;
};

/*
 * Lock order: lock_device_hotplug(), cpupr_sessions_lock,
 * cpupr_lock, cpupr_session.as_lock.
 */
static DEFINE_MUTEX(cpupr_lock);
static DEFINE_MUTEX(cpupr_sessions_lock);
static LIST_HEAD(cpupr_sessions);

/*
 * A preserved CPU is either incoming, handed over by the previous kernel, or
 * outgoing, preserved by this kernel: cpupr_preserve() rejects preserved and
 * offline CPUs.
 */
static struct cpumask cpupr_mask;
static struct cpumask cpupr_incoming_mask;
static struct cpumask cpupr_outgoing_mask;
static DEFINE_PER_CPU(struct cpupr_ser *, cpupr_cpu_ser);
static DEFINE_PER_CPU(struct cpupr_session *, cpupr_cpu_session);
/* The link that points to the descriptor of a CPU on the handover list */
static DEFINE_PER_CPU(u64 *, cpupr_cpu_link);
static struct cpupr_global_ser *cpupr_global_ser;

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

static bool cpupr_folio_block_ser_is_valid(struct cpupr_folio_block_ser *fb)
{
	return fb && fb->size == sizeof(*fb) && fb->flags == 0 &&
		fb->count <= CPUPR_FOLIOS_PER_BLOCK;
}

/* Free an incoming address space, which the previous kernel created. */
static void cpupr_as_restore_free(struct cpupr_as_ser *ser)
{
	struct cpupr_folio_block_ser *blk, *next;

	if (!ser)
		return;

	for (blk = KHOSER_LOAD_PTR(ser->folios); blk; blk = next) {
		if (WARN_ON_ONCE(!cpupr_folio_block_ser_is_valid(blk)))
			break;

		next = KHOSER_LOAD_PTR(blk->next);
		for (u64 i = 0; i < blk->count; i++)
			kho_restore_free(phys_to_virt(blk->pa[i]));
		kho_restore_free(blk);
	}
	kho_restore_free(ser);
}

static bool cpupr_is_incoming(int cpu)
{
	if ((unsigned int)cpu >= nr_cpu_ids)
		return false;
	return cpumask_test_cpu(cpu, &cpupr_incoming_mask);
}

static noinline void cpupr_build_hwid_map(struct xarray *xa)
{
	static DEFINE_MUTEX(hwid_lock);
	unsigned int cpu;

	guard(mutex)(&hwid_lock);
	if (!xa_empty(xa))
		return;

	for_each_present_cpu(cpu)
		xa_store(xa, arch_cpupr_hwid(cpu), xa_mk_value(cpu), GFP_KERNEL);
}

static int cpupr_find_cpu_by_hwid(u64 hwid)
{
	static DEFINE_XARRAY(hwid_to_cpu);
	void *entry;

	if (unlikely(xa_empty(&hwid_to_cpu))) {
		cpupr_build_hwid_map(&hwid_to_cpu);
		BUG_ON(xa_empty(&hwid_to_cpu));
	}

	entry = xa_load(&hwid_to_cpu, hwid);
	if (!entry)
		return -ENODEV;

	return xa_to_value(entry);
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

static void cpupr_signal_exit_ser(struct cpupr_ser *ser)
{
	u32 old_state;

	if (!ser)
		return;

	cpupr_clean(ser);
	old_state = READ_ONCE(ser->state);
	while (!cpupr_state_is_stopped(old_state) && old_state != CPUPR_EXITING) {
		if (try_cmpxchg(&ser->state, &old_state, CPUPR_EXITING))
			break;
	}
	cpupr_clean(ser);
}

/**
 * cpupr_attach_workload - Attach & start workload execution on core
 * @cpu: Logical CPU identifier.
 * @entry_fn: Workload callback to execute repeatedly on the physical core.
 * @data: Opaque argument passed to @entry_fn.
 *
 * Return: 0 on success, negative error code on failure.
 */
int cpupr_attach_workload(int cpu,
			  void (*entry_fn)(void *data), void *data)
{
	struct cpupr_stack_context *sctx;
	struct cpupr_ser *ser;
	u32 old;

	if ((unsigned int)cpu >= nr_cpu_ids || !entry_fn ||
	    !cpupr_is_runtime_text(entry_fn))
		return -EINVAL;

	mutex_lock(&cpupr_lock);
	if (!cpumask_test_cpu(cpu, &cpupr_outgoing_mask)) {
		mutex_unlock(&cpupr_lock);
		return -ENODEV;
	}

	sctx = cpupr_get_sctx(cpu);
	if (!sctx) {
		mutex_unlock(&cpupr_lock);
		return -ENODEV;
	}

	ser = cpupr_get_ser(cpu);
	if (!ser) {
		mutex_unlock(&cpupr_lock);
		return -EBUSY;
	}

	cpupr_clean(ser);
	if (READ_ONCE(ser->state) != CPUPR_PARKED || sctx->entry_fn) {
		mutex_unlock(&cpupr_lock);
		return -EBUSY;
	}

	WRITE_ONCE(sctx->workload_context, (u64)(uintptr_t)data);
	WRITE_ONCE(sctx->entry_fn, entry_fn);
	cpupr_clean(sctx);

	old = CPUPR_PARKED;
	/* Pairs with smp_load_acquire() in cpupr_park_loop() */
	if (!try_cmpxchg_release(&ser->state, &old, CPUPR_WORKLOAD)) {
		WRITE_ONCE(sctx->entry_fn, NULL);
		WRITE_ONCE(sctx->workload_context, 0);
		cpupr_clean(sctx);
		mutex_unlock(&cpupr_lock);
		return -EBUSY;
	}
	cpupr_clean(ser);

	arch_cpupr_kick(cpu);
	mutex_unlock(&cpupr_lock);
	return 0;
}

/*
 * Parking and stopping take microseconds once the CPU runs, so the timeouts
 * only catch a wedged CPU: parking gets 100 ms after the CPU has reported
 * dead, and stopping gets 1s to allow any running workload slice to yield.
 */
#define CPUPR_WAIT_PARKED_TIMEOUT_US	(100 * USEC_PER_MSEC)
#define CPUPR_WAIT_DEAD_TIMEOUT_US	USEC_PER_SEC
#define CPUPR_WAIT_POLL_US		10

static u32 cpupr_read_state(struct cpupr_ser *ser)
{
	cpupr_clean(ser);
	/* Pairs with the release cmpxchg()es of the preserved CPU */
	return smp_load_acquire(&ser->state);
}

/**
 * cpupr_detach_workload - Detach workload and return core to idle park
 * @cpu: Logical CPU identifier.
 *
 * The stack context of a CPU that the previous kernel handed over is private
 * to that kernel: only the state of such a CPU is changed.
 *
 * Return: 0 on success, negative error code on failure.
 */
int cpupr_detach_workload(int cpu)
{
	struct cpupr_stack_context *sctx;
	struct cpupr_ser *ser;
	u32 state;
	int ret;

	if ((unsigned int)cpu >= nr_cpu_ids)
		return -EINVAL;

	scoped_guard(mutex, &cpupr_lock) {
		if (!cpumask_test_cpu(cpu, &cpupr_mask))
			return -ENODEV;

		sctx = cpupr_get_sctx(cpu);
		if (!cpupr_is_incoming(cpu) && !sctx)
			return -ENODEV;

		ser = cpupr_get_ser(cpu);
		if (!ser)
			return -ENODEV;

		state = cpupr_read_state(ser);
		if (state == CPUPR_FAULTED)
			return -EIO;

		if (state != CPUPR_WORKLOAD &&
		    state != CPUPR_DETACHING) {
			if (sctx) {
				WRITE_ONCE(sctx->entry_fn, NULL);
				cpupr_clean(sctx);
			}
			return 0;
		}

		while (state == CPUPR_WORKLOAD) {
			if (try_cmpxchg(&ser->state, &state,
					CPUPR_DETACHING)) {
				cpupr_clean(ser);
				break;
			}
		}

		arch_cpupr_kick(cpu);
	}

	ret = read_poll_timeout(cpupr_read_state, state,
				state != CPUPR_WORKLOAD &&
				state != CPUPR_DETACHING,
				CPUPR_WAIT_POLL_US, CPUPR_WAIT_PARKED_TIMEOUT_US,
				false, ser);
	if (WARN_ON_ONCE(ret || state == CPUPR_FAULTED))
		return ret ?: -EIO;

	scoped_guard(mutex, &cpupr_lock) {
		sctx = cpupr_get_sctx(cpu);
		if (sctx) {
			WRITE_ONCE(sctx->entry_fn, NULL);
			cpupr_clean(sctx);
		}
	}

	return 0;
}

/**
 * cpupr_set_workload_context - Set workload context on a preserved CPU stack
 * @cpu: Logical CPU identifier.
 * @ctx: Opaque owning workload context pointer.
 */
void cpupr_set_workload_context(int cpu, void *ctx)
{
	struct cpupr_stack_context *sctx;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return;

	mutex_lock(&cpupr_lock);
	sctx = cpupr_get_sctx(cpu);
	if (sctx) {
		WRITE_ONCE(sctx->workload_context, (u64)(uintptr_t)ctx);
		cpupr_clean(sctx);
	}
	mutex_unlock(&cpupr_lock);
}

static void cpupr_signal_exit(int cpu)
{
	struct cpupr_stack_context *sctx = cpupr_get_sctx(cpu);

	cpupr_signal_exit_ser(cpupr_get_ser(cpu));
	if (sctx) {
		WRITE_ONCE(sctx->entry_fn, NULL);
		WRITE_ONCE(sctx->workload_context, 0);
		cpupr_clean(sctx);
	}
}

/**
 * cpupr_state - Return the current workload state of a preserved CPU
 * @cpu: Logical CPU identifier.
 *
 * Return: Current &enum cpupr_workload state of @cpu, or
 *         %CPUPR_DEAD if @cpu is not preserved.
 */
u32 cpupr_state(int cpu)
{
	struct cpupr_ser *ser = cpupr_get_ser(cpu);

	if (!ser)
		return CPUPR_DEAD;

	return cpupr_read_state(ser);
}

static int cpupr_wait_parked(int cpu)
{
	struct cpupr_ser *ser = cpupr_get_ser(cpu);
	u32 state;
	int ret;

	if (!ser)
		return -ENODEV;

	ret = read_poll_timeout(cpupr_read_state, state, state != CPUPR_PARKING,
				CPUPR_WAIT_POLL_US, CPUPR_WAIT_PARKED_TIMEOUT_US,
				false, ser);
	if (!ret && state == CPUPR_PARKED)
		return 0;

	pr_err("Preserved cpu %d failed to park (state=%u)\n", cpu, state);

	return ret ?: -EIO;
}

static int cpupr_wait_dead_ser(int cpu, struct cpupr_ser *ser,
			       u64 timeout_us, bool bringup)
{
	u32 state;
	int ret;

	if (!ser)
		return -ENODEV;

	ret = read_poll_timeout(cpupr_read_state, state, cpupr_state_is_stopped(state),
				CPUPR_WAIT_POLL_US, timeout_us, false, ser);
	if (ret) {
		if (IS_ENABLED(CONFIG_X86)) {
			pr_debug("Forcing preserved cpu %d stop via INIT reset (state=%u)\n",
				 cpu, state);
		} else {
			pr_err("Timed out waiting for preserved cpu %d to stop (state=%u)\n",
			       cpu, state);
			return ret;
		}
	}

	if (state == CPUPR_FAULTED)
		pr_err("Preserved cpu %d stopped on a fault\n", cpu);

	arch_cpupr_wait_dead(cpu, ser, bringup);
	return 0;
}

static int cpupr_wait_dead_timeout(int cpu, u64 timeout_us)
{
	return cpupr_wait_dead_ser(cpu, cpupr_get_ser(cpu), timeout_us, false);
}

static int cpupr_wait_dead(int cpu)
{
	return cpupr_wait_dead_ser(cpu, cpupr_get_ser(cpu), 100, true);
}

/**
 * cpupr_park - Main execution and parking loop for a preserved CPU
 * @cpu: Logical CPU identifier of the calling core.
 */
void cpupr_park(int cpu)
{
	struct cpupr_stack_context *sctx = cpupr_get_sctx(cpu);

	if (!sctx || !sctx->session_pgd_pa) {
		struct cpupr_ser *ser = cpupr_get_ser(cpu);

		if (ser) {
			/* Pairs with the acquire in cpupr_read_state() */
			smp_store_release(&ser->state, CPUPR_DEAD);
			cpupr_clean(ser);
		}
		arch_cpupr_park_finish(cpu);
		return;
	}

	arch_cpupr_park_on_stack(cpu, (unsigned long)sctx + CPUPR_STACK_SIZE);
}

static bool cpupr_is_frozen(unsigned int cpu)
{
	return per_cpu(cpupr_cpu_link, cpu) != NULL;
}

/*
 * Unlink @cpu from the list of CPUs handed over to the next kernel.  Each CPU
 * on the list records the link that points to its descriptor, the head of the
 * list or the descriptor of the CPU before it, so that unlinking is O(1).
 */
static void cpupr_handover_del(unsigned int cpu)
{
	struct cpupr_ser *ser = per_cpu(cpupr_cpu_ser, cpu);
	struct cpupr_ser *next = KHOSER_LOAD_PTR(ser->next);
	u64 *link = per_cpu(cpupr_cpu_link, cpu);

	lockdep_assert_held(&cpupr_lock);

	*link = ser->next.phys;
	cpupr_clean(link);
	if (next) {
		int next_cpu = cpupr_find_cpu_by_hwid(next->hwid);

		if (!WARN_ON_ONCE(next_cpu < 0))
			per_cpu(cpupr_cpu_link, next_cpu) = link;
	}

	ser->next.phys = 0;
	cpupr_clean(ser);
	per_cpu(cpupr_cpu_link, cpu) = NULL;
}

/*
 * Drop @cpu out of the preserved state.  The caller holds cpupr_lock,
 * and the core either never parked or has stopped.  Its stack and descriptor
 * stay with the address space of its session, which frees them once every CPU
 * of the session has left it.
 */
static void cpupr_unpreserve_locked(unsigned int cpu)
{
	lockdep_assert_held(&cpupr_lock);

	/* LUO unfreezes a file before it unpreserves it */
	if (WARN_ON_ONCE(cpupr_is_frozen(cpu)))
		cpupr_handover_del(cpu);

	WRITE_ONCE(per_cpu(cpupr_cpu_ser, cpu), NULL);
	per_cpu(cpupr_cpu_session, cpu) = NULL;
	cpumask_clear_cpu(cpu, &cpupr_outgoing_mask);
	cpumask_clear_cpu(cpu, &cpupr_incoming_mask);
	cpumask_clear_cpu(cpu, &cpupr_mask);
}

static struct cpupr_session *cpupr_session_alloc(void)
{
	struct cpupr_session *ps;

	lockdep_assert_held(&cpupr_sessions_lock);

	ps = kzalloc_obj(*ps);
	if (!ps)
		return NULL;

	INIT_LIST_HEAD(&ps->node);
	mutex_init(&ps->as_lock);
	return ps;
}

static void cpupr_session_release(struct cpupr_session *ps)
{
	if (ps->incoming)
		cpupr_as_restore_free(ps->as);
	else
		cpupr_as_unpreserve(ps);

	kfree(ps);
}

/*
 * Look up the preserved CPU session of @s, or create it with its isolated
 * address space.  Return the session, or an ERR_PTR() on failure.
 */
static struct cpupr_session *cpupr_session_get(struct liveupdate_session *s)
{
	struct cpupr_session *ps;
	int ret;

	if (!s)
		return ERR_PTR(-EINVAL);

	guard(mutex)(&cpupr_sessions_lock);
	list_for_each_entry(ps, &cpupr_sessions, node) {
		if (ps->lsession == s)
			return ps;
	}

	ps = cpupr_session_alloc();
	if (!ps)
		return ERR_PTR(-ENOMEM);

	ret = cpupr_as_create(ps);
	if (ret) {
		kfree(ps);
		return ERR_PTR(ret);
	}

	ps->lsession = s;
	list_add(&ps->node, &cpupr_sessions);

	return ps;
}

/*
 * Release @ps if it has no preserved CPUs: the error path of cpupr_preserve()
 * calls this when preserving a CPU fails without leaving the CPU wedged.
 */
static void cpupr_session_put_if_empty(struct cpupr_session *ps)
{
	bool empty;

	if (!ps || IS_ERR(ps))
		return;

	scoped_guard(mutex, &cpupr_sessions_lock) {
		empty = cpumask_empty(&ps->cpus);
		if (empty)
			list_del_init(&ps->node);
	}

	if (empty)
		cpupr_session_release(ps);
}

/*
 * A CPU of @ps failed to stop and stays in @ps->cpus for good, so @ps can
 * outlive its LUO session.  Make sure that no later session can find @ps
 * through the stale pointer.
 */
static void cpupr_session_detach(struct cpupr_session *ps)
{
	if (!ps)
		return;

	guard(mutex)(&cpupr_sessions_lock);
	list_del_init(&ps->node);
	ps->lsession = NULL;
}

/**
 * cpupr_session_cpus - Return the cpumask of preserved CPUs in @ps
 * @ps: Preserved CPU session.
 *
 * Return: Read-only cpumask of physical CPUs currently preserved in @ps, or
 *         %cpu_none_mask if @ps is %NULL or an ERR_PTR().
 */
const struct cpumask *cpupr_session_cpus(struct cpupr_session *ps)
{
	if (!ps || IS_ERR(ps))
		return cpu_none_mask;

	return &ps->cpus;
}

/* The session of a preserved CPU */
static struct cpupr_session *cpupr_session_of(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids)
		return NULL;

	guard(mutex)(&cpupr_lock);
	return per_cpu(cpupr_cpu_session, cpu);
}

static void cpupr_session_remove_cpu(struct cpupr_session *ps,
				     unsigned int cpu)
{
	bool empty;

	if (!ps)
		return;

	scoped_guard(mutex, &cpupr_sessions_lock) {
		if (!cpumask_test_and_clear_cpu(cpu, &ps->cpus))
			return;
		empty = cpumask_empty(&ps->cpus);
		if (empty)
			list_del_init(&ps->node);
	}

	if (empty)
		cpupr_session_release(ps);
}

/*
 * Look up the incoming preserved CPU session of @as, or create it.
 */
static struct cpupr_session *cpupr_incoming_session_get(struct cpupr_as_ser *as)
{
	struct cpupr_session *ps;

	lockdep_assert_held(&cpupr_sessions_lock);
	list_for_each_entry(ps, &cpupr_sessions, node) {
		if (ps->incoming && ps->as == as)
			return ps;
	}

	ps = cpupr_session_alloc();
	if (!ps)
		return NULL;

	ps->incoming = true;
	ps->as = as;
	list_add(&ps->node, &cpupr_sessions);
	return ps;
}

/* Map the context block and the stack, but not the guard page between them. */
static int cpupr_map_stack(struct cpupr_session *ps, void *stack)
{
	unsigned long va = (unsigned long)stack;
	phys_addr_t pa = virt_to_phys(stack);
	int ret;

	ret = cpupr_as_map(ps, pa, va, CPUPR_STACK_CTX_SIZE, PAGE_KERNEL);
	if (ret)
		return ret;

	return cpupr_as_map(ps, pa + CPUPR_STACK_BASE, va + CPUPR_STACK_BASE,
			    THREAD_SIZE, PAGE_KERNEL);
}

/**
 * cpupr_unpreserve - Unpreserve a physical CPU and restore it to online state
 * @cpu: Logical CPU identifier.
 *
 * Signals the CPU to exit the parking loop, waits for it to stop, drops it out
 * of the preserved state, and brings it back online through CPU hotplug.  If
 * bringing the CPU online fails after it has stopped, it remains offline so
 * userspace can manage it.  Its stack stays with the address space of its
 * session, which frees it once every CPU of the session has left.
 *
 * Return: 0 on success, or a negative errno if the CPU did not stop.  If it
 *         did not stop, the CPU stays preserved and everything it can reach
 *         stays allocated.
 */
static int cpupr_unpreserve(unsigned int cpu)
{
	struct device *dev = get_cpu_device(cpu);
	int ret;

	scoped_guard(mutex, &cpupr_lock) {
		if (!cpu_is_preserved(cpu))
			return 0;
		if (!dev)
			return -ENODEV;

		cpupr_signal_exit(cpu);
		arch_cpupr_kick(cpu);
	}

	/*
	 * Do not hold cpupr_lock across the wait: it only reads the
	 * CPU's ser, which stays valid for as long as the CPU is preserved.
	 */
	ret = cpupr_wait_dead(cpu);
	if (WARN_ON_ONCE(ret))
		return ret;

	lock_device_hotplug();
	scoped_guard(mutex, &cpupr_lock)
		cpupr_unpreserve_locked(cpu);
	ret = device_online(dev);
	unlock_device_hotplug();

	if (ret < 0) {
		arch_cpupr_wait_dead(cpu, NULL, false);
		pr_warn("Failed to bring unpreserved cpu %u back online: %d\n",
			cpu, ret);
	}
	return 0;
}

/*
 * Allocate and map the preserved stack and descriptor for @cpu in @ps's
 * isolated address space, which owns and frees them when @ps is released.
 */
static struct cpupr_ser *cpupr_alloc_cpu(struct cpupr_session *ps,
					 unsigned int cpu)
{
	struct cpupr_stack_context *sctx;
	struct cpupr_ser *ser;
	void *stack;
	int ret;

	stack = cpupr_as_alloc_folio(ps, CPUPR_STACK_ORDER);
	if (IS_ERR(stack))
		return stack;

	ser = cpupr_as_alloc_buffer(ps, sizeof(*ser));
	if (IS_ERR(ser))
		return ser;

	ret = cpupr_map_stack(ps, stack);
	if (ret)
		return ERR_PTR(ret);

	ser->size = sizeof(*ser);
	ser->hwid = arch_cpupr_hwid(cpu);
	ser->state = CPUPR_PARKING;
	ser->stack_pa = virt_to_phys(stack);
	KHOSER_STORE_PTR(ser->as, ps->as);
	cpupr_clean(ser);

	sctx = stack;
	sctx->magic = CPUPR_STACK_MAGIC;
	sctx->workload_context = 0;
	sctx->session_pgd_pa = ps->as->pgd_pa;
	sctx->ser = ser;
	sctx->entry_fn = NULL;
	cpupr_clean(sctx);

	return ser;
}

/*
 * Hold the device hotplug lock from the online check until the CPU is offline,
 * so that no other path can take the CPU down (it would park on this stack) or
 * bring it up in between.
 */
static int cpupr_offline_cpu(unsigned int cpu, struct device *dev,
			     struct cpupr_session *ps, struct cpupr_ser *ser)
{
	int ret = 0;

	lock_device_hotplug();
	scoped_guard(mutex, &cpupr_lock) {
		if (cpu_is_preserved(cpu) || !cpu_online(cpu)) {
			ret = -EBUSY;
		} else {
			cpumask_set_cpu(cpu, &cpupr_outgoing_mask);
			cpumask_set_cpu(cpu, &cpupr_mask);
			WRITE_ONCE(per_cpu(cpupr_cpu_ser, cpu), ser);
			per_cpu(cpupr_cpu_session, cpu) = ps;
		}
	}
	if (!ret) {
		ret = device_offline(dev);
		if (ret < 0) {
			pr_err("Failed to offline preserved cpu %u: %d\n",
			       cpu, ret);
			scoped_guard(mutex, &cpupr_lock)
				cpupr_unpreserve_locked(cpu);
		}
	}
	unlock_device_hotplug();

	return ret < 0 ? ret : 0;
}

static int cpupr_preserve(unsigned int cpu, struct liveupdate_session *session)
{
	struct device *dev = get_cpu_device(cpu);
	struct cpupr_session *ps;
	struct cpupr_ser *ser;
	int ret;

	if (!dev)
		return -ENODEV;

	/* Do not grow the address space for a CPU that cannot be preserved */
	if (cpu_is_preserved(cpu) || !cpu_online(cpu))
		return -EBUSY;

	ps = cpupr_session_get(session);
	if (IS_ERR(ps))
		return PTR_ERR(ps);

	ser = cpupr_alloc_cpu(ps, cpu);
	if (IS_ERR(ser)) {
		ret = PTR_ERR(ser);
		goto err_put;
	}

	ret = cpupr_offline_cpu(cpu, dev, ps, ser);
	if (ret)
		goto err_put;

	ret = cpupr_wait_parked(cpu);
	if (ret)
		goto err_unpreserve;

	scoped_guard(mutex, &cpupr_sessions_lock)
		cpumask_set_cpu(cpu, &ps->cpus);
	return 0;

err_unpreserve:
	/* A CPU that fails to stop keeps its session alive */
	if (cpupr_unpreserve(cpu)) {
		scoped_guard(mutex, &cpupr_sessions_lock)
			cpumask_set_cpu(cpu, &ps->cpus);
		cpupr_session_detach(ps);
		return ret;
	}
err_put:
	cpupr_session_put_if_empty(ps);
	return ret;
}

static int cpupr_preserve_runtime_buffer(void)
{
	int ret;

	lockdep_assert_held(&cpupr_lock);

	ret = kho_preserve_folio(cpupr_text_folio);
	if (ret)
		return ret;

	ret = kho_preserve_folio(cpupr_data_folio);
	if (ret) {
		kho_unpreserve_folio(cpupr_text_folio);
		return ret;
	}

	return 0;
}

static void cpupr_unpreserve_runtime_buffer(void)
{
	lockdep_assert_held(&cpupr_lock);

	kho_unpreserve_folio(cpupr_text_folio);
	kho_unpreserve_folio(cpupr_data_folio);
}

static int cpupr_flb_preserve(struct liveupdate_flb_op_args *argp)
{
	struct cpupr_global_ser *ser;
	int ret;

	guard(mutex)(&cpupr_lock);
	ret = cpupr_init_runtime_buffer_locked();
	if (ret)
		return ret;

	ret = cpupr_preserve_runtime_buffer();
	if (ret)
		return ret;

	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser)) {
		cpupr_unpreserve_runtime_buffer();
		return PTR_ERR(ser);
	}

	/* The CPUs are linked when LUO freezes their files */
	ser->size = sizeof(*ser);
	ser->text_runtime_pa = PFN_PHYS(folio_pfn(cpupr_text_folio));
	ser->text_runtime_size = folio_size(cpupr_text_folio);
	ser->data_runtime_pa = PFN_PHYS(folio_pfn(cpupr_data_folio));
	ser->data_runtime_size = folio_size(cpupr_data_folio);
	ser->arch_mode = arch_cpupr_mode();
	cpupr_clean(ser);
	cpupr_global_ser = ser;

	argp->data = virt_to_phys(ser);
	argp->obj = ser;
	return 0;
}

static void cpupr_flb_unpreserve(struct liveupdate_flb_op_args *argp)
{
	struct cpupr_global_ser *ser;

	if (!argp->data)
		return;

	ser = phys_to_virt(argp->data);
	scoped_guard(mutex, &cpupr_lock) {
		if (WARN_ON_ONCE(!cpumask_empty(&cpupr_outgoing_mask)))
			return;
		cpupr_global_ser = NULL;
		cpupr_unpreserve_runtime_buffer();
	}

	kho_unpreserve_free(ser);
}

static bool cpupr_runtime_valid(u64 pa, u64 size)
{
	return pa && size && PAGE_ALIGNED(pa) && PAGE_ALIGNED(size);
}

static bool cpupr_ser_is_valid(struct cpupr_ser *ser)
{
	return ser && ser->size == sizeof(*ser) && ser->flags == 0 &&
		ser->reserved == 0 && ser->stack_pa &&
		PAGE_ALIGNED(ser->stack_pa) && READ_ONCE(ser->state) < CPUPR_NR_STATES;
}

static bool cpupr_as_ser_is_valid(struct cpupr_as_ser *as)
{
	bool c = (as && as->size == sizeof(*as) && as->flags == 0 &&
		  as->pgd_pa && PAGE_ALIGNED(as->pgd_pa));

	return c && cpupr_folio_block_ser_is_valid(KHOSER_LOAD_PTR(as->folios));
}

/*
 * Validate @ser and take over the incoming CPU it describes, attaching it to
 * the session of its isolated address space.
 */
static int cpupr_restore_cpu(struct cpupr_ser *ser)
{
	struct cpupr_session *ps;
	int cpu;

	if (!cpupr_ser_is_valid(ser)) {
		luo_restore_fail("invalid cpupr_ser format\n");
		return -EINVAL;
	}

	if (!cpupr_as_ser_is_valid(KHOSER_LOAD_PTR(ser->as))) {
		luo_restore_fail("invalid cpupr_as_ser format\n");
		return -EINVAL;
	}

	cpu = cpupr_find_cpu_by_hwid(ser->hwid);
	if (cpu < 0) {
		luo_restore_fail("no CPU with hardware id 0x%llx\n", ser->hwid);
		return cpu;
	}

	if (cpu_online(cpu)) {
		luo_restore_fail("Preserved cpu %d is on-line\n", cpu);
		return -EBUSY;
	}

	if (cpumask_test_and_set_cpu(cpu, &cpupr_incoming_mask)) {
		luo_restore_fail("double entry for cpu %d (hwid 0x%llx)\n",
				 cpu, ser->hwid);
		return -EEXIST;
	}

	ps = cpupr_incoming_session_get(KHOSER_LOAD_PTR(ser->as));
	if (!ps) {
		luo_restore_fail("failed to allocate incoming session\n");
		return -ENOMEM;
	}

	cpumask_set_cpu(cpu, &ps->cpus);
	per_cpu(cpupr_cpu_ser, cpu) = ser;
	per_cpu(cpupr_cpu_session, cpu) = ps;
	return 0;
}

/*
 * Take over the CPUs listed in @gser, with their sessions.
 */
static int cpupr_restore_cpus(struct cpupr_global_ser *gser)
{
	struct cpupr_ser *pser;
	int ret;

	guard(mutex)(&cpupr_sessions_lock);
	guard(mutex)(&cpupr_lock);

	if (!KHOSER_LOAD_PTR(gser->cpus)) {
		luo_restore_fail("no preserved CPUs listed\n");
		return -EINVAL;
	}

	for (pser = KHOSER_LOAD_PTR(gser->cpus); pser; pser = KHOSER_LOAD_PTR(pser->next)) {
		ret = cpupr_restore_cpu(pser);
		if (ret)
			return ret;
	}

	cpumask_or(&cpupr_mask, &cpupr_mask, &cpupr_incoming_mask);
	return 0;
}

static int cpupr_flb_retrieve(struct liveupdate_flb_op_args *argp)
{
	struct cpupr_arch_mode_ser mode = arch_cpupr_mode();
	struct cpupr_global_ser *ser;
	int ret;

	if (!argp->data) {
		luo_restore_fail("invalid flb data\n");
		return -EINVAL;
	}

	ser = phys_to_virt(argp->data);
	if (ser->size != sizeof(*ser) || ser->flags != 0) {
		luo_restore_fail("invalid cpupr_global_ser\n");
		return -EINVAL;
	}

	if (!cpupr_runtime_valid(ser->text_runtime_pa, ser->text_runtime_size) ||
	    !cpupr_runtime_valid(ser->data_runtime_pa, ser->data_runtime_size)) {
		luo_restore_fail("invalid runtime buffers\n");
		return -EINVAL;
	}

	if (memcmp(&ser->arch_mode, &mode, sizeof(mode))) {
		luo_restore_fail("incompatible arch mode\n");
		return -EINVAL;
	}

	ret = cpupr_restore_cpus(ser);
	if (ret)
		return ret;

	argp->obj = ser;
	return 0;
}

static void cpupr_flb_finish(struct liveupdate_flb_op_args *argp)
{
	struct cpupr_global_ser *ser = argp->obj;

	if (!ser)
		return;

	guard(mutex)(&cpupr_lock);
	if (WARN_ON_ONCE(!cpumask_empty(&cpupr_incoming_mask)))
		return;

	if (ser->text_runtime_pa)
		kho_restore_free(phys_to_virt(ser->text_runtime_pa));
	if (ser->data_runtime_pa)
		kho_restore_free(phys_to_virt(ser->data_runtime_pa));
	kho_restore_free(ser);
}

static const struct liveupdate_flb_ops cpupr_flb_ops = {
	.preserve   = cpupr_flb_preserve,
	.unpreserve = cpupr_flb_unpreserve,
	.retrieve   = cpupr_flb_retrieve,
	.finish     = cpupr_flb_finish,
	.owner      = THIS_MODULE,
};

static struct liveupdate_flb cpupr_flb = {
	.ops        = &cpupr_flb_ops,
	.compatible = CPUPR_LUO_FLB_COMPATIBLE,
};

static ssize_t preserved_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%*pbl\n", cpumask_pr_args(cpu_get_preserved_mask()));
}
static DEVICE_ATTR_RO(preserved);

static struct attribute *cpu_preserve_root_attrs[] = {
	&dev_attr_preserved.attr,
	NULL
};

/* Attributes of the CPU subsystem root, /sys/devices/system/cpu */
const struct attribute_group cpu_preserve_root_attr_group = {
	.attrs = cpu_preserve_root_attrs,
};

static ssize_t preserve_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", cpu_is_preserved(dev->id));
}
static DEVICE_ATTR_RO(preserve);

static struct attribute *cpu_preserve_attrs[] = {
	&dev_attr_preserve.attr,
	NULL
};

/* Attributes of each hotpluggable CPU device */
const struct attribute_group cpu_preserve_attr_group = {
	.attrs = cpu_preserve_attrs,
};

/* Return the CPU of a cpu<N>/preserve sysfs file, or -EINVAL. */
static int file_to_cpu(struct file *file)
{
	struct kernfs_node *kn, *parent;
	int cpu;

	if (!file)
		return -EINVAL;

	kn = kernfs_node_from_dentry(file->f_path.dentry);
	if (!kn || kn->priv != &dev_attr_preserve.attr)
		return -EINVAL;

	parent = kernfs_get_parent(kn);
	if (!parent)
		return -EINVAL;

	cpu = parent->priv ? kobj_to_dev(parent->priv)->id : -EINVAL;
	kernfs_put(parent);
	return cpu;
}

static bool cpupr_can_preserve_cb(struct liveupdate_file_handler *handler,
				      struct file *file)
{
	return file_to_cpu(file) >= 0;
}

struct cpupr_job {
	struct file *mem_file;
	u64 entry_offset;
	u64 arg_offset;
	struct cpupr_job_ctx *ctx;
};

static void cpupr_free_job(struct cpupr_job *job)
{
	if (!job)
		return;

	fput(job->mem_file);
	kfree(job);
}

static int cpupr_parse_job_arg(struct liveupdate_session *s,
			       const struct liveupdate_cpu_preserve_arg *uarg,
			       struct cpupr_job **jobp)
{
	struct file *mem_file;
	struct cpupr_job *job;
	loff_t size;
	int ret;

	if (uarg->flags)
		return -EOPNOTSUPP;

	ret = liveupdate_get_file_outgoing(s, uarg->mem_token, &mem_file);
	if (ret)
		return ret;

	size = i_size_read(file_inode(mem_file));
	if (size <= 0 || !PAGE_ALIGNED(size) ||
	    uarg->entry_offset >= size || uarg->arg_offset >= size ||
	    !mem_file->f_mapping) {
		fput(mem_file);
		return -EINVAL;
	}

	job = kzalloc_obj(*job);
	if (!job) {
		fput(mem_file);
		return -ENOMEM;
	}

	job->mem_file = mem_file;
	job->entry_offset = uarg->entry_offset;
	job->arg_offset = uarg->arg_offset;
	*jobp = job;
	return 0;
}

static phys_addr_t cpupr_job_offset_pa(struct address_space *mapping,
				       u64 offset)
{
	pgoff_t pgoff = offset >> PAGE_SHIFT;
	struct folio *folio = filemap_get_folio(mapping, pgoff);
	phys_addr_t pa;

	if (IS_ERR(folio))
		return 0;

	pa = PFN_PHYS(folio_pfn(folio) + (pgoff - folio->index)) +
	     offset_in_page(offset);
	folio_put(folio);
	return pa;
}

static int cpupr_setup_job(unsigned int cpu, struct cpupr_job *job)
{
	struct address_space *mapping = job->mem_file->f_mapping;
	struct cpupr_session *ps = cpupr_session_of(cpu);
	loff_t size = i_size_read(file_inode(job->mem_file));
	pgoff_t index = 0, end_index = size >> PAGE_SHIFT;
	phys_addr_t entry_pa, arg_pa;
	struct cpupr_job_ctx *ctx;
	int ret;

	if (!ps || !mapping)
		return -EINVAL;

	ctx = cpupr_as_alloc_buffer(ps, sizeof(*ctx));
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	if (ps->mapped_mem_file != job->mem_file) {
		while (index < end_index) {
			struct folio *folio = filemap_get_folio(mapping, index);
			phys_addr_t pa;
			size_t fsize;

			if (IS_ERR(folio))
				return -EINVAL;

			pa = PFN_PHYS(folio_pfn(folio));
			fsize = folio_size(folio);
			index = folio->index + folio_nr_pages(folio);
			folio_put(folio);

			ret = cpupr_as_map(ps, pa, pa, fsize, PAGE_KERNEL_EXEC);
			if (ret)
				return ret;
		}
		ps->mapped_mem_file = job->mem_file;
	}

	entry_pa = cpupr_job_offset_pa(mapping, job->entry_offset);
	arg_pa = cpupr_job_offset_pa(mapping, job->arg_offset);
	if (!entry_pa || !arg_pa)
		return -EINVAL;

	ctx->entry_pa = entry_pa;
	ctx->arg_pa = arg_pa;
	cpupr_clean(ctx);
	job->ctx = ctx;
	return 0;
}

static void cpupr_clean_job_mem(struct cpupr_job *job)
{
	struct address_space *mapping = job->mem_file->f_mapping;
	loff_t size = i_size_read(file_inode(job->mem_file));
	pgoff_t index = 0, end_index = size >> PAGE_SHIFT;

	while (index < end_index) {
		struct folio *folio = filemap_get_folio(mapping, index);

		if (IS_ERR(folio))
			break;

		cpupr_clean_sz(folio_address(folio), folio_size(folio));
		index = folio->index + folio_nr_pages(folio);
		folio_put(folio);
	}
}

static void cpupr_release_cpu(unsigned int cpu)
{
	struct cpupr_session *ps = cpupr_session_of(cpu);

	if (cpupr_unpreserve(cpu)) {
		cpupr_session_detach(ps);
		return;
	}

	cpupr_session_remove_cpu(ps, cpu);
}

static int cpupr_preserve_cb(struct liveupdate_file_op_args *args)
{
	struct cpupr_job *job = NULL;
	int cpu, ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	cpu = file_to_cpu(args->file);
	if (cpu < 0)
		return cpu;

	if (args->arg) {
		ret = cpupr_parse_job_arg(args->session, args->arg, &job);
		if (ret)
			return ret;
	}

	ret = cpupr_preserve(cpu, args->session);
	if (ret) {
		cpupr_free_job(job);
		return ret;
	}

	if (job) {
		ret = cpupr_setup_job(cpu, job);
		if (ret) {
			cpupr_release_cpu(cpu);
			cpupr_free_job(job);
			return ret;
		}
	}

	args->private_data = job;
	args->serialized_data = virt_to_phys(cpupr_get_ser(cpu));
	return 0;
}

static void cpupr_unpreserve_cb(struct liveupdate_file_op_args *args)
{
	struct cpupr_session *ps;
	struct cpupr_ser *ser;
	int cpu;

	if (!args->serialized_data)
		return;

	ser = phys_to_virt(args->serialized_data);
	cpu = cpupr_find_cpu_by_hwid(ser->hwid);
	if (WARN_ON_ONCE(cpu < 0))
		return;

	if (args->private_data) {
		ps = cpupr_session_of(cpu);
		if (ps) {
			ps->mapped_mem_file = NULL;
			ps->cleaned_mem_file = NULL;
		}
	}
	cpupr_release_cpu(cpu);
	cpupr_free_job(args->private_data);
}

/*
 * Put the CPU of a frozen file on the list of CPUs handed over to the next
 * kernel, in O(1), and start its user job if one was configured.
 */
static int cpupr_freeze_cb(struct liveupdate_file_op_args *args)
{
	struct cpupr_job *job = args->private_data;
	struct cpupr_global_ser *gser;
	struct cpupr_ser *ser, *first;
	int cpu, ret;

	if (!args->serialized_data)
		return -EINVAL;

	ser = phys_to_virt(args->serialized_data);
	cpu = cpupr_find_cpu_by_hwid(ser->hwid);

	scoped_guard(mutex, &cpupr_lock) {
		gser = cpupr_global_ser;
		if (WARN_ON_ONCE(!gser || cpu < 0 ||
				 !cpumask_test_cpu(cpu, &cpupr_outgoing_mask) ||
				 per_cpu(cpupr_cpu_ser, cpu) != ser ||
				 cpupr_is_frozen(cpu)))
			return -EINVAL;

		/* Insert @ser at the head of the list, see cpupr_handover_del() */
		first = KHOSER_LOAD_PTR(gser->cpus);
		ser->next.phys = gser->cpus.phys;
		if (first) {
			int first_cpu = cpupr_find_cpu_by_hwid(first->hwid);

			if (!WARN_ON_ONCE(first_cpu < 0))
				per_cpu(cpupr_cpu_link, first_cpu) = &ser->next.phys;
		}
		KHOSER_STORE_PTR(gser->cpus, ser);
		per_cpu(cpupr_cpu_link, cpu) = &gser->cpus.phys;
		cpupr_clean(ser);
		cpupr_clean(gser);
	}

	if (job) {
		struct cpupr_session *ps = cpupr_session_of(cpu);

		if (!ps || ps->cleaned_mem_file != job->mem_file) {
			cpupr_clean_job_mem(job);
			if (ps)
				ps->cleaned_mem_file = job->mem_file;
		}
		ret = cpupr_attach_workload(cpu, cpupr_job_entry, job->ctx);
		if (ret) {
			guard(mutex)(&cpupr_lock);
			cpupr_handover_del(cpu);
			return ret;
		}
	}

	return 0;
}

static void cpupr_unfreeze_cb(struct liveupdate_file_op_args *args)
{
	struct cpupr_job *job = args->private_data;
	struct cpupr_ser *ser;
	int cpu;

	if (!args->serialized_data)
		return;

	ser = phys_to_virt(args->serialized_data);
	cpu = cpupr_find_cpu_by_hwid(ser->hwid);
	if (WARN_ON_ONCE(cpu < 0))
		return;

	if (job) {
		struct cpupr_session *ps = cpupr_session_of(cpu);

		if (ps)
			ps->cleaned_mem_file = NULL;
		cpupr_detach_workload(cpu);
	}

	guard(mutex)(&cpupr_lock);
	if (cpupr_is_frozen(cpu) &&
	    !WARN_ON_ONCE(per_cpu(cpupr_cpu_ser, cpu) != ser))
		cpupr_handover_del(cpu);
}

/* The CPU that the previous kernel handed over with @ser, or -ENOENT */
static int cpupr_incoming_cpu(u64 data)
{
	struct cpupr_ser *ser;
	int cpu;

	if (!data)
		return -ENOENT;

	ser = phys_to_virt(data);
	guard(mutex)(&cpupr_lock);
	cpu = cpupr_find_cpu_by_hwid(ser->hwid);
	if (cpu >= 0 && cpumask_test_cpu(cpu, &cpupr_incoming_mask) &&
	    per_cpu(cpupr_cpu_ser, cpu) == ser)
		return cpu;

	pr_err("preserved CPU state 0x%llx not in handover data\n", data);
	return -ENOENT;
}

static int cpupr_retrieve_cb(struct liveupdate_file_op_args *args)
{
	struct file *file;
	char path[64];
	int cpu;

	cpu = cpupr_incoming_cpu(args->serialized_data);
	if (cpu < 0)
		return cpu;

	/* Open the attribute under the root of init, not of the caller. */
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/preserve", cpu);
	scoped_with_init_fs()
		file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);

	if (file_to_cpu(file) != cpu) {
		fput(file);
		return -ENOENT;
	}

	args->file = file;
	return 0;
}

static void cpupr_finish_cb(struct liveupdate_file_op_args *args)
{
	struct cpupr_session *ps;
	int cpu;

	cpu = cpupr_incoming_cpu(args->serialized_data);
	if (cpu < 0)
		return;

	ps = cpupr_session_of(cpu);
	if (cpupr_unpreserve(cpu))
		return;

	cpupr_session_remove_cpu(ps, cpu);
}

static const struct liveupdate_file_ops cpu_preserve_file_ops = {
	.can_preserve = cpupr_can_preserve_cb,
	.preserve     = cpupr_preserve_cb,
	.retrieve     = cpupr_retrieve_cb,
	.unpreserve   = cpupr_unpreserve_cb,
	.freeze       = cpupr_freeze_cb,
	.unfreeze     = cpupr_unfreeze_cb,
	.finish       = cpupr_finish_cb,
	.parg         = LIVEUPDATE_ARG(struct liveupdate_cpu_preserve_arg,
				       arg_offset),
	.owner        = THIS_MODULE,
};

static struct liveupdate_file_handler cpu_preserve_handler = {
	.ops        = &cpu_preserve_file_ops,
	.compatible = CPUPR_LUO_FH_COMPATIBLE,
};

static int cpupr_reboot_notify(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	bool handover = kexec_in_progress && liveupdate_enabled();
	ktime_t deadline;
	int cpu;

	/*
	 * Only the CPUs on the handover list, whose files LUO froze, are handed
	 * over to the next kernel.  Stop every other preserved CPU, including
	 * incoming CPUs that no session finished, as their memory is not
	 * preserved again.  Stopping is all that is done: no owner unpreserved,
	 * retrieved or finished them, so neither does this.  They halt like any
	 * offline CPU across kexec, and the next kernel brings them up as usual.
	 */
	guard(mutex)(&cpupr_lock);
	for_each_cpu(cpu, &cpupr_mask) {
		if (handover && cpupr_is_frozen(cpu))
			continue;

		cpupr_signal_exit(cpu);
		arch_cpupr_kick(cpu);
	}

	deadline = ktime_add_us(ktime_get(), CPUPR_WAIT_DEAD_TIMEOUT_US);
	for_each_cpu(cpu, &cpupr_mask) {
		s64 left;

		if (handover && cpupr_is_frozen(cpu))
			continue;

		left = max(ktime_us_delta(deadline, ktime_get()), 1);
		if (!cpupr_wait_dead_timeout(cpu, left))
			cpumask_clear_cpu(cpu, &cpupr_mask);
	}

	return NOTIFY_OK;
}

static struct notifier_block cpupr_reboot_nb = {
	.notifier_call = cpupr_reboot_notify,
};

static int cpupr_pm_notify(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	if (action != PM_SUSPEND_PREPARE && action != PM_HIBERNATION_PREPARE)
		return NOTIFY_DONE;

	/*
	 * Sleep states reset the CPUs, parked ones included.  Lockless read:
	 * cpupr_preserve() sets cpupr_mask before offlining, and PM suspend
	 * blocks CPU hotplug once preparation completes.
	 */
	if (!cpumask_empty(&cpupr_mask)) {
		pr_err("Cannot suspend or hibernate while CPUs are preserved\n");
		return notifier_from_errno(-EBUSY);
	}
	return NOTIFY_DONE;
}

static struct notifier_block cpupr_pm_nb = {
	.notifier_call = cpupr_pm_notify,
};

static int __init cpu_preserve_early_init(void)
{
	void *obj;
	int err;

	if (!liveupdate_enabled())
		return 0;

	arch_cpupr_early_init();

	err = liveupdate_register_file_handler(&cpu_preserve_handler);
	if (err) {
		pr_err("Could not register cpupr_preserve file handler: %pe\n",
		       ERR_PTR(err));
		return err;
	}

	err = liveupdate_register_flb(&cpu_preserve_handler,
				      &cpupr_flb);
	if (err) {
		pr_err("Could not register cpu_preserve FLB: %pe\n",
		       ERR_PTR(err));
		liveupdate_unregister_file_handler(&cpu_preserve_handler);
		return err;
	}

	/* Retrieve incoming preserved CPUs before secondary CPU bringup */
	if (!liveupdate_flb_get_incoming(&cpupr_flb, &obj))
		liveupdate_flb_put_incoming(&cpupr_flb);

	register_reboot_notifier(&cpupr_reboot_nb);
	register_pm_notifier(&cpupr_pm_nb);

	return 0;
}
early_initcall(cpu_preserve_early_init);
