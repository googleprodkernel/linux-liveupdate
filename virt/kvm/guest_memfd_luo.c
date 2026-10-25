// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * Tarun Sahu <tarunsahu@google.com>
 *
 * Guestmemfd Preservation for Live Update Orchestrator (LUO)
 */

/**
 * DOC: Guestmemfd Preservation via LUO
 *
 * Overview
 * ========
 *
 * Guest memory file descriptors (guest_memfd) can be preserved over a kexec
 * reboot using the Live Update Orchestrator (LUO) file preservation. This
 * allows userspace to preserve VM memory across kexec reboots.
 *
 * The preservation is not intended to be transparent. Only select properties
 * of the guest_memfd are preserved, while others are reset to default.
 *
 * Preserved Properties
 * ====================
 *
 * The following properties of guest_memfd are preserved across kexec:
 *
 * File Size
 *   The size of the file is preserved.
 *
 * File Contents
 *   All folios present in the page cache are preserved.
 *
 * File-level Flags
 *   The file-level flags (such as MMAP support and INIT_SHARED default mapping)
 *   are preserved.
 *
 * Non-Preserved Properties
 * ========================
 *
 * NUMA Memory Policy
 *   NUMA memory policies associated with the guest_memfd are not preserved.
 */
#include <linux/liveupdate.h>
#include <linux/kvm_host.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/err.h>
#include <linux/anon_inodes.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/kho/abi/kvm.h>
#include "guest_memfd.h"
#include "kvm_mm.h"


static int kvm_gmem_luo_walk_folios(struct address_space *mapping,
		pgoff_t end_index, struct guest_memfd_luo_folio_ser *folios_ser,
		u64 *out_count)
{
	struct folio_batch fbatch;
	pgoff_t index = 0;
	u64 count = 0;
	int err = 0;

	folio_batch_init(&fbatch);
	while (index < end_index) {
		unsigned int nr, i;

		nr = filemap_get_folios(mapping, &index, end_index - 1, &fbatch);
		if (nr == 0)
			break;

		for (i = 0; i < nr; i++) {
			struct folio *folio = fbatch.folios[i];

			if (folios_ser) {
				if (folio_test_hwpoison(folio)) {
					err = -EHWPOISON;
					folio_batch_release(&fbatch);
					goto out;
				}
				err = kho_preserve_folio(folio);
				if (err) {
					folio_batch_release(&fbatch);
					goto out;
				}

				folios_ser[count].pfn = folio_pfn(folio);
				folios_ser[count].index = folio->index;
				folios_ser[count].flags = folio_test_uptodate(folio) ?
							  GUEST_MEMFD_LUO_FOLIO_UPTODATE : 0;
			}
			count++;
		}
		folio_batch_release(&fbatch);
		cond_resched();
	}

out:
	*out_count = count;
	return err;
}

static bool kvm_gmem_luo_can_preserve(struct liveupdate_file_handler *handler, struct file *file)
{
	struct inode *inode = file_inode(file);
	struct gmem_file *gmem_file;
	struct kvm *kvm;

	if (inode->i_sb->s_magic != GUEST_MEMFD_MAGIC)
		return false;

	gmem_file = file->private_data;
	if (!gmem_file)
		return false;

	/*
	 * Only Fully-shared guest_memfd preservation is supported
	 */
	if (!(GMEM_I(inode)->flags & GUEST_MEMFD_FLAG_INIT_SHARED))
		return false;

	/*
	 * It makes sure that no memory can be converted to private
	 * even if it was initially fully shared (in-place conversions are
	 * prevented).
	 */
	kvm = gmem_file->kvm;
	if (kvm_arch_has_private_mem(kvm))
		return false;

	return true;
}

