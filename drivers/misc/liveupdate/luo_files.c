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
 * types (e.g., memfd, vfio) register a &struct liveupdate_filesystem
 * handler. This handler contains callbacks (&liveupdate_filesystem.prepare,
 * &liveupdate_filesystem.freeze, &liveupdate_filesystem.finish, etc.)
 * and a unique 'compatible' string identifying the file type.
 * Registration occurs via liveupdate_register_filesystem().
 *
 * 2. File Instance Tracking: When a potentially preservable file needs to be
 * managed for live update, the core LUO logic (luo_register_file()) finds a
 * compatible registered handler using its &liveupdate_filesystem.can_preserve
 * callback. If found,  an internal &struct luo_file instance is created,
 * assigned a unique u64 'token', and added to a list.
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
 * handler's &liveupdate_filesystem.retrieve callback, passing the persisted
 * u64 data, to recreate or find the appropriate &struct file object.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/libfdt.h>
#include <linux/liveupdate.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xarray.h>
#include "luo_internal.h"

#define LUO_FILES_NODE_NAME	"file-descriptors"
#define LUO_FILES_COMPATIBLE	"file-descriptors-v1"

static DEFINE_XARRAY(luo_files_xa_in);
static DEFINE_XARRAY(luo_files_xa_out);
static bool luo_files_xa_in_recreated;

/* Regestred filesystems. */
static DECLARE_RWSEM(luo_filesystems_list_rwsem);
static LIST_HEAD(luo_filesystems_list);

static void *luo_fdt_out;
static void *luo_fdt_in;

static u64 luo_next_file_token;

/**
 * struct luo_file - Represents a file descriptor instance preserved
 * across live update.
 * @fs:            Pointer to the &struct liveupdate_filesystems containing
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
 * @mutex:          Lock to protect FD state, and allow independently to change
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
	struct liveupdate_filesystem *fs;
	struct file *file;
	u64 private_data;
	bool reclaimed;
	enum liveupdate_state state;
	struct mutex mutex;
};

/**
 * luo_files_startup - Validates the LUO file-descriptors FDT node at startup.
 * @fdt: Pointer to the LUO FDT blob passed from the previous kernel.
 *
 * This __init function checks the existence and validity of the
 * '/file-descriptors' node in the FDT. This node is considered mandatory. It
 * calls panic() if the node is missing, inaccessible, or invalid (e.g., missing
 * compatible, wrong compatible string), indicating a critical configuration
 * error for LUO.
 */
void __init luo_files_startup(void *fdt)
{
	int ret, node_offset;

	node_offset = fdt_subnode_offset(fdt, 0, LUO_FILES_NODE_NAME);
	if (node_offset < 0)
		panic("Failed to find /file-descriptors node\n");

	ret = fdt_node_check_compatible(fdt, node_offset,
					LUO_FILES_COMPATIBLE);
	if (ret) {
		panic("FDT '%s' is incompatible with '%s' [%d]\n",
		      LUO_FILES_NODE_NAME, LUO_FILES_COMPATIBLE, ret);
	}
	luo_fdt_in = fdt;
}

static void luo_files_recreate_luo_files_xa_in(void)
{
	int parent_node_offset, file_node_offset;
	const char *node_name, *fdt_compat_str;
	struct liveupdate_filesystem *fs;
	struct luo_file *luo_file;
	const void *data_ptr;
	int ret = 0;

	if (luo_files_xa_in_recreated || !luo_fdt_in)
		return;

	/* Take write in order to gurantee that we re-create list once */
	down_write(&luo_filesystems_list_rwsem);
	if (luo_files_xa_in_recreated)
		goto exit_unlock;

	parent_node_offset = fdt_subnode_offset(luo_fdt_in, 0,
						LUO_FILES_NODE_NAME);

	fdt_for_each_subnode(file_node_offset, luo_fdt_in, parent_node_offset) {
		bool handler_found = false;
		u64 token;

		node_name = fdt_get_name(luo_fdt_in, file_node_offset, NULL);
		if (!node_name) {
			panic("Skipping FDT subnode at offset %d: Cannot get name\n",
			      file_node_offset);
		}

		ret = kstrtou64(node_name, 0, &token);
		if (ret < 0) {
			panic("Skipping FDT node '%s': Failed to parse token\n",
			      node_name);
		}

		fdt_compat_str = fdt_getprop(luo_fdt_in, file_node_offset,
					     "compatible", NULL);
		if (!fdt_compat_str) {
			panic("Skipping FDT node '%s': Missing 'compatible' property\n",
			      node_name);
		}

		data_ptr = fdt_getprop(luo_fdt_in, file_node_offset, "data",
				       NULL);
		if (!data_ptr) {
			panic("Can't recover property 'data' for FDT node '%s'\n",
			      node_name);
		}

		list_for_each_entry(fs, &luo_filesystems_list, list) {
			if (!strcmp(fs->compatible, fdt_compat_str)) {
				handler_found = true;
				break;
			}
		}

		if (!handler_found) {
			panic("Skipping FDT node '%s': No registered handler for compatible '%s'\n",
			      node_name, fdt_compat_str);
		}

		luo_file = kmalloc(sizeof(*luo_file),
				   GFP_KERNEL | __GFP_NOFAIL);
		luo_file->fs = fs;
		luo_file->file = NULL;
		memcpy(&luo_file->private_data, data_ptr, sizeof(u64));
		luo_file->reclaimed = false;
		mutex_init(&luo_file->mutex);
		luo_file->state = LIVEUPDATE_STATE_UPDATED;
		ret = xa_err(xa_store(&luo_files_xa_in, token, luo_file,
				      GFP_KERNEL | __GFP_NOFAIL));
		if (ret < 0) {
			panic("Failed to store luo_file for token %llu in XArray: %d\n",
			      token, ret);
		}
	}
	luo_files_xa_in_recreated = true;

exit_unlock:
	up_write(&luo_filesystems_list_rwsem);
}

