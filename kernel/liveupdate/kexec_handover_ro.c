// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_handover_ro.c - Read-Only Kexec Handover backend
 *
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Implements the read-only KHO backend (kho_ro=<pa>). In read-only mode, KHO
 * is not enabled for outgoing or incoming page ownership transfer and makes no
 * changes to boot or struct page initialization. Instead, when subsystems
 * retrieve preserved state, this backend traverses the KHO FDT, radix tree,
 * and preserved metadata by mapping physical pages read-only (PAGE_KERNEL_RO)
 * without modifying or freeing the KHO metadata or reclaiming the preserved
 * pages.
 *
 * The preserved KHO FDT, radix tree, and folio/vmalloc metadata are assumed to
 * remain static for the lifetime of the read-only KHO kernel: the FDT is
 * mapped at early_initcall time, and radix-tree nodes, leaves, and
 * folio/vmalloc mappings are cached on first access. If a caller needs
 * read-write access to preserved folio contents outside of KHO metadata, it
 * can map them with PAGE_KERNEL via kho_ro_map_folio().
 */

#define pr_fmt(fmt) "KHO-RO: " fmt

#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <asm/setup.h>

#include "kexec_handover_internal.h"

#define KHO_RO_MAP_HASH_BITS 10

/*
 * In read-only KHO mode, the preserved KHO tree and memory must reside outside
 * of the kernel's direct map so they do not conflict with the kernel that owns
 * the KHO state. Virtual mappings used to access the tree and preserved memory
 * are stored in an RCU-protected hash table (kho_ro_map_ht) that remains
 * static for the lifetime of the kernel and is never unmapped, allowing fast
 * lockless multi-threaded lookups under rcu_read_lock() during traversal while
 * serializing insertions on cache misses with kho_ro_lock.
 *
 * Entries are keyed by (pa, size, prot):
 * - Radix-tree nodes/leaves and preserved folios use their physical address,
 *   mapped byte size (PAGE_SIZE << order), and pgprot_t.
 * - Preserved kho_vmalloc regions use preservation->first.phys with size = 0
 *   and PAGE_KERNEL_RO so a discontiguous vmalloc mapping never collides with
 *   a direct page/folio mapping (which always has size > 0).
 */
struct kho_ro_map_entry {
	struct hlist_node node;
	phys_addr_t pa;
	size_t size;
	pgprot_t prot;
	void *vaddr;
};

static bool kho_ro __ro_after_init;
static phys_addr_t kho_ro_fdt_phys __ro_after_init;
static phys_addr_t kho_ro_mem_map_phys __ro_after_init;
static const void *kho_ro_fdt __ro_after_init;
static DEFINE_MUTEX(kho_ro_lock);
static DEFINE_HASHTABLE(kho_ro_map_ht, KHO_RO_MAP_HASH_BITS);

/**
 * kho_is_ro - Check if KHO is operating in read-only mode.
 *
 * Return: true if read-only KHO mode is active, false otherwise.
 */
bool kho_is_ro(void)
{
	return kho_ro;
}
EXPORT_SYMBOL_GPL(kho_is_ro);

static int __init kho_parse_ro(char *p)
{
	unsigned long long pa;

	if (!p || kstrtoull(p, 0, &pa) || !pa || !PAGE_ALIGNED(pa))
		return -EINVAL;

	kho_ro_fdt_phys = pa;
	kho_ro = true;
	pr_info_once("Read-only mode enabled with FDT at %pa\n",
		     &kho_ro_fdt_phys);
	return 0;
}
early_param("kho_ro", kho_parse_ro);

static int __init kho_ro_early_parse(char *param, char *val,
				     const char *unused, void *arg)
{
	if (parameq(param, "kho_ro"))
		kho_parse_ro(val);
	return 0;
}

bool __init kho_ro_cmdline_enabled(void)
{
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

	if (!kho_ro) {
		strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
		parse_args("kho_ro", tmp_cmdline, NULL, 0, 0, 0, NULL,
			   kho_ro_early_parse);
	}

	return kho_ro;
}

