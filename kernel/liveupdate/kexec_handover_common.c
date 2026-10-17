// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_handover_common.c - Shared Kexec Handover helpers
 *
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#define pr_fmt(fmt) "KHO: " fmt

#include <linux/bitops.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/libfdt.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "kexec_handover_internal.h"

static unsigned int kho_flags_to_vmalloc(unsigned short kho_flags)
{
	unsigned int vm_flags = 0;

	if (kho_flags & KHO_VMALLOC_ALLOC)
		vm_flags |= VM_ALLOC;
	if (kho_flags & KHO_VMALLOC_HUGE_VMAP)
		vm_flags |= VM_ALLOW_HUGE_VMAP;

	return vm_flags;
}

bool kho_vmalloc_validate(const struct kho_vmalloc *preservation,
			  unsigned int *vm_flags)
{
	if (!preservation || !preservation->first.phys ||
	    !PAGE_ALIGNED(preservation->first.phys) ||
	    !preservation->total_pages)
		return false;

	if (preservation->order >= BITS_PER_LONG - PAGE_SHIFT ||
	    (1UL << preservation->order) > preservation->total_pages ||
	    !IS_ALIGNED(preservation->total_pages, 1UL << preservation->order))
		return false;

	if (preservation->flags & ~(KHO_VMALLOC_ALLOC | KHO_VMALLOC_HUGE_VMAP))
		return false;

	*vm_flags = kho_flags_to_vmalloc(preservation->flags);
	return true;
}

/**
 * kho_vmalloc_walk - Walk the chunk list of a preserved vmalloc region.
 * @preservation: Validated KHO vmalloc preservation metadata.
 * @ops:          Callbacks to acquire/release chunks and process each entry.
 * @priv:         Caller context passed to @ops->entry.
 *
 * Traverses the linked list of struct kho_vmalloc_chunk pages starting from
 * @preservation->first.phys, invoking @ops->entry for each contiguous physical
 * range and verifying that non-terminal chunks are full and that the total
 * number of pages matches @preservation->total_pages.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
int kho_vmalloc_walk(const struct kho_vmalloc *preservation,
		     const struct kho_vmalloc_walk_ops *ops, void *priv)
{
	unsigned long contig_pages = 1UL << preservation->order;
	unsigned long total_pages = preservation->total_pages;
	unsigned long nr_pages = 0;
	phys_addr_t chunk_pa;

	for (chunk_pa = preservation->first.phys; chunk_pa; ) {
		const struct kho_vmalloc_chunk *chunk;
		phys_addr_t next_pa;
		unsigned int i;

		chunk = ops->get_chunk(chunk_pa);
		if (!chunk)
			return -EINVAL;

		for (i = 0; i < ARRAY_SIZE(chunk->phys) && chunk->phys[i]; i++) {
			if (nr_pages + contig_pages > total_pages ||
			    ops->entry(chunk->phys[i], priv))
				goto err_put_chunk;

			nr_pages += contig_pages;
		}

		next_pa = chunk->hdr.next.phys;
		if (next_pa && i < ARRAY_SIZE(chunk->phys))
			goto err_put_chunk;

		ops->put_chunk(chunk);
		chunk_pa = next_pa;
		continue;

err_put_chunk:
		ops->put_chunk(chunk);
		return -EINVAL;
	}

	return nr_pages == total_pages ? 0 : -EINVAL;
}

/**
 * kho_encode_radix_key - Encodes a physical address and order into a radix key.
 * @phys: The physical address of the page.
 * @order: The order of the page.
 *
 * This function combines a page's physical address and its order into a
 * single unsigned long, which is used as a key for all radix tree
 * operations.
 *
 * Return: The encoded unsigned long radix key.
 */
unsigned long kho_encode_radix_key(phys_addr_t phys, unsigned int order)
{
	/* The physical address is encoded by shifting the PFN by its order. */
	unsigned long shift = PAGE_SHIFT + order;
	/* Order bit goes right before the shifted PFN. */
	unsigned long h = 1UL << (64 - shift);
	/* Shifted PFN. */
	unsigned long l = phys >> shift;

	return h | l;
}

/**
 * kho_decode_radix_key - Decodes a radix key back into a physical address and order.
 * @key: The unsigned long key to decode.
 * @order: An output parameter, a pointer to an unsigned int where the decoded
 *         page order will be stored.
 *
 * This function reverses the encoding performed by kho_encode_radix_key(),
 * extracting the original physical address and page order from a given key.
 *
 * Return: The decoded physical address.
 */
phys_addr_t kho_decode_radix_key(unsigned long key, unsigned int *order)
{
	/* fls64() indexes starting from 1. */
	unsigned int order_bit = fls64(key) - 1;
	phys_addr_t phys;

	/* order bit goes right before the shifted PFN. */
	*order = 64 - (PAGE_SHIFT + order_bit);
	/* The order bit is discarded by the shift */
	phys = key << (PAGE_SHIFT + *order);

	return phys;
}

