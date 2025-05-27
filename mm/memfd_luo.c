// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 * Changyuan Lyu <changyuanl@google.com>
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <ptyadav@amazon.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/file.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/liveupdate.h>
#include <linux/kexec_handover.h>
#include <linux/shmem_fs.h>
#include <linux/bits.h>
#include "internal.h"

static const char memfd_luo_compatible[] = "memfd-v1";

#define PRESERVED_PFN_MASK		GENMASK(63, 12)
#define PRESERVED_PFN_SHIFT		12
#define PRESERVED_FLAG_DIRTY		BIT(0)
#define PRESERVED_FLAG_UPTODATE		BIT(1)

#define PRESERVED_FOLIO_PFN(desc)	(((desc) & PRESERVED_PFN_MASK) >> PRESERVED_PFN_SHIFT)
#define PRESERVED_FOLIO_FLAGS(desc)	((desc) & ~PRESERVED_PFN_MASK)
#define PRESERVED_FOLIO_MKDESC(pfn, flags) (((pfn) << PRESERVED_PFN_SHIFT) | (flags))

struct memfd_luo_preserved_folio {
	/*
	 * The folio descriptor is made of 2 parts. The bottom 12 bits are used
	 * for storing flags, the others for storing the PFN.
	 */
	u64 foliodesc;
	u64 index;
};

static int memfd_luo_preserve_folios(struct memfd_luo_preserved_folio *pfolios,
				     struct folio **folios,
				     unsigned int nr_folios)
{
	unsigned int i;
	int err;

	for (i = 0; i < nr_folios; i++) {
		struct memfd_luo_preserved_folio *pfolio = &pfolios[i];
		struct folio *folio = folios[i];
		unsigned int flags = 0;
		unsigned long pfn;

		err = kho_preserve_folio(folio);
		if (err)
			goto err_unpreserve;

		pfn = folio_pfn(folio);
		if (folio_test_dirty(folio))
			flags |= PRESERVED_FLAG_DIRTY;
		if (folio_test_uptodate(folio))
			flags |= PRESERVED_FLAG_UPTODATE;

		pfolio->foliodesc = PRESERVED_FOLIO_MKDESC(pfn, flags);
		pfolio->index = folio->index;
	}

	return 0;

err_unpreserve:
	i--;
	for (; i >= 0; i--)
		WARN_ON_ONCE(kho_unpreserve_folio(folios[i]));
	return err;
}

static void memfd_luo_unpreserve_folios(const struct memfd_luo_preserved_folio *pfolios,
					unsigned int nr_folios)
{
	unsigned int i;

	for (i = 0; i < nr_folios; i++) {
		const struct memfd_luo_preserved_folio *pfolio = &pfolios[i];
		struct folio *folio;

		if (!pfolio->foliodesc)
			continue;

		folio = pfn_folio(PRESERVED_FOLIO_PFN(pfolio->foliodesc));

		kho_unpreserve_folio(folio);
		unpin_folio(folio);
	}
}

static void *memfd_luo_create_fdt(unsigned long size)
{
	unsigned int order = get_order(size);
	struct folio *fdt_folio;
	int err = 0;
	void *fdt;

	if (order > MAX_PAGE_ORDER)
		return NULL;

	fdt_folio = folio_alloc(GFP_KERNEL, order);
	if (!fdt_folio)
		return NULL;

	fdt = folio_address(fdt_folio);

	err |= fdt_create(fdt, (1 << (order + PAGE_SHIFT)));
	err |= fdt_finish_reservemap(fdt);
	err |= fdt_begin_node(fdt, "");
	if (err)
		goto free;

	return fdt;

free:
	folio_put(fdt_folio);
	return NULL;
}

static int memfd_luo_finish_fdt(void *fdt)
{
	int err;

	err = fdt_end_node(fdt);
	if (err)
		return err;

	return fdt_finish(fdt);
}

static int memfd_luo_prepare(struct liveupdate_file_handler *handler,
			     struct file *file, u64 *data)
{
	struct memfd_luo_preserved_folio *preserved_folios;
	struct inode *inode = file_inode(file);
	unsigned int max_folios, nr_folios = 0;
	int err = 0, preserved_size;
	struct folio **folios;
	long size, nr_pinned;
	pgoff_t offset;
	void *fdt;
	u64 pos;

	if (WARN_ON_ONCE(!shmem_file(file)))
		return -EINVAL;

	inode_lock(inode);
	shmem_i_mapping_freeze(inode, true);

	size = i_size_read(inode);
	if ((PAGE_ALIGN(size) / PAGE_SIZE) > UINT_MAX) {
		err = -E2BIG;
		goto err_unlock;
	}

	/*
	 * Guess the number of folios based on inode size. Real number might end
	 * up being smaller if there are higher order folios.
	 */
	max_folios = PAGE_ALIGN(size) / PAGE_SIZE;
	folios = kvmalloc_array(max_folios, sizeof(*folios), GFP_KERNEL);
	if (!folios) {
		err = -ENOMEM;
		goto err_unfreeze;
	}

