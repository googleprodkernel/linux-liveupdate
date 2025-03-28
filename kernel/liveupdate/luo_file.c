// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: LUO file descriptors
 *
 * LUO provides the infrastructure necessary to preserve
 * specific types of stateful file descriptors across a kernel live
 * update transition. The primary goal is to allow workloads, such as virtual
 * machines using vfio, memfd, or iommufd to retain access to their essential
 * resources without interruption after the underlying kernel is  updated.
 *
 * The framework operates based on handler registration and instance tracking:
 *
 * 1. Handler Registration: Kernel modules responsible for specific file
 * types (e.g., memfd, vfio) register a &struct liveupdate_file_handler
 * handler. This handler contains callbacks
 * (&liveupdate_file_handler.ops->prepare,
 * &liveupdate_file_handler.ops->freeze,
 * &liveupdate_file_handler.ops->finish, etc.) and a unique 'compatible' string
 * identifying the file type. Registration occurs via
 * liveupdate_register_file_handler().
 *
 * 2. File Instance Tracking: When a potentially preservable file needs to be
 * managed for live update, the core LUO logic (luo_preserve_file()) finds a
 * compatible registered handler using its
 * &liveupdate_file_handler.ops->can_preserve callback. If found,  an internal
 * &struct luo_file instance is created, assigned a unique u64 'token', and
 * added to a list.
 *
 * 3. State Persistence ...
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xarray.h>
#include "luo_internal.h"

/* Registered files. */
static DECLARE_RWSEM(luo_file_handler_list_rwsem);
static LIST_HEAD(luo_file_handler_list);

/**
 * struct luo_file_ser - Represents the serialized preserves files.
 * @compatible:  File handler compatabile string.
 * @files:   Private data
 * @token:   User provided token for this file
 *
 * If this structure is modified, LUO_SESSION_COMPATIBLE must be updated.
 */
struct luo_file_ser {
	char compatible[LIVEUPDATE_HNDL_COMPAT_LENGTH];
	u64 data;
	u64 token;
} __packed;

/**
 * struct luo_file - Represents a file descriptor instance preserved
 * across live update.
 * @fh:            Pointer to the &struct liveupdate_file_handler containing
 *                 the implementation of prepare, freeze, cancel, and finish
 *                 operations specific to this file's type.
 * @file:          A pointer to the kernel's &struct file object representing
 *                 the open file descriptor that is being preserved.
 * @private_data:  Internal storage used by the live update core framework
 *                 between phases.
 * @reclaimed:     Flag indicating whether this preserved file descriptor has
 *                 been successfully 'reclaimed' (e.g., requested via an ioctl)
 *                 by user-space or the owning kernel subsystem in the new
 *                 kernel after the live update.
 * @state:         The current state of file descriptor, it is allowed to
 *                 prepare, freeze, and finish FDs before the global state
 *                 switch.
 * @mutex:         Lock to protect FD state, and allow independently to change
 *                 the FD state compared to global state.
 *
 * This structure holds the necessary callbacks and context for managing a
 * specific open file descriptor throughout the different phases of a live
 * update process. Instances of this structure are typically allocated,
 * populated with file-specific details (&file, &arg, callbacks, compatibility
 * string, token), and linked into a central list managed by the LUO. The
 * private_data field is used internally by the core logic to store state
 * between phases.
 */
struct luo_file {
	struct liveupdate_file_handler *fh;
	struct file *file;
	u64 private_data;
	bool reclaimed;
	enum liveupdate_state state;
	struct mutex mutex;
};

static int luo_file_prepare_one(struct luo_file *h)
{
	int ret = 0;

	guard(mutex)(&h->mutex);
	if (h->state == LIVEUPDATE_STATE_NORMAL) {
		if (h->fh->ops->prepare) {
			ret = h->fh->ops->prepare(h->fh, h->file,
						  &h->private_data);
		}
		if (!ret)
			h->state = LIVEUPDATE_STATE_PREPARED;
	} else {
		WARN_ON_ONCE(h->state != LIVEUPDATE_STATE_PREPARED &&
			     h->state != LIVEUPDATE_STATE_FROZEN);
	}

	return ret;
}

static int luo_file_freeze_one(struct luo_file *h)
{
	int ret = 0;

	guard(mutex)(&h->mutex);
	if (h->state == LIVEUPDATE_STATE_PREPARED) {
		if (h->fh->ops->freeze) {
			ret = h->fh->ops->freeze(h->fh, h->file,
						 &h->private_data);
		}
		if (!ret)
			h->state = LIVEUPDATE_STATE_FROZEN;
	} else {
		WARN_ON_ONCE(h->state != LIVEUPDATE_STATE_FROZEN);
	}

	return ret;
}