unsigned long kho_radix_get_bitmap_index(unsigned long key)
{
	return key % (1 << KHO_BITMAP_SIZE_LOG2);
}

unsigned long kho_radix_get_table_index(unsigned long key, unsigned int level)
{
	int s;

	s = ((level - 1) * KHO_TABLE_SIZE_LOG2) + KHO_BITMAP_SIZE_LOG2;
	return (key >> s) % (1 << KHO_TABLE_SIZE_LOG2);
}

/**
 * kho_radix_find_leaf - Find the radix tree leaf for a given key.
 * @root_pa: Physical address of the radix tree root node.
 * @key:     The encoded radix key to look up.
 * @map:     Callback to map a physical page address to a virtual pointer.
 *
 * Traverses the intermediate radix tree levels from @root_pa using @map to
 * translate physical node addresses and returns the mapped leaf node for
 * @key.
 *
 * Return: Virtual pointer to the leaf node on success, or NULL if @key is
 * not present or a mapping fails.
 */
struct kho_radix_leaf *kho_radix_find_leaf(phys_addr_t root_pa,
					   unsigned long key,
					   void *(*map)(phys_addr_t pa))
{
	phys_addr_t pa = root_pa;
	unsigned int i, idx;

	if (!pa || unlikely(fls64(key) > KHO_RADIX_KEY_WIDTH))
		return NULL;

	for (i = KHO_TREE_MAX_DEPTH - 1; i > 0; i--) {
		const struct kho_radix_node *node = map(pa);

		if (!node)
			return NULL;

		idx = kho_radix_get_table_index(key, i);
		pa = (phys_addr_t)READ_ONCE(node->table[idx]);
		if (!pa)
			return NULL;
	}

	return map(pa);
}

int __init kho_check_fdt(const void *fdt, phys_addr_t fdt_phys)
{
	int err;

	err = fdt_check_header(fdt);
	if (err) {
		pr_warn("handover FDT (0x%llx) is invalid: %d\n",
			(unsigned long long)fdt_phys, err);
		return err;
	}

	if (fdt_totalsize(fdt) > PAGE_SIZE) {
		pr_warn("handover FDT (0x%llx) totalsize %u exceeds PAGE_SIZE\n",
			(unsigned long long)fdt_phys, fdt_totalsize(fdt));
		return -EINVAL;
	}

	err = fdt_node_check_compatible(fdt, 0, KHO_FDT_COMPATIBLE);
	if (err) {
		pr_warn("handover FDT (0x%llx) is incompatible with '%s': %d\n",
			(unsigned long long)fdt_phys, KHO_FDT_COMPATIBLE, err);
		return err;
	}

	return 0;
}

/* Returns physical address of the preserved memory map from FDT */
phys_addr_t __init kho_get_mem_map_phys(const void *fdt)
{
	const void *mem_ptr;
	int len;

	mem_ptr = fdt_getprop(fdt, 0, KHO_FDT_MEMORY_MAP_PROP_NAME, &len);
	if (!mem_ptr || len != sizeof(u64)) {
		pr_err("failed to get preserved memory map\n");
		return 0;
	}

	return get_unaligned((const u64 *)mem_ptr);
}

/**
 * kho_retrieve_subtree - retrieve a preserved sub blob by its name.
 * @name: the name of the sub blob passed to kho_add_subtree().
 * @phys: if found, the physical address of the sub blob is stored in @phys.
 * @size: if not NULL and found, the size of the sub blob is stored in @size.
 *
 * Retrieve a preserved sub blob named @name and store its physical
 * address in @phys and optionally its size in @size.
 *
 * Return: 0 on success, error code on failure
 */
int kho_retrieve_subtree(const char *name, phys_addr_t *phys, size_t *size)
{
	const void *fdt = kho_is_ro() ? kho_ro_get_fdt() : kho_get_fdt();
	const u64 *val;
	int offset, len;

	if (!fdt)
		return -ENOENT;

	if (!phys)
		return -EINVAL;

	offset = fdt_subnode_offset(fdt, 0, name);
	if (offset < 0)
		return -ENOENT;

	val = fdt_getprop(fdt, offset, KHO_SUB_TREE_PROP_NAME, &len);
	if (!val || len != sizeof(*val))
		return -EINVAL;

	*phys = (phys_addr_t)get_unaligned(val);

	val = fdt_getprop(fdt, offset, KHO_SUB_TREE_SIZE_PROP_NAME, &len);
	if (!val || len != sizeof(*val)) {
		pr_warn("broken KHO subnode '%s': missing or invalid blob-size property\n",
			name);
		return -EINVAL;
	}

	if (size)
		*size = (size_t)get_unaligned(val);

	return 0;
}
EXPORT_SYMBOL_GPL(kho_retrieve_subtree);
