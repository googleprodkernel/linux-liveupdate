// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_handover.c - kexec handover metadata processing
 * Copyright (C) 2023 Alexander Graf <graf@amazon.com>
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (C) 2025 Google LLC, Changyuan Lyu <changyuanl@google.com>
 */

#define pr_fmt(fmt) "KHO: " fmt

#include <linux/cma.h>
#include <linux/count_zeros.h>
#include <linux/kexec.h>
#include <linux/kexec_handover.h>
#include <linux/libfdt.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/page-isolation.h>

#include <asm/early_ioremap.h>

/*
 * KHO is tightly coupled with mm init and needs access to some of mm
 * internal APIs.
 */
#include "../../mm/internal.h"
#include "../kexec_internal.h"
#include "kexec_handover_internal.h"

#define KHO_FDT_COMPATIBLE "kho-v1"
#define PROP_PRESERVED_MEMORY_MAP "preserved-memory-map"
#define PROP_SUB_FDT "fdt"

static bool kho_enable __ro_after_init;

bool kho_is_enabled(void)
{
	return kho_enable;
}
EXPORT_SYMBOL_GPL(kho_is_enabled);

static int __init kho_parse_enable(char *p)
{
	return kstrtobool(p, &kho_enable);
}
early_param("kho", kho_parse_enable);

/*
 * Keep track of memory that is to be preserved across KHO.
 *
 * The serializing side uses two levels of xarrays to manage chunks of per-order
 * 512 byte bitmaps. For instance if PAGE_SIZE = 4096, the entire 1G order of a
 * 1TB system would fit inside a single 512 byte bitmap. For order 0 allocations
 * each bitmap will cover 16M of address space. Thus, for 16G of memory at most
 * 512K of bitmap memory will be needed for order 0.
 *
 * This approach is fully incremental, as the serialization progresses folios
 * can continue be aggregated to the tracker. The final step, immediately prior
 * to kexec would serialize the xarray information into a linked list for the
 * successor kernel to parse.
 */

#define PRESERVE_BITS (512 * 8)

struct kho_mem_phys_bits {
	DECLARE_BITMAP(preserve, PRESERVE_BITS);
};

struct kho_mem_phys {
	/*
	 * Points to kho_mem_phys_bits, a sparse bitmap array. Each bit is sized
	 * to order.
	 */
	struct xarray phys_bits;
};

struct kho_mem_track {
	/* Points to kho_mem_phys, each order gets its own bitmap tree */
	struct xarray orders;
};

struct khoser_mem_chunk;

struct kho_sub_fdt {
	struct list_head l;
	const char *name;
	void *fdt;
};

struct kho_out {
	void *fdt;
	bool finalized;
	struct mutex lock; /* protects KHO FDT finalization */

	struct list_head sub_fdts;
	struct mutex fdts_lock;

	struct kho_mem_track track;
	/* First chunk of serialized preserved memory map */
	struct khoser_mem_chunk *preserved_mem_map;

	struct kho_debugfs dbg;
};

static struct kho_out kho_out = {
	.lock = __MUTEX_INITIALIZER(kho_out.lock),
	.track = {
		.orders = XARRAY_INIT(kho_out.track.orders, 0),
	},
	.sub_fdts = LIST_HEAD_INIT(kho_out.sub_fdts),
	.fdts_lock = __MUTEX_INITIALIZER(kho_out.fdts_lock),
	.finalized = false,
};

static void *xa_load_or_alloc(struct xarray *xa, unsigned long index, size_t sz)
{
	void *elm, *res;

	elm = xa_load(xa, index);
	if (elm)
		return elm;

	elm = kzalloc(sz, GFP_KERNEL);
	if (!elm)
		return ERR_PTR(-ENOMEM);

	res = xa_cmpxchg(xa, index, NULL, elm, GFP_KERNEL);
	if (xa_is_err(res))
		res = ERR_PTR(xa_err(res));

	if (res) {
		kfree(elm);
		return res;
	}

	return elm;
}

static void __kho_unpreserve_order(struct kho_mem_track *track, unsigned long pfn,
				   unsigned int order)
{
	struct kho_mem_phys_bits *bits;
	struct kho_mem_phys *physxa;
	const unsigned long pfn_high = pfn >> order;

	physxa = xa_load(&track->orders, order);
	if (!physxa)
		return;

	bits = xa_load(&physxa->phys_bits, pfn_high / PRESERVE_BITS);
	if (!bits)
		return;

	clear_bit(pfn_high % PRESERVE_BITS, bits->preserve);
}