static void luo_file_finish_one(struct luo_file *h)
{
	guard(mutex)(&h->mutex);
	if (h->state == LIVEUPDATE_STATE_UPDATED) {
		if (h->fh->ops->finish) {
			h->fh->ops->finish(h->fh, h->file, h->private_data,
					   h->reclaimed);
		}
		h->state = LIVEUPDATE_STATE_NORMAL;
	} else {
		WARN_ON_ONCE(h->state != LIVEUPDATE_STATE_NORMAL);
	}
}

static void luo_file_cancel_one(struct luo_file *h)
{
	guard(mutex)(&h->mutex);
	if (h->state == LIVEUPDATE_STATE_NORMAL)
		return;

	if (WARN_ON_ONCE(h->state != LIVEUPDATE_STATE_PREPARED &&
			 h->state != LIVEUPDATE_STATE_FROZEN)) {
		return;
	}

	if (h->fh->ops->cancel)
		h->fh->ops->cancel(h->fh, h->file, h->private_data);

	h->private_data = 0;
	h->state = LIVEUPDATE_STATE_NORMAL;
}

static void __luo_file_cancel(struct luo_session *session)
{
	unsigned long token;
	struct luo_file *h;

	xa_for_each(&session->files_xa, token, h)
		luo_file_cancel_one(h);
}

int luo_file_prepare(struct luo_session *session)
{
	struct luo_file *luo_file;
	struct luo_file_ser *ser;
	unsigned long token;
	size_t ser_size;
	int ret = 0;
	int i;

	if (!session->count)
		return 0;

	ser_size = session->count * sizeof(struct luo_file_ser);
	ser = luo_contig_alloc_preserve(ser_size);
	if (IS_ERR(ser))
		return PTR_ERR(ser);

	i = 0;
	xa_for_each(&session->files_xa, token, luo_file) {
		ret = luo_file_prepare_one(luo_file);
		if (ret < 0) {
			pr_err("Prepare failed for session[%s] token[%#0llx] handler[%s] ret[%d]\n",
			       session->name, (u64)token, luo_file->fh->compatible, ret);
			goto exit_cleanup;
		}

		strscpy(ser[i].compatible, luo_file->fh->compatible,
			sizeof(ser[i].compatible));
		ser[i].data = luo_file->private_data;
		ser[i].token = token;
		i++;
	}

	session->files = __pa(ser);

	return 0;

exit_cleanup:
	__luo_file_cancel(session);
	luo_contig_free_unpreserve(ser, ser_size);

	return ret;
}

int luo_file_freeze(struct luo_session *session)
{
	struct luo_file *luo_file;
	struct luo_file_ser *ser;
	unsigned long token;
	size_t ser_size;
	int ret = 0;
	int i;

	if (!session->count)
		return 0;

	if (WARN_ON(!session->files))
		return -EINVAL;

	ser = __va(session->files);

	i = 0;
	xa_for_each(&session->files_xa, token, luo_file) {
		ret = luo_file_freeze_one(luo_file);
		if (ret < 0) {
			pr_err("Freeze failed for session[%s] token[%#0llx] handler[%s] ret[%d]\n",
			       session->name, (u64)token, luo_file->fh->compatible, ret);
			goto exit_cleanup;
		}
		ser[i].data = luo_file->private_data;
		i++;
	}

	return 0;

exit_cleanup:
	__luo_file_cancel(session);
	ser_size = session->count * sizeof(struct luo_file_ser);
	luo_contig_free_unpreserve(ser, ser_size);

	return ret;
}

void luo_file_finish(struct luo_session *session)
{
	struct luo_file *luo_file;
	struct luo_file_ser *ser;
	unsigned long token;
	size_t ser_size;

	if (!session->count)
		return;

	xa_for_each(&session->files_xa, token, luo_file)
		luo_file_finish_one(luo_file);

	ser_size = session->count * sizeof(struct luo_file_ser);
	ser = __va(session->files);
	luo_contig_free_restore(ser, ser_size);
}

void luo_file_cancel(struct luo_session *session)
{
	struct luo_file_ser *ser;
	size_t ser_size;

	if (!session->count)
		return;

	__luo_file_cancel(session);

	if (session->files) {
		ser = __va(session->files);
		ser_size = session->count * sizeof(struct luo_file_ser);
		luo_contig_free_unpreserve(ser, ser_size);
		session->files = 0;
	}
}