static int kvm_gmem_luo_preserve(struct liveupdate_file_op_args *args)
{
	struct guest_memfd_luo_folio_ser *folios_ser = NULL;
	u64 count = 0, gmem_flags, abi_flags = 0;
	struct guest_memfd_luo_ser *ser;
	struct address_space *mapping;
	struct gmem_file *gmem_file;
	struct file *kvm_file;
	struct inode *inode;
	pgoff_t end_index;
	struct kvm *kvm;
	u64 vm_token;
	int err = 0;
	long size, i;

	gmem_file = args->file->private_data;
	kvm = gmem_file->kvm;

	/*
	 * Obtain a strong reference to kvm->vm_file to prevent the SLAB_TYPESAFE_BY_RCU
	 * file memory from being reallocated while it is being processed.
	 */
	kvm_file = get_file_active(&kvm->vm_file);
	if (!kvm_file)
		return -ENOENT;

	err = liveupdate_get_token_outgoing(args->session, kvm_file, &vm_token);
	fput(kvm_file);
	if (err)
		return err;

	inode = file_inode(args->file);
	kvm_gmem_freeze(inode, true);

	mapping = inode->i_mapping;
	size = i_size_read(inode);
	if (!size) {
		err = -EINVAL;
		goto err_unfreeze_inode;
	}

	if (WARN_ON_ONCE(!PAGE_ALIGNED(size))) {
		err = -EINVAL;
		goto err_unfreeze_inode;
	}

	gmem_flags = READ_ONCE(GMEM_I(inode)->flags);
	if (gmem_flags & ~(GUEST_MEMFD_FLAG_MMAP | GUEST_MEMFD_FLAG_INIT_SHARED
				| GUEST_MEMFD_F_MAPPING_FROZEN)) {
		err = -EOPNOTSUPP;
		goto err_unfreeze_inode;
	}

	if (gmem_flags & GUEST_MEMFD_FLAG_MMAP)
		abi_flags |= GUEST_MEMFD_LUO_FLAG_MMAP;
	if (gmem_flags & GUEST_MEMFD_FLAG_INIT_SHARED)
		abi_flags |= GUEST_MEMFD_LUO_FLAG_INIT_SHARED;

	end_index = size >> PAGE_SHIFT;

	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser)) {
		err = PTR_ERR(ser);
		goto err_unfreeze_inode;
	}

	/* First pass: Count the folios present in the page cache */
	err = kvm_gmem_luo_walk_folios(mapping, end_index, NULL, &count);
	if (err)
		goto err_free_ser;

	ser->size = size;
	ser->flags = abi_flags;
	ser->nr_folios = count;
	ser->vm_token = vm_token;

	if (count > 0) {
		folios_ser = vcalloc(count, sizeof(*folios_ser));
		if (!folios_ser) {
			err = -ENOMEM;
			goto err_free_ser;
		}

		/* Second pass: Fill the metadata array and preserve folios */
		err = kvm_gmem_luo_walk_folios(mapping, end_index, folios_ser, &count);
		if (err)
			goto err_unpreserve_unlocked;

		if (WARN_ON_ONCE(count != ser->nr_folios)) {
			err = -EINVAL;
			goto err_unpreserve_unlocked;
		}
	}

	if (count > 0) {
		err = kho_preserve_vmalloc(folios_ser, &ser->folios);
		if (err)
			goto err_unpreserve_unlocked;
	}

	args->serialized_data = virt_to_phys(ser);
	args->private_data = folios_ser;

	return 0;

err_unpreserve_unlocked:
	for (i = (long)count - 1; i >= 0; i--) {
		struct folio *folio;

		if (!folios_ser[i].pfn)
			continue;

		folio = pfn_folio(folios_ser[i].pfn);
		kho_unpreserve_folio(folio);
	}
	vfree(folios_ser);
err_free_ser:
	kho_unpreserve_free(ser);
err_unfreeze_inode:
	kvm_gmem_freeze(inode, false);
	return err;
}

static void kvm_gmem_luo_discard_folios(
	const struct guest_memfd_luo_folio_ser *folios_ser,
	u64 nr_folios, u64 start_idx)
{
	long i;

	for (i = start_idx; i < nr_folios; i++) {
		struct folio *folio;
		phys_addr_t phys;

		if (!folios_ser[i].pfn)
			continue;

		phys = PFN_PHYS(folios_ser[i].pfn);
		folio = kho_restore_folio(phys);
		if (folio)
			folio_put(folio);
	}
}