static void __kho_unpreserve(struct kho_mem_track *track, unsigned long pfn,
			     unsigned long end_pfn)
{
	unsigned int order;

	while (pfn < end_pfn) {
		order = min(count_trailing_zeros(pfn), ilog2(end_pfn - pfn));

		__kho_unpreserve_order(track, pfn, order);

		pfn += 1 << order;
	}
}

static int __kho_preserve_order(struct kho_mem_track *track, unsigned long pfn,
				unsigned int order)
{
	struct kho_mem_phys_bits *bits;
	struct kho_mem_phys *physxa, *new_physxa;
	const unsigned long pfn_high = pfn >> order;

	might_sleep();

	physxa = xa_load(&track->orders, order);
	if (!physxa) {
		int err;

		new_physxa = kzalloc(sizeof(*physxa), GFP_KERNEL);
		if (!new_physxa)
			return -ENOMEM;

		xa_init(&new_physxa->phys_bits);
		physxa = xa_cmpxchg(&track->orders, order, NULL, new_physxa,
				    GFP_KERNEL);

		err = xa_err(physxa);
		if (err || physxa) {
			xa_destroy(&new_physxa->phys_bits);
			kfree(new_physxa);

			if (err)
				return err;
		} else {
			physxa = new_physxa;
		}
	}

	bits = xa_load_or_alloc(&physxa->phys_bits, pfn_high / PRESERVE_BITS,
				sizeof(*bits));
	if (IS_ERR(bits))
		return PTR_ERR(bits);

	set_bit(pfn_high % PRESERVE_BITS, bits->preserve);

	return 0;
}

/* almost as free_reserved_page(), just don't free the page */
static void kho_restore_page(struct page *page, unsigned int order)
{
	unsigned int nr_pages = (1 << order);

	/* Head page gets refcount of 1. */
	set_page_count(page, 1);

	/* For higher order folios, tail pages get a page count of zero. */
	for (unsigned int i = 1; i < nr_pages; i++)
		set_page_count(page + i, 0);

	if (order > 0)
		prep_compound_page(page, order);

	adjust_managed_page_count(page, nr_pages);
}

/**
 * kho_restore_folio - recreates the folio from the preserved memory.
 * @phys: physical address of the folio.
 *
 * Return: pointer to the struct folio on success, NULL on failure.
 */
struct folio *kho_restore_folio(phys_addr_t phys)
{
	struct page *page = pfn_to_online_page(PHYS_PFN(phys));
	unsigned long order;

	if (!page)
		return NULL;

	order = page->private;
	if (order > MAX_PAGE_ORDER)
		return NULL;

	kho_restore_page(page, order);
	return page_folio(page);
}
EXPORT_SYMBOL_GPL(kho_restore_folio);

/* Serialize and deserialize struct kho_mem_phys across kexec
 *
 * Record all the bitmaps in a linked list of pages for the next kernel to
 * process. Each chunk holds bitmaps of the same order and each block of bitmaps
 * starts at a given physical address. This allows the bitmaps to be sparse. The
 * xarray is used to store them in a tree while building up the data structure,
 * but the KHO successor kernel only needs to process them once in order.
 *
 * All of this memory is normal kmalloc() memory and is not marked for
 * preservation. The successor kernel will remain isolated to the scratch space
 * until it completes processing this list. Once processed all the memory
 * storing these ranges will be marked as free.
 */

struct khoser_mem_bitmap_ptr {
	phys_addr_t phys_start;
	DECLARE_KHOSER_PTR(bitmap, struct kho_mem_phys_bits *);
};

struct khoser_mem_chunk_hdr {
	DECLARE_KHOSER_PTR(next, struct khoser_mem_chunk *);
	unsigned int order;
	unsigned int num_elms;
};

#define KHOSER_BITMAP_SIZE                                   \
	((PAGE_SIZE - sizeof(struct khoser_mem_chunk_hdr)) / \
	 sizeof(struct khoser_mem_bitmap_ptr))

struct khoser_mem_chunk {
	struct khoser_mem_chunk_hdr hdr;
	struct khoser_mem_bitmap_ptr bitmaps[KHOSER_BITMAP_SIZE];
};

static_assert(sizeof(struct khoser_mem_chunk) == PAGE_SIZE);

static struct khoser_mem_chunk *new_chunk(struct khoser_mem_chunk *cur_chunk,
					  unsigned long order)
{
	struct khoser_mem_chunk *chunk;

	chunk = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!chunk)
		return NULL;
	chunk->hdr.order = order;
	if (cur_chunk)
		KHOSER_STORE_PTR(cur_chunk->hdr.next, chunk);
	return chunk;
}