void luo_file_deserialize(struct luo_session *session)
{
	struct luo_file_ser *ser;
	u64 i;

	if (!session->files)
		return;

	guard(rwsem_read)(&luo_file_handler_list_rwsem);
	ser = __va(session->files);
	for (i = 0; i < session->count; i++) {
		struct liveupdate_file_handler *fh;
		bool handler_found = false;
		struct luo_file *luo_file;
		int ret;

		if (xa_load(&session->files_xa, ser[i].token)) {
			luo_restore_fail("Duplicate token %llu found in incoming FDT for file descriptors.\n",
					 ser[i].token);
		}

		list_for_each_entry(fh, &luo_file_handler_list, list) {
			if (!strcmp(fh->compatible, ser[i].compatible)) {
				handler_found = true;
				break;
			}
		}

		if (!handler_found) {
			luo_restore_fail("No registered handler for compatible '%s'\n",
					 ser[i].compatible);
		}

		luo_file = kzalloc(sizeof(*luo_file),
				   GFP_KERNEL | __GFP_NOFAIL);
		luo_file->fh = fh;
		luo_file->file = NULL;
		luo_file->private_data = ser[i].data;
		luo_file->reclaimed = false;
		mutex_init(&luo_file->mutex);
		luo_file->state = LIVEUPDATE_STATE_UPDATED;
		ret = xa_err(xa_store(&session->files_xa, ser[i].token,
				      luo_file, GFP_KERNEL | __GFP_NOFAIL));
		if (ret < 0) {
			luo_restore_fail("Failed to store luo_file for token %llu in XArray: %d\n",
					 ser[i].token, ret);
		}
	}
}

/**
 * luo_preserve_file - Register a file descriptor for live update management.
 * @token: Token value for this file descriptor.
 * @fd: file descriptor to be preserved.
 *
 * Context: Must be called when LUO is in 'normal' state.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int luo_preserve_file(struct luo_session *session, u64 token, int fd)
{
	struct liveupdate_file_handler *fh;
	struct luo_file *luo_file;
	bool found = false;
	int ret = -ENOENT;
	struct file *file;

	file = fget(fd);
	if (!file) {
		pr_err("Bad file descriptor\n");
		return -EBADF;
	}

	guard(rwsem_read)(&luo_file_handler_list_rwsem);
	list_for_each_entry(fh, &luo_file_handler_list, list) {
		if (fh->ops->can_preserve(fh, file)) {
			found = true;
			break;
		}
	}

	if (!found)
		goto exit_cleanup;

	luo_file = kzalloc(sizeof(*luo_file), GFP_KERNEL);
	if (!luo_file) {
		ret = -ENOMEM;
		goto exit_cleanup;
	}

	luo_file->private_data = 0;
	luo_file->reclaimed = false;

	luo_file->file = file;
	luo_file->fh = fh;
	mutex_init(&luo_file->mutex);
	luo_file->state = LIVEUPDATE_STATE_NORMAL;

	if (xa_load(&session->files_xa, token)) {
		ret = -EEXIST;
		pr_warn("Token %llu is already taken\n", token);
		mutex_destroy(&luo_file->mutex);
		kfree(luo_file);
		goto exit_cleanup;
	}

	ret = xa_err(xa_store(&session->files_xa, token, luo_file,
			      GFP_KERNEL));
	if (ret < 0) {
		pr_warn("Failed to store file for token %llu in XArray: %d\n",
			token, ret);
		mutex_destroy(&luo_file->mutex);
		kfree(luo_file);
		goto exit_cleanup;
	}
	atomic_inc(&luo_file->fh->count);
	session->count++;

exit_cleanup:
	if (ret)
		fput(file);

	return ret;
}

/**
 * luo_unpreserve_file - Unregister a file instance using its token.
 * @token: The unique token of the file instance to unregister.
 *
 * Finds the &struct luo_file associated with the @token in the
 * global list and removes it. This function *only* removes the entry from the
 * list; it does *not* free the memory allocated for the &struct luo_file
 * itself. The caller is responsible for freeing the structure after this
 * function returns successfully.
 *
 * Context: Can be called when a preserved file descriptor is closed or
 * no longer needs live update management.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int luo_unpreserve_file(struct luo_session *session, u64 token)
{
	struct luo_file *luo_file;

	luo_file = xa_erase(&session->files_xa, token);
	if (!luo_file)
		return -ENOENT;

	if (luo_file->file)
		fput(luo_file->file);
	mutex_destroy(&luo_file->mutex);
	scoped_guard(rwsem_read, &luo_file_handler_list_rwsem)
		atomic_dec(&luo_file->fh->count);
	kfree(luo_file);
	session->count--;

	return 0;
}

/**
 * luo_retrieve_file - Find a registered file instance by its token.
 * @token: The unique token of the file instance to retrieve.
 * @filep: Output parameter. On success (return value 0), this will point
 * to the retrieved "struct file".
 *
 * Searches the global list for a &struct luo_file matching the @token. Uses a
 * read lock, allowing concurrent retrievals.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int luo_retrieve_file(struct luo_session *session, u64 token,
		      struct file **filep)
{
	struct luo_file *luo_file;
	int ret = 0;

	luo_file = xa_load(&session->files_xa, token);
	if (!luo_file)
		return -ENOENT;

	if (luo_file->reclaimed)
		return -EADDRINUSE;

	guard(mutex)(&luo_file->mutex);
	if (luo_file->reclaimed)
		return -EADDRINUSE;

	ret = luo_file->fh->ops->retrieve(luo_file->fh, luo_file->private_data,
					  filep);
	if (!ret) {
		/* Get a reference so, we can keep this file in LUO */
		luo_file->file = *filep;
		get_file(luo_file->file);
		luo_file->reclaimed = true;
	}

	return ret;
}