static void kvm_gmem_luo_unpreserve(struct liveupdate_file_op_args *args)
{
	struct guest_memfd_luo_folio_ser *folios_ser = args->private_data;
	struct guest_memfd_luo_ser *ser;
	long i;

	if (WARN_ON_ONCE(!args->serialized_data))
		return;

	ser = phys_to_virt(args->serialized_data);

	if (ser->nr_folios > 0)
		kho_unpreserve_vmalloc(&ser->folios);
	for (i = ser->nr_folios - 1; i >= 0; i--) {
		struct folio *folio;

		if (!folios_ser[i].pfn)
			continue;

		folio = pfn_folio(folios_ser[i].pfn);
		kho_unpreserve_folio(folio);
	}
	vfree(folios_ser);

	kho_unpreserve_free(ser);
	kvm_gmem_freeze(file_inode(args->file), false);
}

#ifdef CONFIG_KEXEC_HANDOVER_READONLY
static int kvm_gmem_luo_retrieve_ro_cb(struct liveupdate_file_op_args *args);
#else
static int kvm_gmem_luo_retrieve_ro_cb(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}
#endif

static int kvm_gmem_luo_retrieve(struct liveupdate_file_op_args *args)
{
	struct guest_memfd_luo_folio_ser *folios_ser = NULL;
	struct guest_memfd_luo_ser *ser;
	struct kvm *kvm = NULL;
	struct file *vm_file;
	struct inode *inode;
	struct file *file;
	u64 gmem_flags = 0;
	int err = 0;
	u64 i = 0;

	if (kho_is_ro())
		return kvm_gmem_luo_retrieve_ro_cb(args);

	if (!args->serialized_data)
		return -EINVAL;

	ser = phys_to_virt(args->serialized_data);

	if (ser->size <= 0 || !PAGE_ALIGNED(ser->size)) {
		err = -EINVAL;
		goto err_free_ser;
	}

	if (ser->flags & ~GUEST_MEMFD_LUO_SUPPORTED_FLAGS) {
		err = -EOPNOTSUPP;
		goto err_free_ser;
	}

	if (ser->flags & GUEST_MEMFD_LUO_FLAG_MMAP)
		gmem_flags |= GUEST_MEMFD_FLAG_MMAP;
	if (ser->flags & GUEST_MEMFD_LUO_FLAG_INIT_SHARED)
		gmem_flags |= GUEST_MEMFD_FLAG_INIT_SHARED;

	err = liveupdate_get_file_incoming(args->session, ser->vm_token, &vm_file);
	if (err) {
		pr_warn("gmem: provided VM FD token (%llx) on preserve is incorrect\n",
						ser->vm_token);
		goto err_free_ser;
	}

	if (file_is_kvm(vm_file))
		kvm = vm_file->private_data;

	/*
	 * Release the temporary reference taken by the liveupdate_get_file_incoming
	 * call. LUO still holds a reference.
	 */
	fput(vm_file);

	if (!kvm) {
		err = -EINVAL;
		goto err_free_ser;
	}

	if (gmem_flags & ~kvm_gmem_get_supported_flags(kvm)) {
		err = -EINVAL;
		goto err_free_ser;
	}

	file = __kvm_gmem_create_file(kvm, ser->size, gmem_flags);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_free_ser;
	}

	inode = file_inode(file);

	if (ser->nr_folios) {
		folios_ser = kho_restore_vmalloc(&ser->folios);
		if (!folios_ser) {
			err = -EINVAL;
			goto err_destroy_file;
		}

		for (i = 0; i < ser->nr_folios; i++) {
			struct folio *folio;
			phys_addr_t phys;

			if (!folios_ser[i].pfn)
				continue;

			phys = PFN_PHYS(folios_ser[i].pfn);
			folio = kho_restore_folio(phys);
			if (!folio) {
				pr_err("gmem: failed to restore folio at %llx\n", phys);
				err = -EIO;
				goto err_put_remaining_folios;
			}

			err = filemap_add_folio(inode->i_mapping, folio, folios_ser[i].index,
						GFP_KERNEL);
			if (err) {
				pr_err("gmem: failed to add folio to page cache\n");
				folio_put(folio);
				goto err_put_remaining_folios;
			}

			if (folios_ser[i].flags & GUEST_MEMFD_LUO_FOLIO_UPTODATE)
				folio_mark_uptodate(folio);
			folio_unlock(folio);
			folio_put(folio);
		}
		vfree(folios_ser);
	}

	args->file = file;
	kho_restore_free(ser);
	return 0;