static void kho_mem_ser_free(struct khoser_mem_chunk *first_chunk)
{
	struct khoser_mem_chunk *chunk = first_chunk;

	while (chunk) {
		struct khoser_mem_chunk *tmp = chunk;

		chunk = KHOSER_LOAD_PTR(chunk->hdr.next);
		kfree(tmp);
	}
}

static int kho_mem_serialize(struct kho_out *kho_out)
{
	struct khoser_mem_chunk *first_chunk = NULL;
	struct khoser_mem_chunk *chunk = NULL;
	struct kho_mem_phys *physxa;
	unsigned long order;

	xa_for_each(&kho_out->track.orders, order, physxa) {
		struct kho_mem_phys_bits *bits;
		unsigned long phys;

		chunk = new_chunk(chunk, order);
		if (!chunk)
			goto err_free;

		if (!first_chunk)
			first_chunk = chunk;

		xa_for_each(&physxa->phys_bits, phys, bits) {
			struct khoser_mem_bitmap_ptr *elm;

			if (chunk->hdr.num_elms == ARRAY_SIZE(chunk->bitmaps)) {
				chunk = new_chunk(chunk, order);
				if (!chunk)
					goto err_free;
			}

			elm = &chunk->bitmaps[chunk->hdr.num_elms];
			chunk->hdr.num_elms++;
			elm->phys_start = (phys * PRESERVE_BITS)
					  << (order + PAGE_SHIFT);
			KHOSER_STORE_PTR(elm->bitmap, bits);
		}
	}

	kho_out->preserved_mem_map = first_chunk;

	return 0;

err_free:
	kho_mem_ser_free(first_chunk);
	return -ENOMEM;
}

static void __init deserialize_bitmap(unsigned int order,
				      struct khoser_mem_bitmap_ptr *elm)
{
	struct kho_mem_phys_bits *bitmap = KHOSER_LOAD_PTR(elm->bitmap);
	unsigned long bit;

	for_each_set_bit(bit, bitmap->preserve, PRESERVE_BITS) {
		int sz = 1 << (order + PAGE_SHIFT);
		phys_addr_t phys =
			elm->phys_start + (bit << (order + PAGE_SHIFT));
		struct page *page = phys_to_page(phys);

		memblock_reserve(phys, sz);
		memblock_reserved_mark_noinit(phys, sz);
		page->private = order;
	}
}

static void __init kho_mem_deserialize(const void *fdt)
{
	struct khoser_mem_chunk *chunk;
	const phys_addr_t *mem;
	int len;

	mem = fdt_getprop(fdt, 0, PROP_PRESERVED_MEMORY_MAP, &len);

	if (!mem || len != sizeof(*mem)) {
		pr_err("failed to get preserved memory bitmaps\n");
		return;
	}

	chunk = *mem ? phys_to_virt(*mem) : NULL;
	while (chunk) {
		unsigned int i;

		for (i = 0; i != chunk->hdr.num_elms; i++)
			deserialize_bitmap(chunk->hdr.order,
					   &chunk->bitmaps[i]);
		chunk = KHOSER_LOAD_PTR(chunk->hdr.next);
	}
}

/*
 * With KHO enabled, memory can become fragmented because KHO regions may
 * be anywhere in physical address space. The scratch regions give us a
 * safe zones that we will never see KHO allocations from. This is where we
 * can later safely load our new kexec images into and then use the scratch
 * area for early allocations that happen before page allocator is
 * initialized.
 */
struct kho_scratch *kho_scratch;
unsigned int kho_scratch_cnt;

/*
 * The scratch areas are scaled by default as percent of memory allocated from
 * memblock. A user can override the scale with command line parameter:
 *
 * kho_scratch=N%
 *
 * It is also possible to explicitly define size for a lowmem, a global and
 * per-node scratch areas:
 *
 * kho_scratch=l[KMG],n[KMG],m[KMG]
 *
 * The explicit size definition takes precedence over scale definition.
 */
static unsigned int scratch_scale __initdata = 200;
static phys_addr_t scratch_size_global __initdata;
static phys_addr_t scratch_size_pernode __initdata;
static phys_addr_t scratch_size_lowmem __initdata;

