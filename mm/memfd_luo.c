// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 * Changyuan Lyu <changyuanl@google.com>
 */

#include <linux/file.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/liveupdate.h>
#include <linux/kexec_handover.h>
#include <linux/shmem_fs.h>
#include "internal.h"

static const char memfd_luo_compatible[] = "memfd-v1";

static void memfd_luo_unpreserve_folios(const void *fdt)
{
	const u64(*folios)[2];
	int len;
	int i;

	folios = fdt_getprop(fdt, 0, "folios", &len);
	if (!folios)
		return;

	for (i = 0; i < len / sizeof(*folios); i++) {
		phys_addr_t phys = folios[i][0];
		struct folio *f = page_folio(phys_to_page(phys));

		kho_unpreserve_folio(f);
		folio_put(f);
	}
}

static int memfd_luo_preserve_folio(void *fdt, struct folio *f)
{
	int err;
	u64 phys = PFN_PHYS(folio_pfn(f));
	u64 index = folio_index(f);

	pr_err("to preserve: folio=%llx, index=%llu\n", phys, index);

	err = kho_preserve_folio(f);
	if (err)
		goto put;

	u64 val[2] = { phys, index };
	err = fdt_appendprop(fdt, 0, "folios", val, sizeof(val));
	if (err)
		goto unpreserve;

	return 0;

unpreserve:
	kho_unpreserve_folio(f);
put:
	folio_put(f);
	return err;
}

static int memfd_luo_preserve_folios(struct inode *inode, void *fdt)
{
	int err = 0;
	pgoff_t start = 0;
	const pgoff_t end = -1;
	struct folio_batch fbatch;
	pgoff_t indices[PAGEVEC_SIZE];

	folio_batch_init(&fbatch);

	while (start < end) {
		int count;

		count = find_get_entries(inode->i_mapping, &start, end - 1,
					 &fbatch, indices);
		if (count == 0)
			break;

		for (int i = 0; i < count; i++) {
			struct folio *f = fbatch.folios[i];

			if (xa_is_value(f)) {
				pr_err("TODO: handle swap");
				err = -ENOTSUPP;
				break;
			}

			err = memfd_luo_preserve_folio(fdt, f);
			if (err)
				break;

			pr_err("preserved");
		}
		folio_batch_remove_exceptionals(&fbatch);
		folio_batch_release(&fbatch);
		if (err)
			break;
	}

	if (err)
		memfd_luo_unpreserve_folios(fdt);

	return err;
}

static int memfd_luo_prepare(struct file *file, void *arg, u64 *data)
{
	int err = 0;
	struct page *fdt_page = NULL;
	void *fdt = NULL;
	u64 pos = file->f_pos;
	u64 size = file->f_inode->i_size;

	fdt_page = alloc_page(GFP_KERNEL);
	if (!fdt_page)
		return -ENOMEM;

	fdt = page_to_virt(fdt_page);
	err = fdt_create_empty_tree(fdt, PAGE_SIZE);
	if (err)
		goto free;

	err = fdt_setprop(fdt, 0, "pos", &pos, sizeof(pos));
	if (err)
		goto free;

	err = fdt_setprop(fdt, 0, "size", &size, sizeof(size));
	if (err)
		goto free;

	err = kho_preserve_folio(page_folio(fdt_page));
	if (err)
		goto free;

	err = memfd_luo_preserve_folios(file->f_inode, fdt);
	if (err)
		goto unpreserve;

	*data = (u64)virt_to_phys(fdt);
	return 0;

unpreserve:
	kho_unpreserve_folio(page_folio(fdt_page));
free:
	free_page((unsigned long)fdt_page);
	return err;
}

static void memfd_luo_cancel(struct file *file, void *arg, u64 data)
{
	void *fdt = phys_to_virt(data);
	struct folio *fdt_folio = virt_to_folio(fdt);

	memfd_luo_unpreserve_folios(fdt);
	kho_unpreserve_folio(fdt_folio);
	folio_put(fdt_folio);
	pr_err("unpreserved fdt_folio = %llx\n", data);
}