err_put_remaining_folios:
	i++;
err_destroy_file:
	fput(file);
err_free_ser:
	if (ser->nr_folios) {
		if (!folios_ser)
			folios_ser = kho_restore_vmalloc(&ser->folios);
		if (folios_ser) {
			kvm_gmem_luo_discard_folios(folios_ser, ser->nr_folios, i);
			vfree(folios_ser);
		}
	}
	kho_restore_free(ser);
	return err;
}

static void kvm_gmem_luo_finish(struct liveupdate_file_op_args *args)
{
	struct guest_memfd_luo_ser *ser;
	struct guest_memfd_luo_folio_ser *folios_ser;

	/* Nothing to be done here, if retrieve_status was successful or errored,
	 * Cleanup is taken care of in retrieval call.
	 */
	if (args->retrieve_status)
		return;

	if (!args->serialized_data)
		return;

	ser = phys_to_virt(args->serialized_data);

	if (ser->nr_folios) {
		folios_ser = kho_restore_vmalloc(&ser->folios);
		if (folios_ser) {
			kvm_gmem_luo_discard_folios(folios_ser, ser->nr_folios, 0);
			vfree(folios_ser);
		}
	}

	kho_restore_free(ser);
}

#ifdef CONFIG_KEXEC_HANDOVER_READONLY
struct kvm_gmem_luo_ro_extent {
	pgoff_t index;
	unsigned long pfn;
	unsigned long nr_pages;
};

struct kvm_gmem_luo_ro_ctx {
	struct kvm_gmem_luo_ro_extent *extents;
	u64 nr_extents;
	refcount_t refcount;
	bool finished;
	bool pat_tracked;
};

static const struct kvm_gmem_luo_ro_extent *
kvm_gmem_luo_ro_find_extent(const struct kvm_gmem_luo_ro_ctx *ctx, pgoff_t pgoff)
{
	u64 lo = 0, hi;

	if (!ctx || !ctx->nr_extents)
		return NULL;

	hi = ctx->nr_extents - 1;
	while (lo <= hi) {
		u64 mid = lo + ((hi - lo) >> 1);
		const struct kvm_gmem_luo_ro_extent *ext = &ctx->extents[mid];

		if (pgoff < ext->index) {
			if (mid == 0)
				break;
			hi = mid - 1;
		} else if (pgoff >= ext->index + ext->nr_pages) {
			lo = mid + 1;
		} else {
			return ext;
		}
	}
	return NULL;
}

static vm_fault_t kvm_gmem_luo_ro_fault(struct vm_fault *vmf)
{
	struct file *file = vmf->vma->vm_file;
	struct kvm_gmem_luo_ro_ctx *ctx = file->private_data;
	const struct kvm_gmem_luo_ro_extent *ext;
	unsigned long pfn;

	if (!ctx || READ_ONCE(ctx->finished))
		return VM_FAULT_SIGBUS;

	ext = kvm_gmem_luo_ro_find_extent(ctx, vmf->pgoff);
	if (!ext)
		return VM_FAULT_SIGBUS;

	pfn = ext->pfn + (vmf->pgoff - ext->index);
	return vmf_insert_pfn(vmf->vma, vmf->address, pfn);
}