static int __init kho_parse_scratch_size(char *p)
{
	size_t len;
	unsigned long sizes[3];
	int i;

	if (!p)
		return -EINVAL;

	len = strlen(p);
	if (!len)
		return -EINVAL;

	/* parse nn% */
	if (p[len - 1] == '%') {
		/* unsigned int max is 4,294,967,295, 10 chars */
		char s_scale[11] = {};
		int ret = 0;

		if (len > ARRAY_SIZE(s_scale))
			return -EINVAL;

		memcpy(s_scale, p, len - 1);
		ret = kstrtouint(s_scale, 10, &scratch_scale);
		if (!ret)
			pr_notice("scratch scale is %d%%\n", scratch_scale);
		return ret;
	}

	/* parse ll[KMG],mm[KMG],nn[KMG] */
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		char *endp = p;

		if (i > 0) {
			if (*p != ',')
				return -EINVAL;
			p += 1;
		}

		sizes[i] = memparse(p, &endp);
		if (!sizes[i] || endp == p)
			return -EINVAL;
		p = endp;
	}

	scratch_size_lowmem = sizes[0];
	scratch_size_global = sizes[1];
	scratch_size_pernode = sizes[2];
	scratch_scale = 0;

	pr_notice("scratch areas: lowmem: %lluMiB global: %lluMiB pernode: %lldMiB\n",
		  (u64)(scratch_size_lowmem >> 20),
		  (u64)(scratch_size_global >> 20),
		  (u64)(scratch_size_pernode >> 20));

	return 0;
}
early_param("kho_scratch", kho_parse_scratch_size);

static void __init scratch_size_update(void)
{
	phys_addr_t size;

	if (!scratch_scale)
		return;

	size = memblock_reserved_kern_size(ARCH_LOW_ADDRESS_LIMIT,
					   NUMA_NO_NODE);
	size = size * scratch_scale / 100;
	scratch_size_lowmem = round_up(size, CMA_MIN_ALIGNMENT_BYTES);

	size = memblock_reserved_kern_size(MEMBLOCK_ALLOC_ANYWHERE,
					   NUMA_NO_NODE);
	size = size * scratch_scale / 100 - scratch_size_lowmem;
	scratch_size_global = round_up(size, CMA_MIN_ALIGNMENT_BYTES);
}

static phys_addr_t __init scratch_size_node(int nid)
{
	phys_addr_t size;

	if (scratch_scale) {
		size = memblock_reserved_kern_size(MEMBLOCK_ALLOC_ANYWHERE,
						   nid);
		size = size * scratch_scale / 100;
	} else {
		size = scratch_size_pernode;
	}

	return round_up(size, CMA_MIN_ALIGNMENT_BYTES);
}

/**
 * kho_reserve_scratch - Reserve a contiguous chunk of memory for kexec
 *
 * With KHO we can preserve arbitrary pages in the system. To ensure we still
 * have a large contiguous region of memory when we search the physical address
 * space for target memory, let's make sure we always have a large CMA region
 * active. This CMA region will only be used for movable pages which are not a
 * problem for us during KHO because we can just move them somewhere else.
 */
static void __init kho_reserve_scratch(void)
{
	phys_addr_t addr, size;
	int nid, i = 0;

	if (!kho_enable)
		return;

	scratch_size_update();

	/* FIXME: deal with node hot-plug/remove */
	kho_scratch_cnt = num_online_nodes() + 2;
	size = kho_scratch_cnt * sizeof(*kho_scratch);
	kho_scratch = memblock_alloc(size, PAGE_SIZE);
	if (!kho_scratch)
		goto err_disable_kho;

	/*
	 * reserve scratch area in low memory for lowmem allocations in the
	 * next kernel
	 */
	size = scratch_size_lowmem;
	addr = memblock_phys_alloc_range(size, CMA_MIN_ALIGNMENT_BYTES, 0,
					 ARCH_LOW_ADDRESS_LIMIT);
	if (!addr)
		goto err_free_scratch_desc;

	kho_scratch[i].addr = addr;
	kho_scratch[i].size = size;
	i++;

	/* reserve large contiguous area for allocations without nid */
	size = scratch_size_global;
	addr = memblock_phys_alloc(size, CMA_MIN_ALIGNMENT_BYTES);
	if (!addr)
		goto err_free_scratch_areas;

	kho_scratch[i].addr = addr;
	kho_scratch[i].size = size;
	i++;

	for_each_online_node(nid) {
		size = scratch_size_node(nid);
		addr = memblock_alloc_range_nid(size, CMA_MIN_ALIGNMENT_BYTES,
						0, MEMBLOCK_ALLOC_ACCESSIBLE,
						nid, true);
		if (!addr)
			goto err_free_scratch_areas;

		kho_scratch[i].addr = addr;
		kho_scratch[i].size = size;
		i++;
	}

	return;

err_free_scratch_areas:
	for (i--; i >= 0; i--)
		memblock_phys_free(kho_scratch[i].addr, kho_scratch[i].size);
err_free_scratch_desc:
	memblock_free(kho_scratch, kho_scratch_cnt * sizeof(*kho_scratch));
err_disable_kho:
	pr_warn("Failed to reserve scratch area, disabling kexec handover\n");
	kho_enable = false;
}