static int kho_ro_map_range(unsigned long vaddr, phys_addr_t pa, size_t size,
			    pgprot_t prot)
{
	if (!pa || !size || !PAGE_ALIGNED(pa | size) || pa + size - 1 < pa)
		return -EINVAL;

	if (region_intersects(pa, size, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT) {
		pr_warn_once("Cannot map System RAM at %pa (size 0x%zx)\n",
			     &pa, size);
		return -EINVAL;
	}

	return vmap_page_range(vaddr, vaddr + size, pa, prot);
}

static void *kho_ro_map(phys_addr_t pa, size_t size, pgprot_t prot)
{
	struct vm_struct *area;

	area = get_vm_area_caller(size, VM_IOREMAP, __builtin_return_address(0));
	if (!area)
		return NULL;

	area->phys_addr = pa;
	if (kho_ro_map_range((unsigned long)area->addr, pa, size, prot)) {
		vunmap(area->addr);
		return NULL;
	}

	return area->addr;
}

static int __init kho_ro_init(void)
{
	phys_addr_t mem_map_phys;
	const void *fdt;

	if (!kho_ro)
		return 0;

	fdt = kho_ro_map(kho_ro_fdt_phys, PAGE_SIZE, PAGE_KERNEL_RO);
	if (!fdt) {
		pr_err("Failed to map FDT at %pa\n", &kho_ro_fdt_phys);
		goto err_disable;
	}

	if (kho_check_fdt(fdt, kho_ro_fdt_phys))
		goto err_unmap;

	mem_map_phys = kho_get_mem_map_phys(fdt);
	if (!mem_map_phys)
		goto err_unmap;

	kho_ro_fdt = fdt;
	kho_ro_mem_map_phys = mem_map_phys;
	return 0;

err_unmap:
	vunmap(fdt);
err_disable:
	pr_warn("Disabling read-only mode\n");
	kho_ro = false;
	return -EINVAL;
}
early_initcall(kho_ro_init);

const void *kho_ro_get_fdt(void)
{
	return kho_ro_fdt;
}

static void *kho_ro_lookup_cached(phys_addr_t pa, size_t size, pgprot_t prot)
{
	struct kho_ro_map_entry *entry;
	void *vaddr = NULL;

	rcu_read_lock();
	hash_for_each_possible_rcu(kho_ro_map_ht, entry, node, pa) {
		if (entry->pa == pa && entry->size == size &&
		    pgprot_val(entry->prot) == pgprot_val(prot)) {
			vaddr = entry->vaddr;
			break;
		}
	}
	rcu_read_unlock();

	return vaddr;
}

static void *kho_ro_insert_cached(phys_addr_t pa, size_t size, pgprot_t prot,
				  void *vaddr)
{
	struct kho_ro_map_entry *entry;
	void *existing;

	guard(mutex)(&kho_ro_lock);
	existing = kho_ro_lookup_cached(pa, size, prot);
	if (existing)
		return existing;

	entry = kzalloc_obj(*entry);
	if (!entry)
		return NULL;

	entry->pa = pa;
	entry->size = size;
	entry->prot = prot;
	entry->vaddr = vaddr;
	hash_add_rcu(kho_ro_map_ht, &entry->node, pa);

	return vaddr;
}

static void *kho_ro_map_cached(phys_addr_t pa, size_t size, pgprot_t prot)
{
	void *vaddr, *cached;

	vaddr = kho_ro_lookup_cached(pa, size, prot);
	if (vaddr)
		return vaddr;

	vaddr = kho_ro_map(pa, size, prot);
	if (!vaddr)
		return NULL;

	cached = kho_ro_insert_cached(pa, size, prot, vaddr);
	if (cached != vaddr)
		vunmap(vaddr);

	return cached;
}

static void *kho_ro_map_radix(phys_addr_t pa)
{
	return kho_ro_map_cached(pa, PAGE_SIZE, PAGE_KERNEL_RO);
}

static bool kho_ro_radix_key_exists(unsigned long key)
{
	const struct kho_radix_leaf *leaf;

	leaf = kho_radix_find_leaf(kho_ro_mem_map_phys, key, kho_ro_map_radix);
	if (!leaf)
		return false;

	return test_bit(kho_radix_get_bitmap_index(key), leaf->bitmap);
}

/**
 * kho_ro_lookup_folio_order - Look up the preserved order of a physical page in KHO.
 * @pa: Page-aligned physical address of the preserved folio.
 *
 * Queries the preserved KHO memory radix tree in read-only mode to determine
 * the order at which @pa was preserved.
 *
 * Return: Non-negative page order on success, -ENOENT if @pa is not preserved,
 * or -EOPNOTSUPP if read-only KHO is not active.
 */
int kho_ro_lookup_folio_order(phys_addr_t pa)
{
	unsigned int order, max_order;

	might_sleep();
	if (!kho_ro)
		return -EOPNOTSUPP;
	if (!pa || !PAGE_ALIGNED(pa) || !kho_ro_mem_map_phys)
		return -ENOENT;

	max_order = min_t(unsigned int, __ffs64(pa >> PAGE_SHIFT),
			  KHO_RADIX_KEY_WIDTH - 2);

	for (order = 0; order <= max_order; order++) {
		if (kho_ro_radix_key_exists(kho_encode_radix_key(pa, order)))
			return (int)order;
	}

	return -ENOENT;
}
EXPORT_SYMBOL_GPL(kho_ro_lookup_folio_order);

/**
 * kho_ro_map_folio - Map a preserved KHO folio into kernel virtual address space.
 * @pa:   Page-aligned physical address of the preserved folio.
 * @prot: Page protection flags for the mapping (e.g. PAGE_KERNEL_RO or PAGE_KERNEL).
 *
 * Looks up @pa in the preserved KHO radix tree to determine its folio order,
 * maps the entire folio (PAGE_SIZE << order) into kernel virtual address
 * space with @prot permissions, and caches the mapping (keyed by @pa, size,
 * and @prot) for the lifetime of the read-only KHO kernel.
 *
 * Return: Virtual pointer to the mapped folio on success, or NULL on failure.
 */
void *kho_ro_map_folio(phys_addr_t pa, pgprot_t prot)
{
	int order;

	might_sleep();
	order = kho_ro_lookup_folio_order(pa);
	if (order < 0)
		return NULL;

	return kho_ro_map_cached(pa, PAGE_SIZE << order, prot);
}
EXPORT_SYMBOL_GPL(kho_ro_map_folio);

struct kho_ro_vmalloc_ctx {
	unsigned long vaddr;
	size_t contig_size;
	unsigned int order;
};

static const struct kho_vmalloc_chunk *kho_ro_vmalloc_get_chunk(phys_addr_t pa)
{
	if (kho_ro_lookup_folio_order(pa) != 0)
		return NULL;

	return kho_ro_map(pa, PAGE_SIZE, PAGE_KERNEL_RO);
}

static void kho_ro_vmalloc_put_chunk(const struct kho_vmalloc_chunk *chunk)
{
	vunmap(chunk);
}

static int kho_ro_vmalloc_map_entry(phys_addr_t pa, void *priv)
{
	struct kho_ro_vmalloc_ctx *ctx = priv;
	int err;

	if (kho_ro_lookup_folio_order(pa) != ctx->order)
		return -EINVAL;

	err = kho_ro_map_range(ctx->vaddr, pa, ctx->contig_size,
			       PAGE_KERNEL_RO);
	if (err)
		return err;

	ctx->vaddr += ctx->contig_size;
	return 0;
}

static const struct kho_vmalloc_walk_ops kho_ro_vmalloc_ops = {
	.get_chunk = kho_ro_vmalloc_get_chunk,
	.put_chunk = kho_ro_vmalloc_put_chunk,
	.entry = kho_ro_vmalloc_map_entry,
};

/**
 * kho_ro_map_vmalloc - Map a preserved vmalloc region read-only in place.
 * @preservation: Pointer to the KHO vmalloc preservation metadata.
 *
 * Allocates a VM_IOREMAP virtual address range, validates each chunk header and
 * preserved page order in the cached KHO radix tree, maps the preserved
 * physical pages directly with read-only permissions (PAGE_KERNEL_RO) with
 * zero RAM allocation or memcpy overhead, and caches the mapping for the
 * lifetime of the read-only KHO kernel.
 *
 * Return: Read-only virtual pointer on success, or NULL on failure.
 */
const void *kho_ro_map_vmalloc(const struct kho_vmalloc *preservation)
{
	struct kho_ro_vmalloc_ctx ctx;
	struct vm_struct *area;
	unsigned int vm_flags;
	size_t total_size;
	void *cached;

	might_sleep();
	if (!kho_ro || !kho_vmalloc_validate(preservation, &vm_flags))
		return NULL;

	/* Use size 0 in the cache key to distinguish vmalloc from folios. */
	cached = kho_ro_lookup_cached(preservation->first.phys, 0,
				      PAGE_KERNEL_RO);
	if (cached)
		return cached;

	total_size = (size_t)preservation->total_pages * PAGE_SIZE;
	area = get_vm_area_caller(total_size, VM_IOREMAP,
				  __builtin_return_address(0));
	if (!area)
		return NULL;

	ctx = (struct kho_ro_vmalloc_ctx){
		.vaddr = (unsigned long)area->addr,
		.contig_size = PAGE_SIZE << preservation->order,
		.order = preservation->order,
	};

	if (kho_vmalloc_walk(preservation, &kho_ro_vmalloc_ops, &ctx)) {
		vunmap(area->addr);
		return NULL;
	}

	cached = kho_ro_insert_cached(preservation->first.phys, 0,
				      PAGE_KERNEL_RO, area->addr);
	if (cached != area->addr)
		vunmap(area->addr);

	return cached;
}
EXPORT_SYMBOL_GPL(kho_ro_map_vmalloc);