static vm_fault_t kvm_gmem_luo_ro_huge_fault(struct vm_fault *vmf,
					     unsigned int order)
{
	struct file *file = vmf->vma->vm_file;
	struct kvm_gmem_luo_ro_ctx *ctx = file->private_data;
	const struct kvm_gmem_luo_ro_extent *ext;
	unsigned long nr = 1UL << order;
	unsigned long hmask = ~(nr - 1);
	pgoff_t hpgoff = vmf->pgoff & hmask;
	unsigned long haddr = vmf->address & ~((PAGE_SIZE << order) - 1);
	bool write = !!(vmf->vma->vm_flags & VM_WRITE);
	unsigned long pfn;

	if (!ctx || READ_ONCE(ctx->finished))
		return VM_FAULT_SIGBUS;

	if (order != PMD_ORDER && order != PUD_ORDER)
		return VM_FAULT_FALLBACK;

	if (haddr < vmf->vma->vm_start ||
	    haddr + (PAGE_SIZE << order) > vmf->vma->vm_end)
		return VM_FAULT_FALLBACK;

	ext = kvm_gmem_luo_ro_find_extent(ctx, hpgoff);
	if (!ext)
		return VM_FAULT_FALLBACK;

	if (hpgoff < ext->index ||
	    hpgoff + nr > ext->index + ext->nr_pages)
		return VM_FAULT_FALLBACK;

	pfn = ext->pfn + (hpgoff - ext->index);
	if (!IS_ALIGNED(pfn, nr))
		return VM_FAULT_FALLBACK;

	if (order == PUD_ORDER)
		return vmf_insert_pfn_pud(vmf, pfn, write);
	return vmf_insert_pfn_pmd(vmf, pfn, write);
}

static const struct vm_operations_struct kvm_gmem_luo_ro_vm_ops = {
	.fault      = kvm_gmem_luo_ro_fault,
	.huge_fault = kvm_gmem_luo_ro_huge_fault,
};

static int kvm_gmem_luo_ro_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct kvm_gmem_luo_ro_ctx *ctx = file->private_data;

	if (!ctx || READ_ONCE(ctx->finished))
		return -EINVAL;

	if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) !=
	    (VM_SHARED | VM_MAYSHARE))
		return -EINVAL;

	vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP | VM_HUGEPAGE);
	vma->vm_ops = &kvm_gmem_luo_ro_vm_ops;
	return 0;
}

static unsigned long kvm_gmem_luo_ro_get_unmapped_area(struct file *file,
						       unsigned long addr,
						       unsigned long len,
						       unsigned long pgoff,
						       unsigned long flags)
{
	struct mm_struct *mm = current->mm;
	struct vm_unmapped_area_info info = {};
	unsigned long align_size = 0;

	if (flags & MAP_FIXED)
		return addr;

	if (len >= PUD_SIZE)
		align_size = PUD_SIZE;
	else if (len >= PMD_SIZE)
		align_size = PMD_SIZE;

	if (!align_size || addr)
		return mm_get_unmapped_area(file, addr, len, pgoff, flags);

	info.flags = VM_UNMAPPED_AREA_TOPDOWN;
	info.length = len;
	info.low_limit = FIRST_USER_ADDRESS;
	info.high_limit = mm->mmap_base;
	info.align_mask = align_size - 1;
	info.align_offset = (pgoff << PAGE_SHIFT) & (align_size - 1);
	addr = vm_unmapped_area(&info);
	if (!offset_in_page(addr))
		return addr;

	return mm_get_unmapped_area(file, 0, len, pgoff, flags);
}

static long kvm_gmem_luo_ro_fallocate(struct file *file, int mode,
				      loff_t offset, loff_t len)
{
	return -EPERM;
}

static int kvm_gmem_luo_ro_open(struct inode *inode, struct file *file)
{
	struct kvm_gmem_luo_ro_ctx *ctx = inode->i_private;

	if (!ctx || READ_ONCE(ctx->finished))
		return -EINVAL;
	if (!refcount_inc_not_zero(&ctx->refcount))
		return -EINVAL;
	file->private_data = ctx;
	return 0;
}