/**
 * kho_add_subtree - record the physical address of a sub FDT in KHO root tree.
 * @name: name of the sub tree.
 * @fdt: the sub tree blob.
 *
 * Creates a new child node named @name in KHO root FDT and records
 * the physical address of @fdt. The pages of @fdt must also be preserved
 * by KHO for the new kernel to retrieve it after kexec.
 *
 * A debugfs blob entry is also created at
 * ``/sys/kernel/debug/kho/out/sub_fdts/@name`` when kernel is configured with
 * CONFIG_KEXEC_HANDOVER_DEBUG
 *
 * Return: 0 on success, error code on failure
 */
int kho_add_subtree(const char *name, void *fdt)
{
	struct kho_sub_fdt *sub_fdt;
	int err;

	sub_fdt = kmalloc(sizeof(*sub_fdt), GFP_KERNEL);
	if (!sub_fdt)
		return -ENOMEM;

	INIT_LIST_HEAD(&sub_fdt->l);
	sub_fdt->name = name;
	sub_fdt->fdt = fdt;

	mutex_lock(&kho_out.fdts_lock);
	list_add_tail(&sub_fdt->l, &kho_out.sub_fdts);
	err = kho_debugfs_fdt_add(&kho_out.dbg, name, fdt, false);
	mutex_unlock(&kho_out.fdts_lock);

	return err;
}
EXPORT_SYMBOL_GPL(kho_add_subtree);

void kho_remove_subtree(void *fdt)
{
	struct kho_sub_fdt *sub_fdt;

	mutex_lock(&kho_out.fdts_lock);
	list_for_each_entry(sub_fdt, &kho_out.sub_fdts, l) {
		if (sub_fdt->fdt == fdt) {
			list_del(&sub_fdt->l);
			kfree(sub_fdt);
			kho_debugfs_fdt_remove(&kho_out.dbg, fdt);
			break;
		}
	}
	mutex_unlock(&kho_out.fdts_lock);

}
EXPORT_SYMBOL_GPL(kho_remove_subtree);

/**
 * kho_preserve_folio - preserve a folio across kexec.
 * @folio: folio to preserve.
 *
 * Instructs KHO to preserve the whole folio across kexec. The order
 * will be preserved as well.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_folio(struct folio *folio)
{
	const unsigned long pfn = folio_pfn(folio);
	const unsigned int order = folio_order(folio);
	struct kho_mem_track *track = &kho_out.track;

	if (kho_out.finalized)
		return -EBUSY;

	return __kho_preserve_order(track, pfn, order);
}
EXPORT_SYMBOL_GPL(kho_preserve_folio);

/**
 * kho_unpreserve_folio - unpreserve a folio.
 * @folio: folio to unpreserve.
 *
 * Instructs KHO to unpreserve a folio that was preserved by
 * kho_preserve_folio() before. The provided @folio (pfn and order)
 * must exactly match a previously preserved folio.
 *
 * Return: 0 on success, error code on failure
 */
int kho_unpreserve_folio(struct folio *folio)
{
	const unsigned long pfn = folio_pfn(folio);
	const unsigned int order = folio_order(folio);
	struct kho_mem_track *track = &kho_out.track;

	if (kho_out.finalized)
		return -EBUSY;

	__kho_unpreserve_order(track, pfn, order);
	return 0;
}
EXPORT_SYMBOL_GPL(kho_unpreserve_folio);