	/*
	 * Pin the folios so they don't move around behind our back. This also
	 * ensures none of the folios are in CMA -- which ensures they don't
	 * fall in KHO scratch memory. It also moves swapped out folios back to
	 * memory.
	 *
	 * A side effect of doing this is that it allocates a folio for all
	 * indices in the file. This might waste memory on sparse memfds. If
	 * that is really a problem in the future, we can have a
	 * memfd_pin_folios() variant that does not allocate a page on empty
	 * slots.
	 */
	nr_pinned = memfd_pin_folios(file, 0, size - 1, folios, max_folios,
				     &offset);
	if (nr_pinned < 0) {
		err = nr_pinned;
		pr_err("failed to pin folios: %d\n", err);
		goto err_free_folios;
	}
	/* nr_pinned won't be more than max_folios which is also unsigned int. */
	nr_folios = (unsigned int)nr_pinned;

	preserved_size = sizeof(struct memfd_luo_preserved_folio) * nr_folios;
	if (check_mul_overflow(sizeof(struct memfd_luo_preserved_folio),
			       nr_folios, &preserved_size)) {
		err = -E2BIG;
		goto err_unpin;
	}

	/*
	 * Most of the space should be taken by preserved folios. So take its
	 * size, plus a page for other properties.
	 */
	fdt = memfd_luo_create_fdt(PAGE_ALIGN(preserved_size) + PAGE_SIZE);
	if (!fdt) {
		err = -ENOMEM;
		goto err_unpin;
	}

	pos = file->f_pos;
	err = fdt_property(fdt, "pos", &pos, sizeof(pos));
	if (err)
		goto err_free_fdt;

	err = fdt_property(fdt, "size", &size, sizeof(size));
	if (err)
		goto err_free_fdt;

	err = fdt_property_placeholder(fdt, "folios", preserved_size,
				       (void **)&preserved_folios);
	if (err) {
		pr_err("Failed to reserve folios property in FDT: %s\n",
		       fdt_strerror(err));
		err = -ENOMEM;
		goto err_free_fdt;
	}

	err = memfd_luo_preserve_folios(preserved_folios, folios, nr_folios);
	if (err)
		goto err_free_fdt;

	err = memfd_luo_finish_fdt(fdt);
	if (err)
		goto err_unpreserve;

	err = kho_preserve_folio(virt_to_folio(fdt));
	if (err)
		goto err_unpreserve;

	kvfree(folios);
	inode_unlock(inode);

	*data = virt_to_phys(fdt);
	return 0;

err_unpreserve:
	memfd_luo_unpreserve_folios(preserved_folios, nr_folios);
err_free_fdt:
	folio_put(virt_to_folio(fdt));
err_unpin:
	unpin_folios(folios, nr_pinned);
err_free_folios:
	kvfree(folios);
err_unfreeze:
	shmem_i_mapping_freeze(inode, false);
err_unlock:
	inode_unlock(inode);
	return err;
}

static int memfd_luo_freeze(struct liveupdate_file_handler *handler,
			    struct file *file, u64 *data)
{
	u64 pos = file->f_pos;
	void *fdt;
	int err;

	if (WARN_ON_ONCE(!*data))
		return -EINVAL;

	fdt = phys_to_virt(*data);

	/*
	 * The pos or size might have changed since prepare. Everything else
	 * stays the same.
	 */
	err = fdt_setprop(fdt, 0, "pos", &pos, sizeof(pos));
	if (err)
		return err;

	return 0;
}

static void memfd_luo_cancel(struct liveupdate_file_handler *handler,
			     struct file *file, u64 data)
{
	const struct memfd_luo_preserved_folio *pfolios;
	struct inode *inode = file_inode(file);
	struct folio *fdt_folio;
	void *fdt;
	int len;

	if (WARN_ON_ONCE(!data))
		return;

	inode_lock(inode);
	shmem_i_mapping_freeze(inode, false);

	fdt = phys_to_virt(data);
	fdt_folio = virt_to_folio(fdt);
	pfolios = fdt_getprop(fdt, 0, "folios", &len);
	if (pfolios)
		memfd_luo_unpreserve_folios(pfolios, len / sizeof(*pfolios));

	kho_unpreserve_folio(fdt_folio);
	folio_put(fdt_folio);
	inode_unlock(inode);
}

static struct folio *memfd_luo_get_fdt(u64 data)
{
	return kho_restore_folio((phys_addr_t)data);
}

static void memfd_luo_finish(struct liveupdate_file_handler *handler,
			     struct file *file, u64 data, bool reclaimed)
{
	const struct memfd_luo_preserved_folio *pfolios;
	struct folio *fdt_folio;
	int len;

	if (reclaimed)
		return;

	fdt_folio = memfd_luo_get_fdt(data);

	pfolios = fdt_getprop(folio_address(fdt_folio), 0, "folios", &len);
	if (pfolios)
		memfd_luo_unpreserve_folios(pfolios, len / sizeof(*pfolios));

	folio_put(fdt_folio);
}