static int kvm_gmem_luo_ro_release(struct inode *inode, struct file *file)
{
	struct kvm_gmem_luo_ro_ctx *ctx = file->private_data;
	u64 i;

	if (ctx && refcount_dec_and_test(&ctx->refcount)) {
		WRITE_ONCE(ctx->finished, true);
		unmap_mapping_range(inode->i_mapping, 0, 0, 1);
		inode->i_private = NULL;
		if (ctx->pat_tracked) {
			ctx->pat_tracked = false;
			for (i = 0; i < ctx->nr_extents; i++) {
				pfnmap_untrack(ctx->extents[i].pfn,
					       ctx->extents[i].nr_pages << PAGE_SHIFT);
			}
		}
		kvfree(ctx->extents);
		kfree(ctx);
	}
	file->private_data = NULL;
	return 0;
}

static const struct file_operations kvm_gmem_luo_ro_fops = {
	.owner             = THIS_MODULE,
	.open              = kvm_gmem_luo_ro_open,
	.release           = kvm_gmem_luo_ro_release,
	.mmap              = kvm_gmem_luo_ro_mmap,
	.get_unmapped_area = kvm_gmem_luo_ro_get_unmapped_area,
	.fallocate         = kvm_gmem_luo_ro_fallocate,
};

static unsigned long
kvm_gmem_luo_ro_folio_nr_pages(const struct guest_memfd_luo_folio_ser *folios_ser,
			       u64 i, u64 nr_folios, u64 total_pages)
{
	pgoff_t index = folios_ser[i].index;
	unsigned long pfn = folios_ser[i].pfn;
	int order;

	if (i + 1 < nr_folios) {
		unsigned long span = folios_ser[i + 1].index - index;

		if (is_power_of_2(span) && IS_ALIGNED(pfn, span))
			return span;
	} else if (total_pages > index) {
		unsigned long span = total_pages - index;

		if (is_power_of_2(span) && IS_ALIGNED(pfn, span))
			return span;
	}

	order = kho_ro_lookup_folio_order(PFN_PHYS(pfn));
	if (order >= 0)
		return 1UL << order;

	return 1;
}