/**
 * luo_files_fdt_setup - Adds and populates the 'file-descriptors' node in the
 * FDT.
 * @fdt: Pointer to the LUO FDT blob.
 *
 * Add file-descriptors node and each FD node to the LUO FDT blob.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int luo_files_fdt_setup(void *fdt)
{
	int ret, files_node_offset, node_offset;
	const u64 zero_data = 0;
	unsigned long token;
	struct luo_file *h;
	char token_str[19];

	ret = fdt_add_subnode(fdt, 0, LUO_FILES_NODE_NAME);
	if (ret < 0)
		goto exit_error;

	files_node_offset = ret;
	ret = fdt_setprop_string(fdt, files_node_offset, "compatible",
				 LUO_FILES_COMPATIBLE);
	if (ret < 0)
		goto exit_error;

	xa_for_each(&luo_files_xa_out, token, h) {
		snprintf(token_str, sizeof(token_str), "%#0llx", (u64)token);

		ret = fdt_add_subnode(fdt, files_node_offset, token_str);
		if (ret < 0)
			goto exit_error;

		node_offset = ret;
		ret = fdt_setprop_string(fdt, node_offset, "compatible",
					 h->fs->compatible);
		if (ret < 0)
			goto exit_error;

		ret = fdt_setprop(fdt, node_offset, "data",
				  &zero_data, sizeof(zero_data));
	}

	luo_fdt_out = fdt;

	return 0;
exit_error:
	pr_err("Failed to setup 'file-descriptors' node to FDT: %s\n",
	       fdt_strerror(ret));
	return -ENOSPC;
}

/**
 * luo_do_files_prepare_calls - Calls prepare callbacks and updates FDT
 * if all prepares succeed. Handles cancellation on failure.
 *
 * Phase 1: Calls 'prepare' for all files and stores results temporarily.
 * If any 'prepare' fails, calls 'cancel' on previously prepared files
 * and returns the error.
 * Phase 2: If all 'prepare' calls succeeded, writes the stored data to the FDT.
 * If any FDT write fails, calls 'cancel' on *all* prepared files and
 * returns the FDT error.
 *
 * Returns: 0 on success. Negative errno on failure.
 */
int luo_do_files_prepare_calls(void)
{
	return 0;
}

/**
 * luo_do_files_freeze_calls - Calls freeze callbacks and updates FDT
 * if all calls succeed. Handles cancellation on failure.
 *
 * Phase 1: Calls 'freeze' for all files and stores results temporarily.
 * If any 'freeze' fails, calls 'cancel' on previously called files.
 * and returns the error.
 * Phase 2: If all 'freeze' calls succeeded, writes the stored data to the FDT.
 * If any FDT write fails, calls 'cancel' on *all* files and returns the FDT
 * error.
 *
 * Returns: 0 on success. Negative errno on failure.
 */
int luo_do_files_freeze_calls(void)
{
	return 0;
}

/**
 * luo_do_files_finish_calls - Calls finish callbacks for all file descriptors.
 *
 * This function is called at the end of live update cycle to do the final
 * clean-up or housekeeping of the post-live update states.
 */
void luo_do_files_finish_calls(void)
{
	luo_files_recreate_luo_files_xa_in();
}

/**
 * luo_do_files_cancel_calls - Calls cancel callbacks for all file descriptors.
 *
 * This function is typically called when the live update process needs to be
 * aborted externally, for example, after the prepare phase may have run but
 * before actual reboot. It iterates through all registered files and calls
 * the 'cancel' callback for those that implement it and likely completed
 * prepare.
 */
void luo_do_files_cancel_calls(void)
{
}

