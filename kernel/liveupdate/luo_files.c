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
 * managed for live update, the core LUO logic (luo_register_file()) finds a
 * compatible registered handler using its
 * &liveupdate_file_handler.ops->can_preserve callback. If found,  an internal
 * &struct luo_file instance is created, assigned a unique u64 'token', and
 * added to a list.
 *
 * 3. State Persistence (FDT): During the LUO prepare/freeze phases, the
 * registered handler callbacks are invoked for each tracked file instance.
 * These callbacks can generate a u64 data payload representing the minimal
 * state needed for restoration. This payload, along with the handler's
 * compatible string and the unique token, is stored in a dedicated
 * '/file-descriptors' node within the main LUO FDT blob passed via
 * Kexec Handover (KHO).
 *
 * 4. Restoration: In the new kernel, the LUO framework parses the incoming
 * FDT to reconstruct the list of &struct luo_file instances. When the
 * original owner requests the file, luo_retrieve_file() uses the corresponding
 * handler's &liveupdate_file_handler.ops->retrieve callback, passing the
 * persisted u64 data, to recreate or find the appropriate &struct file object.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/kexec_handover.h>
#include <linux/libfdt.h>
#include <linux/liveupdate.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xarray.h>
#include "luo_internal.h"

#define LUO_FILES_NODE_NAME	"file-descriptors"
#define LUO_FILES_COMPATIBLE	"file-descriptors-v1"

static DEFINE_XARRAY(luo_files_xa_in);
static DEFINE_XARRAY(luo_files_xa_out);
static bool luo_files_xa_in_recreated;

/* Registered files. */
static DECLARE_RWSEM(luo_register_file_list_rwsem);
static LIST_HEAD(luo_register_file_list);

static DECLARE_RWSEM(luo_file_fdt_rwsem);
static void *luo_file_fdt_out;
static void *luo_file_fdt_in;

static size_t luo_file_fdt_out_size;

static atomic64_t luo_files_count;

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

static void luo_files_recreate_luo_files_xa_in(void)
{
	const char *node_name, *fdt_compat_str;
	struct liveupdate_file_handler *fh;
	struct luo_file *luo_file;
	const void *data_ptr;
	int file_node_offset;
	int ret = 0;

	guard(rwsem_read)(&luo_file_fdt_rwsem);
	if (luo_files_xa_in_recreated || !luo_file_fdt_in)
		return;

	/* Take write in order to guarantee that we re-create list once */
	guard(rwsem_write)(&luo_register_file_list_rwsem);
	if (luo_files_xa_in_recreated)
		return;

	fdt_for_each_subnode(file_node_offset, luo_file_fdt_in, 0) {
		bool handler_found = false;
		u64 token;

		node_name = fdt_get_name(luo_file_fdt_in, file_node_offset,
					 NULL);
		if (!node_name) {
			luo_restore_fail("FDT subnode at offset %d: Cannot get name\n",
					 file_node_offset);
		}

		ret = kstrtou64(node_name, 0, &token);
		if (ret < 0) {
			luo_restore_fail("FDT node '%s': Failed to parse token\n",
					 node_name);
		}

		if (xa_load(&luo_files_xa_in, token)) {
			luo_restore_fail("Duplicate token %llu found in incoming FDT for file descriptors.\n",
					 token);
		}

		fdt_compat_str = fdt_getprop(luo_file_fdt_in, file_node_offset,
					     "compatible", NULL);
		if (!fdt_compat_str) {
			luo_restore_fail("FDT node '%s': Missing 'compatible' property\n",
					 node_name);
		}

		data_ptr = fdt_getprop(luo_file_fdt_in, file_node_offset, "data",
				       NULL);
		if (!data_ptr) {
			luo_restore_fail("Can't recover property 'data' for FDT node '%s'\n",
					 node_name);
		}

		list_for_each_entry(fh, &luo_register_file_list, list) {
			if (!strcmp(fh->compatible, fdt_compat_str)) {
				handler_found = true;
				break;
			}
		}

		if (!handler_found) {
			luo_restore_fail("FDT node '%s': No registered handler for compatible '%s'\n",
					 node_name, fdt_compat_str);
		}

		luo_file = kmalloc(sizeof(*luo_file),
				   GFP_KERNEL | __GFP_NOFAIL);
		luo_file->fh = fh;
		luo_file->file = NULL;
		memcpy(&luo_file->private_data, data_ptr, sizeof(u64));
		luo_file->reclaimed = false;
		mutex_init(&luo_file->mutex);
		luo_file->state = LIVEUPDATE_STATE_UPDATED;
		ret = xa_err(xa_store(&luo_files_xa_in, token, luo_file,
				      GFP_KERNEL | __GFP_NOFAIL));
		if (ret < 0) {
			luo_restore_fail("Failed to store luo_file for token %llu in XArray: %d\n",
					 token, ret);
		}
	}
	luo_files_xa_in_recreated = true;
}

