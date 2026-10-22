// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <ptyadav@amazon.de>
 */

/**
 * DOC: Memfd and Tmpfs Preservation via LUO
 *
 * Overview
 * ========
 *
 * Memory file descriptors (memfd) and tmpfs files can be preserved over a kexec
 * using the Live Update Orchestrator (LUO) file preservation. This allows
 * userspace to transfer its memory contents to the next kernel after a kexec.
 *
 * Here, a memfd is a memfd created without ``MFD_HUGETLB``, which is a shmem
 * file on the kernel's internal shmem mount, and a tmpfs file is a file on any
 * other tmpfs mount. Memfds created with ``MFD_HUGETLB`` and other files on
 * hugetlbfs are not shmem files, and this file handler doesn't preserve them.
 * Preserving them fails with ``-ENOENT``.
 *
 * The preservation is not intended to be transparent. Only select properties of
 * the file are preserved. All others are reset to default. The preserved
 * properties are described below.
 *
 * .. note::
 *    The LUO API is not stabilized yet, so the preserved properties of a memfd
 *    or tmpfs file are also not stable and are subject to backwards
 *    incompatible changes.
 *
 * Preserved Properties
 * ====================
 *
 * The following properties of the file are preserved across kexec:
 *
 * File Contents
 *   All data stored in the file is preserved.
 *
 * File Size
 *   The size of the file is preserved. Holes in the file are filled by
 *   allocating pages for them during preservation.
 *
 * File Position
 *   The file position at the time the session is frozen is preserved,
 *   allowing applications to continue reading/writing from their last
 *   position.
 *
 * File Status Flags
 *   memfds are always opened with ``O_RDWR`` and ``O_LARGEFILE``. This property
 *   is maintained. The file status flags of tmpfs files are not preserved.
 *   Userspace chooses the flags when retrieving them, as described in
 *   "Retrieving Tmpfs Files" below.
 *
 * Seals
 *   The seals of the file are preserved and re-applied on restore.
 *   Only seals known to this LUO version (see ``SHMEM_LUO_ALL_SEALS``) may
 *   be present; preservation fails with ``-EOPNOTSUPP`` otherwise.
 *
 * Non-Preserved Properties
 * ========================
 *
 * All properties which are not preserved must be assumed to be reset to
 * default. This section describes some of those properties which may be more of
 * note.
 *
 * ``FD_CLOEXEC`` flag
 *   A memfd can be created with the ``MFD_CLOEXEC`` flag that sets the
 *   ``FD_CLOEXEC`` on the file. This flag is not preserved and must be set
 *   again after restore via ``fcntl()``.
 *
 * Inode Attributes
 *   The name, path, owner, mode, timestamps and extended attributes of the
 *   file are not preserved.
 *
 * Retrieving Tmpfs Files
 * ======================
 *
 * A tmpfs file can be retrieved onto any tmpfs mount that userspace chooses.
 * The mount that the file was on doesn't exist in the new kernel, and userspace
 * may want to put the file elsewhere anyway.
 *
 * To retrieve a tmpfs file, userspace passes a
 * ``struct liveupdate_shmem_retrieve_arg`` to
 * ``LIVEUPDATE_SESSION_RETRIEVE_FD``. The argument specifies a directory on the
 * target mount, and the flags and mode of the new file. The kernel creates the
 * file as if by ``openat(dirfd, ".", O_TMPFILE | flags, mode)``, so the file
 * gets the same flags, owner, permission checks and security labels as any
 * other new file in that directory. The kernel then fills the file with the
 * preserved contents, size, position and seals before it returns the file, so
 * nothing else can access the file before it is complete.
 *
 * Like any ``O_TMPFILE`` file, the new file is not linked into the filesystem.
 * Userspace can link it at the path it wants with ``linkat()``, unless it asked
 * for ``O_EXCL``. Like with ``open()``, the file must be opened for writing.
 * LUO keeps the file open until the session is finished, so a retrieved binary
 * can't be executed until then.
 *
 * The blocks of the file are charged to the target mount, so the size limit
 * and quotas of the mount apply. If the file doesn't fit, the retrieve fails
 * permanently, since the preserved contents have already been restored at that
 * point. If the argument is invalid or the file can't be created, the retrieve
 * fails before the preserved contents are restored, so it can be retried.
 *
 * memfds can only be retrieved as memfds, and files on tmpfs mounts can only be
 * retrieved onto tmpfs mounts. Retrieving a memfd with the argument, or a tmpfs
 * file without it, fails with ``-EINVAL``.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/shmem.h>
#include <linux/liveupdate.h>
#include <linux/shmem_fs.h>
#include <linux/vmalloc.h>
#include <linux/memfd.h>
#include <uapi/linux/magic.h>
#include <uapi/linux/memfd.h>

#include "internal.h"

static int shmem_luo_preserve_folios(struct file *file,
				     struct kho_vmalloc *kho_vmalloc,
				     struct shmem_luo_folio_ser **out_folios_ser,
				     u64 *nr_foliosp)
{
	struct inode *inode = file_inode(file);
	struct shmem_luo_folio_ser *folios_ser;
	unsigned int max_folios;
	long i, size, nr_pinned;
	struct folio **folios;
	int err = -EINVAL;
	pgoff_t offset;
	u64 nr_folios;

	size = i_size_read(inode);
	/*
	 * If the file has zero size, then the folios and nr_folios properties
	 * are not set.
	 */
	if (!size) {
		*nr_foliosp = 0;
		*out_folios_ser = NULL;
		return 0;
	}

	/*
	 * Guess the number of folios based on inode size. Real number might end
	 * up being smaller if there are higher order folios.
	 */
	max_folios = PAGE_ALIGN(size) / PAGE_SIZE;
	folios = kvmalloc_objs(*folios, max_folios);
	if (!folios)
		return -ENOMEM;

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
	nr_folios = nr_pinned;

	folios_ser = vcalloc(nr_folios, sizeof(*folios_ser));
	if (!folios_ser) {
		err = -ENOMEM;
		goto err_unpin;
	}

	for (i = 0; i < nr_folios; i++) {
		struct shmem_luo_folio_ser *pfolio = &folios_ser[i];
		struct folio *folio = folios[i];

		err = kho_preserve_folio(folio);
		if (err)
			goto err_unpreserve;

		folio_lock(folio);

		/*
		 * A dirty folio is one which has been written to. A clean folio
		 * is its opposite. Since a clean folio does not carry user
		 * data, it can be freed by page reclaim under memory pressure.
		 *
		 * Saving the dirty flag at prepare() time doesn't work since it
		 * can change later. Saving it at freeze() also won't work
		 * because the dirty bit is normally synced at unmap and there
		 * might still be a mapping of the file at freeze().
		 *
		 * To see why this is a problem, say a folio is clean at
		 * preserve, but gets dirtied later. The pfolio flags will mark
		 * it as clean. After retrieve, the next kernel might try to
		 * reclaim this folio under memory pressure, losing user data.
		 *
		 * Unconditionally mark it dirty to avoid this problem. This
		 * comes at the cost of making clean folios un-reclaimable after
		 * live update.
		 */
		folio_mark_dirty(folio);

		/*
		 * If the folio is not uptodate, it was fallocated but never
		 * used. Saving this flag at prepare() doesn't work since it
		 * might change later when someone uses the folio.
		 *
		 * Since we have taken the performance penalty of allocating,
		 * zeroing, and pinning all the folios in the holes, take a bit
		 * more and zero all non-uptodate folios too.
		 *
		 * NOTE: For someone looking to improve preserve performance,
		 * this is a good place to look.
		 */
		if (!folio_test_uptodate(folio)) {
			folio_zero_range(folio, 0, folio_size(folio));
			flush_dcache_folio(folio);
			folio_mark_uptodate(folio);
		}

		folio_unlock(folio);

		pfolio->pfn = folio_pfn(folio);
		pfolio->flags = SHMEM_LUO_FOLIO_DIRTY | SHMEM_LUO_FOLIO_UPTODATE;
		pfolio->index = folio->index;
	}

	err = kho_preserve_vmalloc(folios_ser, kho_vmalloc);
	if (err)
		goto err_unpreserve;

	kvfree(folios);
	*nr_foliosp = nr_folios;
	*out_folios_ser = folios_ser;

	/*
	 * Note: folios_ser is purposely not freed here. It is preserved
	 * memory (via KHO). In the 'unpreserve' path, we use the vmap pointer
	 * that is passed via private_data.
	 */
	return 0;