/**
 * kho_preserve_phys - preserve a physically contiguous range across kexec.
 * @phys: physical address of the range.
 * @size: size of the range.
 *
 * Instructs KHO to preserve the memory range from @phys to @phys + @size
 * across kexec.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_phys(phys_addr_t phys, size_t size)
{
	unsigned long pfn = PHYS_PFN(phys);
	unsigned long failed_pfn = 0;
	const unsigned long start_pfn = pfn;
	const unsigned long end_pfn = PHYS_PFN(phys + size);
	int err = 0;
	struct kho_mem_track *track = &kho_out.track;

	if (kho_out.finalized)
		return -EBUSY;

	if (!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size))
		return -EINVAL;

	while (pfn < end_pfn) {
		const unsigned int order =
			min(count_trailing_zeros(pfn), ilog2(end_pfn - pfn));

		err = __kho_preserve_order(track, pfn, order);
		if (err) {
			failed_pfn = pfn;
			break;
		}

		pfn += 1 << order;
	}

	if (err)
		__kho_unpreserve(track, start_pfn, failed_pfn);

	return err;
}
EXPORT_SYMBOL_GPL(kho_preserve_phys);

/**
 * kho_unpreserve_phys - unpreserve a physically contiguous range.
 * @phys: physical address of the range.
 * @size: size of the range.
 *
 * Instructs KHO to unpreserve the memory range from @phys to @phys + @size.
 * The @phys address must be aligned to @size, and @size must be a
 * power-of-2 multiple of PAGE_SIZE.
 * This call must exactly match a granularity at which memory was originally
 * preserved (either by a `kho_preserve_phys` call with the same `phys` and
 * `size`). Unpreserving arbitrary sub-ranges of larger preserved blocks is not
 * supported.
 *
 * Return: 0 on success, error code on failure
 */
int kho_unpreserve_phys(phys_addr_t phys, size_t size)
{
	struct kho_mem_track *track = &kho_out.track;
	unsigned long pfn = PHYS_PFN(phys);
	unsigned long end_pfn = PHYS_PFN(phys + size);

	if (kho_out.finalized)
		return -EBUSY;

	if (!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size))
		return -EINVAL;

	__kho_unpreserve(track, pfn, end_pfn);

	return 0;
}
EXPORT_SYMBOL_GPL(kho_unpreserve_phys);

static int __kho_abort(void)
{
	if (kho_out.preserved_mem_map) {
		kho_mem_ser_free(kho_out.preserved_mem_map);
		kho_out.preserved_mem_map = NULL;
	}

	return 0;
}

int kho_abort(void)
{
	int ret = 0;

	if (!kho_enable)
		return -EOPNOTSUPP;

	mutex_lock(&kho_out.lock);

	if (!kho_out.finalized) {
		ret = -ENOENT;
		goto unlock;
	}

	ret = __kho_abort();
	if (ret)
		goto unlock;

	kho_out.finalized = false;

	kho_debugfs_fdt_remove(&kho_out.dbg, kho_out.fdt);

unlock:
	mutex_unlock(&kho_out.lock);
	return ret;
}

static int __kho_finalize(void)
{
	int err = 0;
	u64 *preserved_mem_map;
	void *root = kho_out.fdt;
	struct kho_sub_fdt *fdt;

	err |= fdt_create(root, PAGE_SIZE);
	err |= fdt_finish_reservemap(root);
	err |= fdt_begin_node(root, "");
	err |= fdt_property_string(root, "compatible", KHO_FDT_COMPATIBLE);
	/*
	 * Reserve the preserved-memory-map property in the root FDT, so
	 * that all property definitions will precede subnodes created by
	 * KHO callers.
	 */
	err |= fdt_property_placeholder(root, PROP_PRESERVED_MEMORY_MAP,
					sizeof(*preserved_mem_map),
					(void **)&preserved_mem_map);
	if (err)
		goto abort;

	err = kho_preserve_folio(virt_to_folio(kho_out.fdt));
	if (err)
		goto abort;

	err = kho_mem_serialize(&kho_out);
	if (err)
		goto abort;

	*preserved_mem_map = (u64)virt_to_phys(kho_out.preserved_mem_map);

	mutex_lock(&kho_out.fdts_lock);
	list_for_each_entry(fdt, &kho_out.sub_fdts, l) {
		phys_addr_t phys = virt_to_phys(fdt->fdt);

		err |= fdt_begin_node(root, fdt->name);
		err |= fdt_property(root, PROP_SUB_FDT, &phys, sizeof(phys));
		err |= fdt_end_node(root);
	};
	mutex_unlock(&kho_out.fdts_lock);

	err |= fdt_end_node(root);
	err |= fdt_finish(root);

abort:
	if (err) {
		pr_err("Failed to convert KHO state tree: %d\n", err);
		__kho_abort();
	}

	return err;
}

int kho_finalize(void)
{
	int ret = 0;

	if (!kho_enable)
		return -EOPNOTSUPP;

	mutex_lock(&kho_out.lock);

	if (kho_out.finalized) {
		ret = -EEXIST;
		goto unlock;
	}

	ret = __kho_finalize();
	if (ret)
		goto unlock;

	kho_out.finalized = true;
	ret = kho_debugfs_fdt_add(&kho_out.dbg, "fdt",
				  kho_out.fdt, true);

unlock:
	mutex_unlock(&kho_out.lock);
	return ret;
}