static size_t luo_files_fdt_size(void)
{
	u64 num_files = atomic64_read(&luo_files_count);

	/* Estimate a 1K overhead, + 128 bytes per file entry */
	return PAGE_SIZE << get_order(SZ_1K + (num_files * 128));
}

static void luo_files_fdt_cleanup(void)
{
	WARN_ON_ONCE(kho_unpreserve_phys(__pa(luo_file_fdt_out),
					 luo_file_fdt_out_size));

	free_pages((unsigned long)luo_file_fdt_out,
		   get_order(luo_file_fdt_out_size));

	luo_file_fdt_out_size = 0;
	luo_file_fdt_out = NULL;
}

static int luo_files_to_fdt(struct xarray *files_xa_out)
{
	const u64 zero_data = 0;
	unsigned long token;
	struct luo_file *h;
	char token_str[19];
	int ret = 0;

	xa_for_each(files_xa_out, token, h) {
		snprintf(token_str, sizeof(token_str), "%#0llx", (u64)token);

		ret = fdt_begin_node(luo_file_fdt_out, token_str);
		if (ret < 0)
			break;

		ret = fdt_property_string(luo_file_fdt_out, "compatible",
					  h->fh->compatible);
		if (ret < 0) {
			fdt_end_node(luo_file_fdt_out);
			break;
		}

		ret = fdt_property_u64(luo_file_fdt_out, "data", zero_data);
		if (ret < 0) {
			fdt_end_node(luo_file_fdt_out);
			break;
		}

		ret = fdt_end_node(luo_file_fdt_out);
		if (ret < 0)
			break;
	}

	return ret;
}

static int luo_files_fdt_setup(void)
{
	int ret;

	guard(rwsem_write)(&luo_file_fdt_rwsem);
	luo_file_fdt_out_size = luo_files_fdt_size();
	luo_file_fdt_out = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
						    get_order(luo_file_fdt_out_size));
	if (!luo_file_fdt_out) {
		pr_err("Failed to allocate FDT memory (%zu bytes)\n",
		       luo_file_fdt_out_size);
		luo_file_fdt_out_size = 0;
		return -ENOMEM;
	}

	ret = kho_preserve_phys(__pa(luo_file_fdt_out), luo_file_fdt_out_size);
	if (ret) {
		pr_err("Failed to kho preserve FDT memory (%zu bytes)\n",
		       luo_file_fdt_out_size);
		luo_file_fdt_out_size = 0;
		luo_file_fdt_out = NULL;
		return ret;
	}

	ret = fdt_create(luo_file_fdt_out, luo_file_fdt_out_size);
	if (ret < 0)
		goto exit_cleanup;

	ret = fdt_finish_reservemap(luo_file_fdt_out);
	if (ret < 0)
		goto exit_finish;

	ret = fdt_begin_node(luo_file_fdt_out, LUO_FILES_NODE_NAME);
	if (ret < 0)
		goto exit_finish;

	ret = fdt_property_string(luo_file_fdt_out, "compatible",
				  LUO_FILES_COMPATIBLE);
	if (ret < 0)
		goto exit_end_node;

	ret = luo_files_to_fdt(&luo_files_xa_out);
	if (ret < 0)
		goto exit_end_node;

	ret = fdt_end_node(luo_file_fdt_out);
	if (ret < 0)
		goto exit_finish;

	ret = fdt_finish(luo_file_fdt_out);
	if (ret < 0)
		goto exit_cleanup;

	return 0;