/**
 * luo_register_file - Register a file descriptor for live update management.
 * @tokenp: Return argument for the token value.
 * @file: Pointer to the struct file to be preserved.
 *
 * Context: Must be called when LUO is in 'normal' state.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int luo_register_file(u64 *tokenp, struct file *file)
{
	struct liveupdate_filesystem *fs;
	bool found = false;
	int ret = -ENOENT;
	u64 token;

	luo_state_read_enter();
	if (!liveupdate_state_normal() && !liveupdate_state_updated()) {
		pr_warn("File can be registered only in normal or prepared state\n");
		luo_state_read_exit();
		return -EBUSY;
	}

	down_read(&luo_filesystems_list_rwsem);
	list_for_each_entry(fs, &luo_filesystems_list, list) {
		if (fs->can_preserve(file, fs->arg)) {
			found = true;
			break;
		}
	}

	if (found) {
		struct luo_file *luo_file = kmalloc(sizeof(*luo_file),
						    GFP_KERNEL);

		if (!luo_file) {
			ret = -ENOMEM;
			goto exit_unlock;
		}

		token = luo_next_file_token;
		luo_next_file_token++;

		luo_file->private_data = 0;
		luo_file->reclaimed = false;

		luo_file->file = file;
		luo_file->fs = fs;
		mutex_init(&luo_file->mutex);
		luo_file->state = LIVEUPDATE_STATE_NORMAL;
		ret = xa_err(xa_store(&luo_files_xa_out, token, luo_file,
				      GFP_KERNEL));
		if (ret < 0) {
			pr_warn("Failed to store file for token %llu in XArray: %d\n",
				token, ret);
			kfree(luo_file);
			goto exit_unlock;
		}
		*tokenp = token;
	}

exit_unlock:
	up_read(&luo_filesystems_list_rwsem);
	luo_state_read_exit();

	return ret;
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
 * no longer needs live update management. Uses down_write_killable
 * for list modification.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int luo_unregister_file(u64 token)
{
	struct luo_file *luo_file;
	int ret = 0;

	luo_state_read_enter();
	if (!liveupdate_state_normal() && !liveupdate_state_updated()) {
		pr_warn("File can be unregistered only in normal or updates state\n");
		luo_state_read_exit();
		return -EBUSY;
	}

	luo_file = xa_erase(&luo_files_xa_out, token);
	if (luo_file) {
		kfree(luo_file);
	} else {
		pr_warn("Failed to unregister: token %llu not found.\n",
			token);
		ret = -ENOENT;
	}
	luo_state_read_exit();

	return ret;
}

/**
 * luo_retrieve_file - Find a registered file instance by its token.
 * @token: The unique token of the file instance to retrieve.
 * @file: Output parameter. On success (return value 0), this will point
 * to the retrieved "struct file".
 *
 * Searches the global list for a &struct luo_file matching the @token. Uses a
 * read lock, allowing concurrent retrievals.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int luo_retrieve_file(u64 token, struct file **file)
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
		luo_file->reclaimed = true;
		ret = luo_file->fs->retrieve(luo_file->fs->arg,
					     luo_file->private_data,
					     file);
		if (!ret)
			luo_file->file = *file;
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
 * liveupdate_register_filesystem - Register a filesystem handler with LUO.
 * @fs: Pointer to a caller-allocated &struct liveupdate_filesystem.
 * The caller must initialize this structure, including a unique
 * 'compatible' string and a valid 'fs' callbacks. This function adds the
 * handler to the global list of supported filesystem handlers.
 *
 * Context: Typically called during module initialization for filesystems or
 * file types that support live update preservation.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int liveupdate_register_filesystem(struct liveupdate_filesystem *fs)
{
	struct liveupdate_filesystem *fs_iter;
	int ret = 0;

	luo_state_read_enter();
	if (!liveupdate_state_normal() && !liveupdate_state_updated()) {
		luo_state_read_exit();
		return -EBUSY;
	}

	down_write(&luo_filesystems_list_rwsem);
	list_for_each_entry(fs_iter, &luo_filesystems_list, list) {
		if (!strcmp(fs_iter->compatible, fs->compatible)) {
			pr_err("Filesystem handler registration failed: Compatible string '%s' already registered.\n",
			       fs->compatible);
			ret = -EEXIST;
			goto exit_unlock;
		}
	}

	INIT_LIST_HEAD(&fs->list);
	list_add_tail(&fs->list, &luo_filesystems_list);

exit_unlock:
	up_write(&luo_filesystems_list_rwsem);
	luo_state_read_exit();

	return ret;
}
EXPORT_SYMBOL_GPL(liveupdate_register_filesystem);

/**
 * liveupdate_unregister_filesystem - Unregister a filesystem handler.
 * @fs: Pointer to the specific &struct liveupdate_filesystem instance
 * that was previously returned by or passed to liveupdate_register_filesystem.
 *
 * Removes the specified handler instance @fs from the global list of
 * registered filesystem handlers. This function only removes the entry from the
 * list; it does not free the memory associated with @fs itself. The caller
 * is responsible for freeing the structure memory after this function returns
 * successfully.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int liveupdate_unregister_filesystem(struct liveupdate_filesystem *fs)
{
	int ret = 0;

	luo_state_read_enter();
	if (!liveupdate_state_normal() && !liveupdate_state_updated()) {
		luo_state_read_exit();
		return -EBUSY;
	}

	down_write(&luo_filesystems_list_rwsem);
	list_del_init(&fs->list);
	up_write(&luo_filesystems_list_rwsem);
	luo_state_read_exit();

	return ret;
}
EXPORT_SYMBOL_GPL(liveupdate_unregister_filesystem);