bool kho_finalized(void)
{
	bool ret;

	mutex_lock(&kho_out.lock);
	ret = kho_out.finalized;
	mutex_unlock(&kho_out.lock);

	return ret;
}

struct kho_in {
	phys_addr_t fdt_phys;
	phys_addr_t scratch_phys;
	struct kho_debugfs dbg;
};

static struct kho_in kho_in = {
};

static const void *kho_get_fdt(void)
{
	return kho_in.fdt_phys ? phys_to_virt(kho_in.fdt_phys) : NULL;
}

/**
 * kho_retrieve_subtree - retrieve a preserved sub FDT by its name.
 * @name: the name of the sub FDT passed to kho_add_subtree().
 * @phys: if found, the physical address of the sub FDT is stored in @phys.
 *
 * Retrieve a preserved sub FDT named @name and store its physical
 * address in @phys.
 *
 * Return: 0 on success, error code on failure
 */
int kho_retrieve_subtree(const char *name, phys_addr_t *phys)
{
	const void *fdt = kho_get_fdt();
	const u64 *val;
	int offset, len;

	if (!fdt)
		return -ENOENT;

	if (!phys)
		return -EINVAL;

	offset = fdt_subnode_offset(fdt, 0, name);
	if (offset < 0)
		return -ENOENT;

	val = fdt_getprop(fdt, offset, PROP_SUB_FDT, &len);
	if (!val || len != sizeof(*val))
		return -EINVAL;

	*phys = (phys_addr_t)*val;

	return 0;
}
EXPORT_SYMBOL_GPL(kho_retrieve_subtree);

static __init int kho_init(void)
{
	int err = 0;
	const void *fdt = kho_get_fdt();
	struct page *fdt_page;

	if (!kho_enable)
		return 0;

	fdt_page = alloc_page(GFP_KERNEL);
	if (!fdt_page) {
		err = -ENOMEM;
		goto err_free_scratch;
	}
	kho_out.fdt = page_to_virt(fdt_page);

	err = kho_debugfs_init();
	if (err)
		goto err_free_fdt;

	err = kho_out_debugfs_init(&kho_out.dbg);
	if (err)
		goto err_free_fdt;

	if (fdt) {
		kho_in_debugfs_init(&kho_in.dbg, fdt);
		return 0;
	}

	for (int i = 0; i < kho_scratch_cnt; i++) {
		unsigned long base_pfn = PHYS_PFN(kho_scratch[i].addr);
		unsigned long count = kho_scratch[i].size >> PAGE_SHIFT;
		unsigned long pfn;

		for (pfn = base_pfn; pfn < base_pfn + count;
		     pfn += pageblock_nr_pages)
			init_cma_reserved_pageblock(pfn_to_page(pfn));
	}

	return 0;

err_free_fdt:
	put_page(fdt_page);
	kho_out.fdt = NULL;
err_free_scratch:
	for (int i = 0; i < kho_scratch_cnt; i++) {
		void *start = __va(kho_scratch[i].addr);
		void *end = start + kho_scratch[i].size;

		free_reserved_area(start, end, -1, "");
	}
	kho_enable = false;
	return err;
}
fs_initcall(kho_init);

static void __init kho_release_scratch(void)
{
	phys_addr_t start, end;
	u64 i;

	memmap_init_kho_scratch_pages();

	/*
	 * Mark scratch mem as CMA before we return it. That way we
	 * ensure that no kernel allocations happen on it. That means
	 * we can reuse it as scratch memory again later.
	 */
	__for_each_mem_range(i, &memblock.memory, NULL, NUMA_NO_NODE,
			     MEMBLOCK_KHO_SCRATCH, &start, &end, NULL) {
		ulong start_pfn = pageblock_start_pfn(PFN_DOWN(start));
		ulong end_pfn = pageblock_align(PFN_UP(end));
		ulong pfn;

		for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages)
			init_pageblock_migratetype(pfn_to_page(pfn),
						   MIGRATE_CMA, false);
	}
}

void __init kho_memory_init(void)
{
	struct folio *folio;

	if (kho_in.scratch_phys) {
		kho_scratch = phys_to_virt(kho_in.scratch_phys);
		kho_release_scratch();

		kho_mem_deserialize(kho_get_fdt());
		folio = kho_restore_folio(kho_in.fdt_phys);
		if (!folio)
			pr_warn("failed to restore folio for KHO fdt\n");
	} else {
		kho_reserve_scratch();
	}
}