exit_end_node:
	fdt_end_node(luo_file_fdt_out);
exit_finish:
	fdt_finish(luo_file_fdt_out);
exit_cleanup:
	pr_err("Failed to setup FDT: %s (ret %d)\n", fdt_strerror(ret), ret);
	luo_files_fdt_cleanup();

	return ret;
}

static int luo_files_prepare_one(struct luo_file *h)
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

static int luo_files_freeze_one(struct luo_file *h)
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

static void luo_files_finish_one(struct luo_file *h)
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

static void luo_files_cancel_one(struct luo_file *h)
{
	int ret;

	guard(mutex)(&h->mutex);
	if (h->state == LIVEUPDATE_STATE_NORMAL)
		return;

	ret = WARN_ON_ONCE(h->state != LIVEUPDATE_STATE_PREPARED &&
			   h->state != LIVEUPDATE_STATE_FROZEN);
	if (ret)
		return;

	if (h->fh->ops->cancel)
		h->fh->ops->cancel(h->fh, h->file, h->private_data);
	h->private_data = 0;
	h->state = LIVEUPDATE_STATE_NORMAL;
}

static void __luo_files_cancel(struct luo_file *boundary_file)
{
	unsigned long token;
	struct luo_file *h;

	xa_for_each(&luo_files_xa_out, token, h) {
		if (h == boundary_file)
			break;

		luo_files_cancel_one(h);
	}
	luo_files_fdt_cleanup();
}

static int luo_files_commit_data_to_fdt(void)
{
	int node_offset, ret;
	unsigned long token;
	char token_str[19];
	struct luo_file *h;

	guard(rwsem_read)(&luo_file_fdt_rwsem);
	xa_for_each(&luo_files_xa_out, token, h) {
		snprintf(token_str, sizeof(token_str), "%#0llx", (u64)token);
		node_offset = fdt_subnode_offset(luo_file_fdt_out,
						 0,
						 token_str);
		ret = fdt_setprop(luo_file_fdt_out, node_offset, "data",
				  &h->private_data, sizeof(h->private_data));
		if (ret < 0) {
			pr_err("Failed to set data property for token %s: %s\n",
			       token_str, fdt_strerror(ret));
			return -ENOSPC;
		}
	}

	return 0;
}

static int luo_files_prepare(struct liveupdate_subsystem *h, u64 *data)
{
	unsigned long token;
	struct luo_file *luo_file;
	int ret;

	ret = luo_files_fdt_setup();
	if (ret)
		return ret;

	xa_for_each(&luo_files_xa_out, token, luo_file) {
		ret = luo_files_prepare_one(luo_file);
		if (ret < 0) {
			pr_err("Prepare failed for file token %#0llx handler '%s' [%d]\n",
			       (u64)token, luo_file->fh->compatible, ret);
			__luo_files_cancel(luo_file);

			return ret;
		}
	}

	ret = luo_files_commit_data_to_fdt();
	if (ret) {
		__luo_files_cancel(NULL);
	} else {
		scoped_guard(rwsem_read, &luo_file_fdt_rwsem)
			*data = __pa(luo_file_fdt_out);
	}

	return ret;
}

static int luo_files_freeze(struct liveupdate_subsystem *h, u64 *data)
{
	unsigned long token;
	struct luo_file *luo_file;
	int ret;

	xa_for_each(&luo_files_xa_out, token, luo_file) {
		ret = luo_files_freeze_one(luo_file);
		if (ret < 0) {
			pr_err("Freeze callback failed for file token %#0llx handler '%s' [%d]\n",
			       (u64)token, luo_file->fh->compatible, ret);
			__luo_files_cancel(luo_file);

			return ret;
		}
	}

	ret = luo_files_commit_data_to_fdt();
	if (ret)
		__luo_files_cancel(NULL);

	return ret;
}

