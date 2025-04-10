// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_handover.c - kexec handover metadata processing
 * Copyright (C) 2023 Alexander Graf <graf@amazon.com>
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (C) 2025 Google LLC, Changyuan Lyu <changyuanl@google.com>
 * Copyright (C) 2025 Google LLC, Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#define pr_fmt(fmt) "KHO: " fmt

#include <linux/init.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/mm.h>
#include "kexec_handover_internal.h"

static struct dentry *debugfs_root;

struct fdt_debugfs {
	struct list_head list;
	struct debugfs_blob_wrapper wrapper;
	struct dentry *file;
};

static int __kho_debugfs_fdt_add(struct list_head *list, struct dentry *dir,
				 const char *name, const void *fdt)
{
	struct fdt_debugfs *f;
	struct dentry *file;

	f = kmalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;

	f->wrapper.data = (void *)fdt;
	f->wrapper.size = fdt_totalsize(fdt);

	file = debugfs_create_blob(name, 0400, dir, &f->wrapper);
	if (IS_ERR(file)) {
		kfree(f);
		return PTR_ERR(file);
	}

	f->file = file;
	list_add(&f->list, list);

	return 0;
}

int kho_debugfs_fdt_add(struct kho_serialization *ser, const char *name,
			const void *fdt)
{
	return __kho_debugfs_fdt_add(&ser->fdt_list, ser->sub_fdt_dir, name,
				     fdt);
}

static int kho_out_update_debugfs_fdt(void)
{
	int err = 0;
	struct fdt_debugfs *ff, *tmp;

	if (kho_out.finalized) {
		err = __kho_debugfs_fdt_add(&kho_out.ser.fdt_list, kho_out.dir,
					    "fdt",
					    page_to_virt(kho_out.ser.fdt));
	} else {
		list_for_each_entry_safe(ff, tmp, &kho_out.ser.fdt_list, list) {
			debugfs_remove(ff->file);
			list_del(&ff->list);
			kfree(ff);
		}
	}

	return err;
}

static int kho_out_finalize_get(void *data, u64 *val)
{
	mutex_lock(&kho_out.lock);
	*val = kho_out.finalized;
	mutex_unlock(&kho_out.lock);

	return 0;
}

static int kho_out_finalize_set(void *data, u64 _val)
{
	int ret = 0;
	bool val = !!_val;

	mutex_lock(&kho_out.lock);

	if (val == kho_out.finalized) {
		if (kho_out.finalized)
			ret = -EEXIST;
		else
			ret = -ENOENT;
		goto unlock;
	}

	if (val)
		ret = __kho_finalize();
	else
		ret = __kho_abort();

	if (ret)
		goto unlock;

	kho_out.finalized = val;
	ret = kho_out_update_debugfs_fdt();

unlock:
	mutex_unlock(&kho_out.lock);
	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_kho_out_finalize, kho_out_finalize_get,
			 kho_out_finalize_set, "%llu\n");

static int scratch_phys_show(struct seq_file *m, void *v)
{
	for (int i = 0; i < kho_scratch_cnt; i++)
		seq_printf(m, "0x%llx\n", kho_scratch[i].addr);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(scratch_phys);

static int scratch_len_show(struct seq_file *m, void *v)
{
	for (int i = 0; i < kho_scratch_cnt; i++)
		seq_printf(m, "0x%llx\n", kho_scratch[i].size);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(scratch_len);

__init void kho_in_debugfs_init(const void *fdt)
{
	struct dentry *sub_fdt_dir;
	int err, child;

	kho_in.dir = debugfs_create_dir("in", debugfs_root);
	if (IS_ERR(kho_in.dir)) {
		err = PTR_ERR(kho_in.dir);
		goto err_out;
	}

	sub_fdt_dir = debugfs_create_dir("sub_fdts", kho_in.dir);
	if (IS_ERR(sub_fdt_dir)) {
		err = PTR_ERR(sub_fdt_dir);
		goto err_rmdir;
	}

	err = __kho_debugfs_fdt_add(&kho_in.fdt_list, kho_in.dir, "fdt", fdt);
	if (err)
		goto err_rmdir;

	fdt_for_each_subnode(child, fdt, 0) {
		int len = 0;
		const char *name = fdt_get_name(fdt, child, NULL);
		const u64 *fdt_phys;

		fdt_phys = fdt_getprop(fdt, child, "fdt", &len);
		if (!fdt_phys)
			continue;
		if (len != sizeof(*fdt_phys)) {
			pr_warn("node `%s`'s prop `fdt` has invalid length: %d\n",
				name, len);
			continue;
		}
		err = __kho_debugfs_fdt_add(&kho_in.fdt_list, sub_fdt_dir, name,
					    phys_to_virt(*fdt_phys));
		if (err) {
			pr_warn("failed to add fdt `%s` to debugfs: %d\n", name,
				err);
			continue;
		}
	}

	return;
err_rmdir:
	debugfs_remove_recursive(kho_in.dir);
err_out:
	/*
	 * Failure to create /sys/kernel/debug/kho/in does not prevent
	 * reviving state from KHO and setting up KHO for the next
	 * kexec.
	 */
	if (err)
		pr_err("failed exposing handover FDT in debugfs: %d\n", err);
}

__init int kho_out_debugfs_init(void)
{
	struct dentry *dir, *f, *sub_fdt_dir;

	dir = debugfs_create_dir("out", debugfs_root);
	if (IS_ERR(dir))
		return -ENOMEM;

	sub_fdt_dir = debugfs_create_dir("sub_fdts", dir);
	if (IS_ERR(sub_fdt_dir))
		goto err_rmdir;

	f = debugfs_create_file("scratch_phys", 0400, dir, NULL,
				&scratch_phys_fops);
	if (IS_ERR(f))
		goto err_rmdir;

	f = debugfs_create_file("scratch_len", 0400, dir, NULL,
				&scratch_len_fops);
	if (IS_ERR(f))
		goto err_rmdir;

	f = debugfs_create_file("finalize", 0600, dir, NULL,
				&fops_kho_out_finalize);
	if (IS_ERR(f))
		goto err_rmdir;

	kho_out.dir = dir;
	kho_out.ser.sub_fdt_dir = sub_fdt_dir;
	return 0;

err_rmdir:
	debugfs_remove_recursive(dir);
	return -ENOENT;
}

__init int kho_debugfs_init(void)
{
	debugfs_root = debugfs_create_dir("kho", NULL);
	if (IS_ERR(debugfs_root))
		return -ENOENT;
	return 0;
}