static int memfd_luo_retrieve(struct liveupdate_file_handler *handler, u64 data,
			      struct file **file_p)
{
	const struct memfd_luo_preserved_folio *pfolios;
	int nr_pfolios, len, ret = 0, i = 0;
	struct address_space *mapping;
	struct folio *folio, *fdt_folio;
	const u64 *pos, *size;
	struct inode *inode;
	struct file *file;
	const void *fdt;

	fdt_folio = memfd_luo_get_fdt(data);
	if (!fdt_folio)
		return -ENOENT;

	fdt = page_to_virt(folio_page(fdt_folio, 0));

	pfolios = fdt_getprop(fdt, 0, "folios", &len);
	if (!pfolios || len % sizeof(*pfolios)) {
		pr_err("invalid 'folios' property\n");
		ret = -EINVAL;
		goto put_fdt;
	}
	nr_pfolios = len / sizeof(*pfolios);

	size = fdt_getprop(fdt, 0, "size", &len);
	if (!size || len != sizeof(u64)) {
		pr_err("invalid 'size' property\n");
		ret = -EINVAL;
		goto put_folios;
	}

	pos = fdt_getprop(fdt, 0, "pos", &len);
	if (!pos || len != sizeof(u64)) {
		pr_err("invalid 'pos' property\n");
		ret = -EINVAL;
		goto put_folios;
	}

	file = shmem_file_setup("", 0, VM_NORESERVE);

	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		pr_err("failed to setup file: %d\n", ret);
		goto put_folios;
	}

	inode = file->f_inode;
	mapping = inode->i_mapping;
	vfs_setpos(file, *pos, MAX_LFS_FILESIZE);

	for (; i < nr_pfolios; i++) {
		const struct memfd_luo_preserved_folio *pfolio = &pfolios[i];
		phys_addr_t phys;
		u64 index;
		int flags;

		if (!pfolio->foliodesc)
			continue;

		phys = PFN_PHYS(PRESERVED_FOLIO_PFN(pfolio->foliodesc));
		folio = kho_restore_folio(phys);
		if (!folio) {
			pr_err("Unable to restore folio at physical address: %llx\n",
			       phys);
			goto put_file;
		}
		index = pfolio->index;
		flags = PRESERVED_FOLIO_FLAGS(pfolio->foliodesc);

		/* Set up the folio for insertion. */
		/*
		 * TODO: Should find a way to unify this and
		 * shmem_alloc_and_add_folio().
		 */
		__folio_set_locked(folio);
		__folio_set_swapbacked(folio);

		ret = mem_cgroup_charge(folio, NULL, mapping_gfp_mask(mapping));
		if (ret) {
			pr_err("shmem: failed to charge folio index %d: %d\n",
			       i, ret);
			goto unlock_folio;
		}

		ret = shmem_add_to_page_cache(folio, mapping, index, NULL,
					      mapping_gfp_mask(mapping));
		if (ret) {
			pr_err("shmem: failed to add to page cache folio index %d: %d\n",
			       i, ret);
			goto unlock_folio;
		}

		if (flags & PRESERVED_FLAG_UPTODATE)
			folio_mark_uptodate(folio);
		if (flags & PRESERVED_FLAG_DIRTY)
			folio_mark_dirty(folio);

		ret = shmem_inode_acct_blocks(inode, 1);
		if (ret) {
			pr_err("shmem: failed to account folio index %d: %d\n",
			       i, ret);
			goto unlock_folio;
		}

		shmem_recalc_inode(inode, 1, 0);
		folio_add_lru(folio);
		folio_unlock(folio);
		folio_put(folio);
	}

	inode->i_size = *size;
	*file_p = file;
	folio_put(fdt_folio);
	return 0;

unlock_folio:
	folio_unlock(folio);
	folio_put(folio);
put_file:
	fput(file);
	i++;
put_folios:
	for (; i < nr_pfolios; i++) {
		const struct memfd_luo_preserved_folio *pfolio = &pfolios[i];

		folio = kho_restore_folio(PRESERVED_FOLIO_PFN(pfolio->foliodesc));
		if (folio)
			folio_put(folio);
	}

put_fdt:
	folio_put(fdt_folio);
	return ret;
}

static bool memfd_luo_can_preserve(struct liveupdate_file_handler *handler,
				   struct file *file)
{
	struct inode *inode = file_inode(file);

	return shmem_file(file) && !inode->i_nlink;
}

static const struct liveupdate_file_ops memfd_luo_file_ops = {
	.prepare = memfd_luo_prepare,
	.freeze = memfd_luo_freeze,
	.cancel = memfd_luo_cancel,
	.finish = memfd_luo_finish,
	.retrieve = memfd_luo_retrieve,
	.can_preserve = memfd_luo_can_preserve,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler memfd_luo_handler = {
	.ops = &memfd_luo_file_ops,
	.compatible = memfd_luo_compatible,
};

static int __init memfd_luo_init(void)
{
	int err;

	err = liveupdate_register_file_handler(&memfd_luo_handler);
	if (err)
		pr_err("Could not register luo filesystem handler: %d\n", err);

	return err;
}
late_initcall(memfd_luo_init);