static void luo_files_finish(struct liveupdate_subsystem *h, u64 data)
{
	unsigned long token;
	struct luo_file *luo_file;

	luo_files_recreate_luo_files_xa_in();
	xa_for_each(&luo_files_xa_in, token, luo_file) {
		luo_files_finish_one(luo_file);
		mutex_destroy(&luo_file->mutex);
		kfree(luo_file);
	}
	xa_destroy(&luo_files_xa_in);
}

static void luo_files_cancel(struct liveupdate_subsystem *h, u64 data)
{
	__luo_files_cancel(NULL);
}

static void luo_files_boot(struct liveupdate_subsystem *h, u64 fdt_pa)
{
	int ret;

	ret = fdt_node_check_compatible(__va(fdt_pa), 0,
					LUO_FILES_COMPATIBLE);
	if (ret) {
		luo_restore_fail("FDT '%s' is incompatible with '%s' [%d]\n",
				 LUO_FILES_NODE_NAME, LUO_FILES_COMPATIBLE,
				 ret);
	}
	scoped_guard(rwsem_write, &luo_file_fdt_rwsem)
		luo_file_fdt_in = __va(fdt_pa);
}

static const struct liveupdate_subsystem_ops luo_file_subsys_ops = {
	.prepare = luo_files_prepare,
	.freeze = luo_files_freeze,
	.cancel = luo_files_cancel,
	.boot = luo_files_boot,
	.finish = luo_files_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_subsystem luo_file_subsys = {
	.ops = &luo_file_subsys_ops,
	.name = LUO_FILES_NODE_NAME,
};

static int __init luo_files_startup(void)
{
	int ret;

	if (!liveupdate_enabled())
		return 0;

	ret = liveupdate_register_subsystem(&luo_file_subsys);
	if (ret) {
		pr_warn("Failed to register luo_file subsystem [%d]\n", ret);
		return ret;
	}

	return ret;
}
late_initcall(luo_files_startup);

/**
 * luo_register_file - Register a file descriptor for live update management.
 * @token: Token value for this file descriptor.
 * @fd: file descriptor to be preserved.
 *
 * Context: Must be called when LUO is in 'normal' state.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int luo_register_file(u64 token, int fd)
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

	luo_state_read_enter();
	if (!liveupdate_state_normal() && !liveupdate_state_updated()) {
		pr_warn("File can be registered only in normal or updated state\n");
		luo_state_read_exit();
		fput(file);
		return -EBUSY;
	}

	guard(rwsem_read)(&luo_register_file_list_rwsem);
	list_for_each_entry(fh, &luo_register_file_list, list) {
		if (fh->ops->can_preserve(fh, file)) {
			found = true;
			break;
		}
	}

	if (!found)
		goto exit_unlock;

	luo_file = kmalloc(sizeof(*luo_file), GFP_KERNEL);
	if (!luo_file) {
		ret = -ENOMEM;
		goto exit_unlock;
	}

	luo_file->private_data = 0;
	luo_file->reclaimed = false;

	luo_file->file = file;
	luo_file->fh = fh;
	mutex_init(&luo_file->mutex);
	luo_file->state = LIVEUPDATE_STATE_NORMAL;

	if (xa_load(&luo_files_xa_out, token)) {
		ret = -EEXIST;
		pr_warn("Token %llu is already taken\n", token);
		mutex_destroy(&luo_file->mutex);
		kfree(luo_file);
		goto exit_unlock;
	}

	ret = xa_err(xa_store(&luo_files_xa_out, token, luo_file,
			      GFP_KERNEL));
	if (ret < 0) {
		pr_warn("Failed to store file for token %llu in XArray: %d\n",
			token, ret);
		mutex_destroy(&luo_file->mutex);
		kfree(luo_file);
		goto exit_unlock;
	}
	atomic64_inc(&luo_files_count);

exit_unlock:
	luo_state_read_exit();

	if (ret)
		fput(file);

	return ret;
}

static void luo_files_fdt_remove_node(u64 token)
{
	char token_str[19];
	int offset, ret;

	guard(rwsem_write)(&luo_file_fdt_rwsem);
	if (!luo_file_fdt_out)
		return;

	snprintf(token_str, sizeof(token_str), "%#0llx", token);
	offset = fdt_subnode_offset(luo_file_fdt_out, 0, token_str);
	if (offset < 0)
		return;

	ret = fdt_del_node(luo_file_fdt_out, offset);
	if (ret < 0) {
		pr_warn("LUO Files: Failed to delete FDT node for token %s: %s\n",
			token_str, fdt_strerror(ret));
	}
}

static int __luo_unregister_file(u64 token)
{
	struct luo_file *luo_file;

	luo_file = xa_erase(&luo_files_xa_out, token);
	if (!luo_file)
		return -ENOENT;

	if (luo_file->state == LIVEUPDATE_STATE_FROZEN ||
	    luo_file->state == LIVEUPDATE_STATE_PREPARED) {
		luo_files_cancel_one(luo_file);
		luo_files_fdt_remove_node(token);
	}

	fput(luo_file->file);
	mutex_destroy(&luo_file->mutex);
	kfree(luo_file);
	atomic64_dec(&luo_files_count);

	return 0;
}

/**
 * luo_unregister_file - Unregister a file instance using its token.
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
int luo_unregister_file(u64 token)
{
	int ret = 0;

	luo_state_read_enter();
	if (!liveupdate_state_normal() && !liveupdate_state_updated()) {
		pr_warn("File can be unregistered only in normal or updates state\n");
		luo_state_read_exit();
		return -EBUSY;
	}

	ret = __luo_unregister_file(token);
	if (ret) {
		pr_warn("Failed to unregister: token %llu not found.\n",
			token);
	}
	luo_state_read_exit();

	return ret;
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
int luo_retrieve_file(u64 token, struct file **filep)
{
	struct luo_file *luo_file;
	int ret = 0;

	luo_files_recreate_luo_files_xa_in();
	luo_state_read_enter();
	if (!liveupdate_state_updated()) {
		pr_warn("File can be retrieved only in updated state\n");
		luo_state_read_exit();
		return -EBUSY;
	}

	luo_file = xa_load(&luo_files_xa_in, token);
	if (luo_file && !luo_file->reclaimed) {
		scoped_guard(mutex, &luo_file->mutex) {
			if (!luo_file->reclaimed) {
				luo_file->reclaimed = true;
				ret = luo_file->fh->ops->retrieve(luo_file->fh,
								  luo_file->private_data,
								  filep);
				if (!ret)
					luo_file->file = *filep;
			}
		}
	} else if (luo_file && luo_file->reclaimed) {
		pr_err("The file descriptor for token %lld has already been retrieved\n",
		       token);
		ret = -EINVAL;
	} else {
		ret = -ENOENT;
	}

	luo_state_read_exit();

	return ret;
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
	int ret = 0;

	luo_state_read_enter();
	if (!liveupdate_state_normal() && !liveupdate_state_updated()) {
		luo_state_read_exit();
		return -EBUSY;
	}

	guard(rwsem_write)(&luo_register_file_list_rwsem);
	list_for_each_entry(fh_iter, &luo_register_file_list, list) {
		if (!strcmp(fh_iter->compatible, fh->compatible)) {
			pr_err("File handler registration failed: Compatible string '%s' already registered.\n",
			       fh->compatible);
			ret = -EEXIST;
			goto exit_unlock;
		}
	}

	if (!try_module_get(fh->ops->owner)) {
		pr_warn("File handler '%s' unable to get reference.\n",
			fh->compatible);
		ret = -EAGAIN;
		goto exit_unlock;
	}

	INIT_LIST_HEAD(&fh->list);
	list_add_tail(&fh->list, &luo_register_file_list);

exit_unlock:
	luo_state_read_exit();

	return ret;
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
	unsigned long token;
	struct luo_file *h;
	int ret = 0;

	luo_state_read_enter();
	if (!liveupdate_state_normal() && !liveupdate_state_updated()) {
		luo_state_read_exit();
		return -EBUSY;
	}

	guard(rwsem_write)(&luo_register_file_list_rwsem);

	xa_for_each(&luo_files_xa_out, token, h) {
		if (h->fh == fh) {
			luo_state_read_exit();
			return -EBUSY;
		}
	}

	list_del_init(&fh->list);
	luo_state_read_exit();
	module_put(fh->ops->owner);

	return ret;
}