err_unpreserve:
	for (i = i - 1; i >= 0; i--)
		kho_unpreserve_folio(folios[i]);
	vfree(folios_ser);
err_unpin:
	unpin_folios(folios, nr_folios);
err_free_folios:
	kvfree(folios);

	return err;
}

static void shmem_luo_unpreserve_folios(struct kho_vmalloc *kho_vmalloc,
					struct shmem_luo_folio_ser *folios_ser,
					u64 nr_folios)
{
	long i;

	if (!nr_folios)
		return;

	kho_unpreserve_vmalloc(kho_vmalloc);

	for (i = 0; i < nr_folios; i++) {
		const struct shmem_luo_folio_ser *pfolio = &folios_ser[i];
		struct folio *folio;

		if (!pfolio->pfn)
			continue;

		folio = pfn_folio(pfolio->pfn);

		kho_unpreserve_folio(folio);
		unpin_folio(folio);
	}

	vfree(folios_ser);
}

static int shmem_luo_preserve(struct liveupdate_file_op_args *args)
{
	struct inode *inode = file_inode(args->file);
	struct shmem_luo_folio_ser *folios_ser;
	struct shmem_luo_ser *ser;
	u64 nr_folios, inode_size;
	int err = 0, seals;

	inode_lock(inode);
	shmem_freeze(inode, true);

	/* Allocate the main serialization structure in preserved memory */
	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser)) {
		err = PTR_ERR(ser);
		goto err_unlock;
	}

	seals = memfd_get_seals(args->file);
	if (seals < 0) {
		err = seals;
		goto err_free_ser;
	}

	/* Make sure the file only has the seals supported by this version. */
	if (seals & ~SHMEM_LUO_ALL_SEALS) {
		err = -EOPNOTSUPP;
		goto err_free_ser;
	}

	ser->pos = args->file->f_pos;
	inode_size = i_size_read(inode);

	/*
	 * memfd_pin_folios() caps at UINT_MAX folios; refuse larger
	 * files to avoid silently preserving only a prefix.
	 */
	if (DIV_ROUND_UP_ULL(inode_size, PAGE_SIZE) > UINT_MAX) {
		err = -EFBIG;
		goto err_free_ser;
	}

	ser->size = inode_size;
	ser->seals = seals;
	if (!shmem_inode_on_internal_mount(inode))
		ser->flags |= SHMEM_LUO_FLAG_TMPFS;

	err = shmem_luo_preserve_folios(args->file, &ser->folios,
					&folios_ser, &nr_folios);
	if (err)
		goto err_free_ser;

	ser->nr_folios = nr_folios;
	inode_unlock(inode);

	args->private_data = folios_ser;
	args->serialized_data = virt_to_phys(ser);

	return 0;