static int kvm_gmem_luo_retrieve_ro_cb(struct liveupdate_file_op_args *args)
{
	const struct guest_memfd_luo_folio_ser *folios_ser = NULL;
	struct kvm_gmem_luo_ro_extent *extents = NULL;
	const struct guest_memfd_luo_ser *ser;
	struct kvm_gmem_luo_ro_ctx *ctx;
	u64 nr_folios, size, total_pages;
	u64 nr_extents = 0, i, j;
	struct file *file;
	int err = 0;

	if (!args->serialized_data)
		return -EINVAL;

	ser = kho_ro_map_folio(args->serialized_data, PAGE_KERNEL_RO);
	if (!ser)
		return -EINVAL;

	size = ser->size;
	nr_folios = ser->nr_folios;

	if (size <= 0 || !PAGE_ALIGNED(size))
		return -EINVAL;

	if (ser->flags & ~GUEST_MEMFD_LUO_SUPPORTED_FLAGS)
		return -EOPNOTSUPP;

	if (nr_folios > 0) {
		folios_ser = kho_ro_map_vmalloc(&ser->folios);
		if (!folios_ser)
			return -EINVAL;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	total_pages = DIV_ROUND_UP(size, PAGE_SIZE);

	if (nr_folios > 0) {
		pgprot_t wb_prot = PAGE_KERNEL;
		pgoff_t prev_end_idx = 0;
		unsigned long prev_end_pfn = 0;

		/* Pass 1: count coalesced contiguous extents */
		for (i = 0; i < nr_folios; i++) {
			unsigned long pfn = folios_ser[i].pfn;
			pgoff_t index = folios_ser[i].index;
			unsigned long nr_pages;

			if (!pfn)
				continue;

			nr_pages = kvm_gmem_luo_ro_folio_nr_pages(folios_ser, i,
								  nr_folios,
								  total_pages);
			if (nr_extents > 0 &&
			    prev_end_idx == index &&
			    prev_end_pfn == pfn) {
				prev_end_idx += nr_pages;
				prev_end_pfn += nr_pages;
			} else {
				nr_extents++;
				prev_end_idx = index + nr_pages;
				prev_end_pfn = pfn + nr_pages;
			}
		}

		if (nr_extents > 0) {
			u64 ext_idx = 0;

			extents = kvmalloc_array(nr_extents, sizeof(*extents),
						 GFP_KERNEL);
			if (!extents) {
				kfree(ctx);
				return -ENOMEM;
			}

			/* Pass 2: populate coalesced extents */
			for (i = 0; i < nr_folios; i++) {
				unsigned long pfn = folios_ser[i].pfn;
				pgoff_t index = folios_ser[i].index;
				unsigned long nr_pages;

				if (!pfn)
					continue;

				nr_pages = kvm_gmem_luo_ro_folio_nr_pages(folios_ser, i,
									  nr_folios,
									  total_pages);
				if (ext_idx > 0 &&
				    extents[ext_idx - 1].index + extents[ext_idx - 1].nr_pages == index &&
				    extents[ext_idx - 1].pfn + extents[ext_idx - 1].nr_pages == pfn) {
					extents[ext_idx - 1].nr_pages += nr_pages;
				} else {
					extents[ext_idx].index = index;
					extents[ext_idx].pfn = pfn;
					extents[ext_idx].nr_pages = nr_pages;
					ext_idx++;
				}
			}

			for (i = 0; i < nr_extents; i++) {
				pgprot_t ext_prot = wb_prot;

				err = pfnmap_track(extents[i].pfn,
						   extents[i].nr_pages << PAGE_SHIFT,
						   &ext_prot);
				if (err) {
					for (j = 0; j < i; j++) {
						pfnmap_untrack(extents[j].pfn,
							       extents[j].nr_pages << PAGE_SHIFT);
					}
					kvfree(extents);
					kfree(ctx);
					return err;
				}
			}
			ctx->pat_tracked = true;
		}
	}

	ctx->extents = extents;
	ctx->nr_extents = nr_extents;
	refcount_set(&ctx->refcount, 1);

	file = kvm_gmem_create_ro_file(size, &kvm_gmem_luo_ro_fops, ctx);
	if (IS_ERR(file)) {
		if (ctx->pat_tracked) {
			for (i = 0; i < ctx->nr_extents; i++) {
				pfnmap_untrack(ctx->extents[i].pfn,
					       ctx->extents[i].nr_pages << PAGE_SHIFT);
			}
		}
		kvfree(extents);
		kfree(ctx);
		return PTR_ERR(file);
	}

	args->file = file;
	return 0;
}
#endif /* CONFIG_KEXEC_HANDOVER_READONLY */

static const struct liveupdate_file_ops kvm_gmem_luo_file_ops = {
	.can_preserve = kvm_gmem_luo_can_preserve,
	.preserve = kvm_gmem_luo_preserve,
	.retrieve = kvm_gmem_luo_retrieve,
	.unpreserve = kvm_gmem_luo_unpreserve,
	.finish = kvm_gmem_luo_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler kvm_gmem_luo_handler = {
	.ops = &kvm_gmem_luo_file_ops,
	.compatible = GUEST_MEMFD_LUO_FH_COMPATIBLE,
	.flags = LIVEUPDATE_FH_RO_RETRIEVE,
};

int kvm_gmem_luo_init(void)
{
	int err = liveupdate_register_file_handler(&kvm_gmem_luo_handler);

	if (err && err != -EOPNOTSUPP) {
		pr_err("Could not register luo filesystem handler: %pe\n", ERR_PTR(err));
		return err;
	}

	return 0;
}

void kvm_gmem_luo_exit(void)
{
	liveupdate_unregister_file_handler(&kvm_gmem_luo_handler);
}