void luo_file_unpreserve_all_files(struct luo_session *session)
{
	unsigned long token;
	struct luo_file *h;

	xa_for_each(&session->files_xa, token, h)
		luo_unpreserve_file(session, token);
}

void luo_file_unpreserve_unreclaimed_files(struct luo_session *session)
{
	unsigned long token;
	struct luo_file *h;

	xa_for_each(&session->files_xa, token, h) {
		if (!h->reclaimed) {
			pr_err("Unpreserving unreclaimed file, session[%s] token[%#0llx] handler[%s]\n",
			       session->name, (u64)token, h->fh->compatible);
			luo_unpreserve_file(session, token);
		}
	}
}

/**
 * liveupdate_register_file_handler - Register a file handler with LUO.
 * @fh: Pointer to a caller-allocated &struct liveupdate_file_handler.
 * The caller must initialize this structure, including a unique
 * 'compatible' string and a valid 'fh' callbacks. This function adds the
 * handler to the global list of supported file handlers.
 *
 * Context: Typically called during module initialization for file types that
 * support live update preservation.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int liveupdate_register_file_handler(struct liveupdate_file_handler *fh)
{
	struct liveupdate_file_handler *fh_iter;

	guard(rwsem_read)(&luo_state_rwsem);
	if (!liveupdate_state_normal() && !liveupdate_state_updated())
		return -EBUSY;

	guard(rwsem_write)(&luo_file_handler_list_rwsem);
	list_for_each_entry(fh_iter, &luo_file_handler_list, list) {
		if (!strcmp(fh_iter->compatible, fh->compatible)) {
			pr_err("File handler registration failed: Compatible string '%s' already registered.\n",
			       fh->compatible);
			return -EEXIST;
		}
	}

	if (!try_module_get(fh->ops->owner))
		return -EAGAIN;

	INIT_LIST_HEAD(&fh->list);
	atomic_set(&fh->count, 0);
	list_add_tail(&fh->list, &luo_file_handler_list);

	return 0;
}

/**
 * liveupdate_unregister_file - Unregister a file handler.
 * @fh: Pointer to the specific &struct liveupdate_file_handler instance
 * that was previously returned by or passed to
 * liveupdate_register_file_handler.
 *
 * Removes the specified handler instance @fh from the global list of
 * registered file handlers. This function only removes the entry from the
 * list; it does not free the memory associated with @fh itself. The caller
 * is responsible for freeing the structure memory after this function returns
 * successfully.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int liveupdate_unregister_file_handler(struct liveupdate_file_handler *fh)
{
	guard(rwsem_read)(&luo_state_rwsem);
	if (!liveupdate_state_normal() && !liveupdate_state_updated())
		return -EBUSY;

	guard(rwsem_write)(&luo_file_handler_list_rwsem);
	if (atomic_read(&fh->count)) {
		pr_warn("Unable to unregister file handler, files are preserved\n");
		return -EBUSY;
	}

	list_del_init(&fh->list);
	module_put(fh->ops->owner);

	return 0;
}