err_free_ser:
	kho_unpreserve_free(ser);
err_unlock:
	shmem_freeze(inode, false);
	inode_unlock(inode);
	return err;
}

static int shmem_luo_freeze(struct liveupdate_file_op_args *args)
{
	struct shmem_luo_ser *ser;

	if (WARN_ON_ONCE(!args->serialized_data))
		return -EINVAL;

	ser = phys_to_virt(args->serialized_data);

	/*
	 * The pos might have changed since prepare. Everything else stays the
	 * same.
	 */
	ser->pos = args->file->f_pos;

	return 0;
}

static void shmem_luo_unpreserve(struct liveupdate_file_op_args *args)
{
	struct inode *inode = file_inode(args->file);
	struct shmem_luo_ser *ser;

	if (WARN_ON_ONCE(!args->serialized_data))
		return;

	inode_lock(inode);
	shmem_freeze(inode, false);

	ser = phys_to_virt(args->serialized_data);

	shmem_luo_unpreserve_folios(&ser->folios, args->private_data,
				    ser->nr_folios);

	kho_unpreserve_free(ser);
	inode_unlock(inode);
}

static void shmem_luo_discard_folios(const struct shmem_luo_folio_ser *folios_ser,
				     u64 nr_folios)
{
	u64 i;

	for (i = 0; i < nr_folios; i++) {
		const struct shmem_luo_folio_ser *pfolio = &folios_ser[i];
		struct folio *folio;
		phys_addr_t phys;

		if (!pfolio->pfn)
			continue;

		phys = PFN_PHYS(pfolio->pfn);
		folio = kho_restore_folio(phys);
		if (!folio) {
			pr_warn_ratelimited("Unable to restore folio at physical address: %llx\n",
					    phys);
			continue;
		}

		folio_put(folio);
	}
}

static void shmem_luo_finish(struct liveupdate_file_op_args *args)
{
	struct shmem_luo_folio_ser *folios_ser;
	struct shmem_luo_ser *ser;

	/*
	 * If retrieve was successful, nothing to do. If it failed permanently,
	 * retrieve() already cleaned up everything it could. So nothing to do
	 * there either. Only need to clean up when the file was not retrieved.
	 */
	if (args->retrieve_status)
		return;

	ser = phys_to_virt(args->serialized_data);
	if (!ser)
		return;

	if (ser->nr_folios) {
		folios_ser = kho_restore_vmalloc(&ser->folios);
		if (!folios_ser)
			goto out;

		shmem_luo_discard_folios(folios_ser, ser->nr_folios);
		vfree(folios_ser);
	}

out:
	kho_restore_free(ser);
}