static void memfd_luo_finish(struct file *file, void *arg, u64 data,
			     bool reclaimed)
{
	phys_addr_t fdt_phys = data;
	void *fdt = phys_to_virt(fdt_phys);
	struct folio *fdt_folio;

	if (!reclaimed) {
		const u64(*folios)[2];
		int len;
		int i;

		folios = fdt_getprop(fdt, 0, "folios", &len);
		if (!folios)
			return;

		for (i = 0; i < len / sizeof(*folios); i++) {
			phys_addr_t phys = folios[i][0];
			struct folio *f = kho_restore_folio(phys);

			folio_put(f);
			pr_err("memfd_luo_finish, not relcaimed: %llx, phys=%llx\n",
			       data, phys);
		}
	}

	fdt_folio = kho_restore_folio(fdt_phys);
	folio_put(fdt_folio);
	pr_err("memfd_luo_finish, put fdt: %llx\n", fdt_phys);
}

static int memfd_luo_retrieve(void *arg, u64 data, struct file **file_p)
{
	const void *fdt = phys_to_virt((phys_addr_t)data);
	const u64(*folios)[2];
	int len_folios, len;
	int ret = 0;
	const u64 *pos, *size;
	struct file *file;
	int i = 0;
	struct inode *inode;
	struct address_space *mapping;
	struct folio *folio;

	folios = fdt_getprop(fdt, 0, "folios", &len_folios);
	if (!folios || len_folios % sizeof(*folios)) {
		pr_err("invalid 'folios' property\n");
		return -EINVAL;
	}

	size = fdt_getprop(fdt, 0, "size", &len);
	if (!size || len != sizeof(u64)) {
		pr_err("invalid 'size' property\n");
		ret = -EINVAL;
		goto free;
	}

	pos = fdt_getprop(fdt, 0, "pos", &len);
	if (!pos || len != sizeof(u64)) {
		pr_err("invalid 'pos' property\n");
		ret = -EINVAL;
		goto free;
	}

	/*
	 * TODO: This sets UID/GID, cgroup accounting to root. Should this
	 * be given to the first user that maps the FD instead?
	 */
	file = shmem_file_setup("", 0, VM_NORESERVE);

	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		pr_err("failed to setup file: %d\n", ret);
		goto free;
	}

	inode = file->f_inode;
	mapping = inode->i_mapping;
	vfs_setpos(file, *pos, MAX_LFS_FILESIZE);

	for (; i < len_folios / sizeof(*folios); i++) {
		u64 index;

		folio = kho_restore_folio(folios[i][0]);
		if (!folio) {
			pr_err("invalid folio physical address: %llx\n",
			       folios[i][0]);
			goto put_file;
		}
		index = folios[i][1];

		/* Set up the folio for insertion. */

		/*
		 * TODO: This breaks falloc-ed folios since now they get marked
		 * uptodate when they might not actually be zeroed out yet. Need
		 * a way to distinguish falloc-ed folios.
		 */
		folio_mark_uptodate(folio);
		folio_mark_dirty(folio);

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
	return 0;

unlock_folio:
	folio_unlock(folio);
	folio_put(folio);
put_file:
	fput(file);
	i++;
free:
	for (; i < len_folios / sizeof(*folios); i++) {
		folio = kho_restore_folio(folios[i][0]);
		if (folio)
			folio_put(folio);
	}

	return ret;
}

static bool memfd_luo_can_preserve(struct file *file, void *arg)
{
	struct inode *inode = file_inode(file);

	return shmem_file(file) && !inode->i_nlink;
}

static struct liveupdate_filesystem memfd_luo_fs_ops = {
	.prepare = memfd_luo_prepare,
	.cancel = memfd_luo_cancel,
	.finish = memfd_luo_finish,
	.retrieve = memfd_luo_retrieve,
	.compatible = memfd_luo_compatible,
	.can_preserve = memfd_luo_can_preserve,
};

static int __init memfd_luo_init(void)
{
	int err;

	err = liveupdate_register_filesystem(&memfd_luo_fs_ops);
	if (err)
		pr_err("Could not register luo filesystem handler: %d\n", err);

	return err;
}
late_initcall(memfd_luo_init);