void __init kho_populate(phys_addr_t fdt_phys, u64 fdt_len,
			 phys_addr_t scratch_phys, u64 scratch_len)
{
	void *fdt = NULL;
	struct kho_scratch *scratch = NULL;
	int err = 0;
	unsigned int scratch_cnt = scratch_len / sizeof(*kho_scratch);

	/* Validate the input FDT */
	fdt = early_memremap(fdt_phys, fdt_len);
	if (!fdt) {
		pr_warn("setup: failed to memremap FDT (0x%llx)\n", fdt_phys);
		err = -EFAULT;
		goto out;
	}
	err = fdt_check_header(fdt);
	if (err) {
		pr_warn("setup: handover FDT (0x%llx) is invalid: %d\n",
			fdt_phys, err);
		err = -EINVAL;
		goto out;
	}
	err = fdt_node_check_compatible(fdt, 0, KHO_FDT_COMPATIBLE);
	if (err) {
		pr_warn("setup: handover FDT (0x%llx) is incompatible with '%s': %d\n",
			fdt_phys, KHO_FDT_COMPATIBLE, err);
		err = -EINVAL;
		goto out;
	}

	scratch = early_memremap(scratch_phys, scratch_len);
	if (!scratch) {
		pr_warn("setup: failed to memremap scratch (phys=0x%llx, len=%lld)\n",
			scratch_phys, scratch_len);
		err = -EFAULT;
		goto out;
	}

	/*
	 * We pass a safe contiguous blocks of memory to use for early boot
	 * purporses from the previous kernel so that we can resize the
	 * memblock array as needed.
	 */
	for (int i = 0; i < scratch_cnt; i++) {
		struct kho_scratch *area = &scratch[i];
		u64 size = area->size;

		memblock_add(area->addr, size);
		err = memblock_mark_kho_scratch(area->addr, size);
		if (WARN_ON(err)) {
			pr_warn("failed to mark the scratch region 0x%pa+0x%pa: %d",
				&area->addr, &size, err);
			goto out;
		}
		pr_debug("Marked 0x%pa+0x%pa as scratch", &area->addr, &size);
	}

	memblock_reserve(scratch_phys, scratch_len);

	/*
	 * Now that we have a viable region of scratch memory, let's tell
	 * the memblocks allocator to only use that for any allocations.
	 * That way we ensure that nothing scribbles over in use data while
	 * we initialize the page tables which we will need to ingest all
	 * memory reservations from the previous kernel.
	 */
	memblock_set_kho_scratch_only();

	kho_in.fdt_phys = fdt_phys;
	kho_in.scratch_phys = scratch_phys;
	kho_scratch_cnt = scratch_cnt;
	pr_info("found kexec handover data. Will skip init for some devices\n");

out:
	if (fdt)
		early_memunmap(fdt, fdt_len);
	if (scratch)
		early_memunmap(scratch, scratch_len);
	if (err)
		pr_warn("disabling KHO revival: %d\n", err);
}

/* Helper functions for kexec_file_load */

int kho_fill_kimage(struct kimage *image)
{
	ssize_t scratch_size;
	int err = 0;
	struct kexec_buf scratch;

	if (!kho_enable)
		return 0;

	image->kho.fdt = virt_to_phys(kho_out.fdt);

	scratch_size = sizeof(*kho_scratch) * kho_scratch_cnt;
	scratch = (struct kexec_buf){
		.image = image,
		.buffer = kho_scratch,
		.bufsz = scratch_size,
		.mem = KEXEC_BUF_MEM_UNKNOWN,
		.memsz = scratch_size,
		.buf_align = SZ_64K, /* Makes it easier to map */
		.buf_max = ULONG_MAX,
		.top_down = true,
	};
	err = kexec_add_buffer(&scratch);
	if (err)
		return err;
	image->kho.scratch = &image->segment[image->nr_segments - 1];

	return 0;
}

static int kho_walk_scratch(struct kexec_buf *kbuf,
			    int (*func)(struct resource *, void *))
{
	int ret = 0;
	int i;

	for (i = 0; i < kho_scratch_cnt; i++) {
		struct resource res = {
			.start = kho_scratch[i].addr,
			.end = kho_scratch[i].addr + kho_scratch[i].size - 1,
		};

		/* Try to fit the kimage into our KHO scratch region */
		ret = func(&res, kbuf);
		if (ret)
			break;
	}

	return ret;
}

int kho_locate_mem_hole(struct kexec_buf *kbuf,
			int (*func)(struct resource *, void *))
{
	int ret;

	if (!kho_enable || kbuf->image->type == KEXEC_TYPE_CRASH)
		return 1;

	ret = kho_walk_scratch(kbuf, func);

	return ret == 1 ? 0 : -EADDRNOTAVAIL;
}