static int shmem_luo_retrieve_folios(struct file *file,
				     struct shmem_luo_folio_ser *folios_ser,
				     u64 nr_folios)
{
	struct inode *inode = file_inode(file);
	struct address_space *mapping = inode->i_mapping;
	struct folio *folio;
	long npages, nr_added_pages = 0;
	int err = -EIO;
	long i;

	for (i = 0; i < nr_folios; i++) {
		const struct shmem_luo_folio_ser *pfolio = &folios_ser[i];
		phys_addr_t phys;
		u64 index;
		int flags;

		if (!pfolio->pfn)
			continue;

		phys = PFN_PHYS(pfolio->pfn);
		folio = kho_restore_folio(phys);
		if (!folio) {
			pr_err("Unable to restore folio at physical address: %llx\n",
			       phys);
			err = -EIO;
			goto put_folios;
		}
		index = pfolio->index;
		flags = pfolio->flags;

		/* Set up the folio for insertion. */
		__folio_set_locked(folio);
		__folio_set_swapbacked(folio);

		err = mem_cgroup_charge(folio, NULL, mapping_gfp_mask(mapping));
		if (err) {
			pr_err("shmem: failed to charge folio index %ld: %d\n",
			       i, err);
			goto unlock_folio;
		}

		err = shmem_add_to_page_cache(folio, mapping, index, NULL,
					      mapping_gfp_mask(mapping));
		if (err) {
			pr_err("shmem: failed to add to page cache folio index %ld: %d\n",
			       i, err);
			goto unlock_folio;
		}

		if (flags & SHMEM_LUO_FOLIO_UPTODATE)
			folio_mark_uptodate(folio);
		if (flags & SHMEM_LUO_FOLIO_DIRTY)
			folio_mark_dirty(folio);

		npages = folio_nr_pages(folio);
		err = shmem_inode_acct_blocks(inode, npages);
		if (err) {
			pr_err("shmem: failed to account folio index %ld(%ld pages): %d\n",
			       i, npages, err);
			goto remove_from_cache;
		}

		nr_added_pages += npages;
		folio_add_lru(folio);
		folio_unlock(folio);
		folio_put(folio);
	}

	shmem_recalc_inode(inode, nr_added_pages, 0);

	return 0;

remove_from_cache:
	filemap_remove_folio(folio);
unlock_folio:
	folio_unlock(folio);
	folio_put(folio);
put_folios:
	/*
	 * Note: don't free the folios already added to the file. They will be
	 * freed when the file is freed. Free the ones not added yet here.
	 */
	for (long j = i + 1; j < nr_folios; j++) {
		const struct shmem_luo_folio_ser *pfolio = &folios_ser[j];
		phys_addr_t phys;

		if (!pfolio->pfn)
			continue;

		phys = PFN_PHYS(pfolio->pfn);
		folio = kho_restore_folio(phys);
		if (folio)
			folio_put(folio);
	}

	shmem_recalc_inode(inode, nr_added_pages, 0);

	return err;
}

/* Create the memfd to retrieve a file from the internal shmem mount into. */
static struct file *shmem_luo_create_memfd(const struct liveupdate_shmem_retrieve_arg *arg,
					   unsigned int seals)
{
	struct file *file;
	int err;

	/* memfds are retrieved as memfds, so there is nothing to choose. */
	if (arg)
		return ERR_PTR(-EINVAL);

	/*
	 * The seals are preserved. Allow sealing here so they can be added
	 * later.
	 */
	file = memfd_alloc_file("", MFD_ALLOW_SEALING);
	if (IS_ERR(file)) {
		pr_err("failed to setup file: %pe\n", file);
		return file;
	}

	err = memfd_add_seals(file, seals);
	if (err) {
		pr_err("failed to add seals: %pe\n", ERR_PTR(err));
		fput(file);
		return ERR_PTR(err);
	}

	return file;
}

/*
 * Create the file to retrieve a file from a tmpfs mount into, on the tmpfs
 * mount chosen by userspace.
 */
static struct file *shmem_luo_create_tmpfs_file(const struct liveupdate_shmem_retrieve_arg *arg,
						unsigned int seals)
{
	struct file *file;
	int err;

	if (!arg)
		return ERR_PTR(-EINVAL);

	/*
	 * Like openat2(), reject unknown flags and mode bits instead of
	 * ignoring them. Also reject O_PATH, which would make file_open_root()
	 * ignore O_TMPFILE and open the directory instead of creating a file.
	 */
	if (arg->flags & ~(u64)VALID_OPEN_FLAGS || arg->flags & O_PATH ||
	    arg->mode & ~(u64)S_IALLUGO)
		return ERR_PTR(-EINVAL);

	CLASS(fd_raw, dir)(arg->dirfd);
	if (fd_empty(dir))
		return ERR_PTR(-EBADF);

	if (fd_file(dir)->f_path.dentry->d_sb->s_magic != TMPFS_MAGIC)
		return ERR_PTR(-EINVAL);

	/*
	 * Create the file like open(O_TMPFILE) does, so that it gets the same
	 * flag checks, permission checks, owner and security labels.
	 */
	file = file_open_root(&fd_file(dir)->f_path, ".",
			      O_TMPFILE | O_LARGEFILE | arg->flags, arg->mode);
	if (IS_ERR(file))
		return file;

	/*
	 * Nothing else can access the new file yet, so set the preserved seals
	 * directly. memfd_add_seals() refuses to, since files on tmpfs mounts
	 * are created with F_SEAL_SEAL.
	 */
	if (seals & F_SEAL_WRITE) {
		err = mapping_deny_writable(file->f_mapping);
		if (err) {
			fput(file);
			return ERR_PTR(err);
		}
	}
	SHMEM_I(file_inode(file))->seals = seals;

	return file;
}

static int shmem_luo_retrieve(struct liveupdate_file_op_args *args)
{
	struct shmem_luo_folio_ser *folios_ser;
	struct shmem_luo_ser *ser;
	struct file *file;
	int err;

	ser = phys_to_virt(args->serialized_data);
	if (!ser)
		return -EINVAL;

	/*
	 * Nothing has consumed the preserved state until the folios are
	 * restored below, so failures before that can be retried. If no
	 * retrieve succeeds, finish() frees the preserved state.
	 */
	args->retryable = true;

	/* Make sure the file only has seals and flags this version supports. */
	if ((ser->seals & ~SHMEM_LUO_ALL_SEALS) ||
	    (ser->flags & ~SHMEM_LUO_ALL_FLAGS))
		return -EOPNOTSUPP;

	/* Retrieve memfds as memfds, and other files onto a tmpfs mount. */
	if (ser->flags & SHMEM_LUO_FLAG_TMPFS)
		file = shmem_luo_create_tmpfs_file(args->arg, ser->seals);
	else
		file = shmem_luo_create_memfd(args->arg, ser->seals);
	if (IS_ERR(file))
		return PTR_ERR(file);

	vfs_setpos(file, ser->pos, MAX_LFS_FILESIZE);
	i_size_write(file_inode(file), ser->size);

	/* The preserved state is consumed from here on. */
	args->retryable = false;

	if (ser->nr_folios) {
		folios_ser = kho_restore_vmalloc(&ser->folios);
		if (!folios_ser) {
			err = -EINVAL;
			goto free_ser;
		}

		err = shmem_luo_retrieve_folios(file, folios_ser, ser->nr_folios);
		vfree(folios_ser);
		if (err)
			goto free_ser;
	}

	args->file = file;
	kho_restore_free(ser);

	return 0;

free_ser:
	kho_restore_free(ser);
	fput(file);
	return err;
}

static bool shmem_luo_can_preserve(struct liveupdate_file_handler *handler,
				   struct file *file)
{
	struct inode *inode = file_inode(file);

	return shmem_file(file) && !inode->i_nlink;
}

static unsigned long shmem_luo_get_id(struct file *file)
{
	return (unsigned long)file_inode(file);
}

static const struct liveupdate_file_ops shmem_luo_file_ops = {
	.freeze = shmem_luo_freeze,
	.finish = shmem_luo_finish,
	.retrieve = shmem_luo_retrieve,
	.preserve = shmem_luo_preserve,
	.unpreserve = shmem_luo_unpreserve,
	.can_preserve = shmem_luo_can_preserve,
	.get_id = shmem_luo_get_id,
	.rarg = LIVEUPDATE_ARG(struct liveupdate_shmem_retrieve_arg, mode),
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler shmem_luo_handler = {
	.ops = &shmem_luo_file_ops,
	.compatible = SHMEM_LUO_FH_COMPATIBLE,
};

static int __init shmem_luo_init(void)
{
	int err = liveupdate_register_file_handler(&shmem_luo_handler);

	if (err && err != -EOPNOTSUPP) {
		pr_err("Could not register luo filesystem handler: %pe\n",
		       ERR_PTR(err));

		return err;
	}

	return 0;
}
late_initcall(shmem_luo_init);
