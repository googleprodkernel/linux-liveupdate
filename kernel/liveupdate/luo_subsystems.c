// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: LUO Subsystems support
 *
 * Various kernel subsystems register with the Live Update Orchestrator to
 * participate in the live update process. These subsystems are notified at
 * different stages of the live update sequence, allowing them to serialize
 * device state before the reboot and restore it afterwards. Examples include
 * the device layer, interrupt controllers, KVM, IOMMU, and specific device
 * drivers.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/libfdt.h>
#include <linux/liveupdate.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include "luo_internal.h"

#define LUO_SUBSYSTEMS_NODE_NAME	"subsystems"
#define LUO_SUBSYSTEMS_COMPATIBLE	"subsystems-v1"

static DEFINE_MUTEX(luo_subsystem_list_mutex);
static LIST_HEAD(luo_subsystems_list);
static void *luo_fdt_out;
static void *luo_fdt_in;

/**
 * luo_subsystems_fdt_setup - Adds and populates the 'subsystems' node in the
 * FDT.
 * @fdt: Pointer to the LUO FDT blob.
 *
 * Add subsystems node and each subsystem to the LUO FDT blob.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int luo_subsystems_fdt_setup(void *fdt)
{
	struct liveupdate_subsystem *subsystem;
	const u64 zero_data = 0;
	int ret, node_offset;

	guard(mutex)(&luo_subsystem_list_mutex);
	ret = fdt_add_subnode(fdt, 0, LUO_SUBSYSTEMS_NODE_NAME);
	if (ret < 0)
		goto exit_error;

	node_offset = ret;
	ret = fdt_setprop_string(fdt, node_offset, "compatible",
				 LUO_SUBSYSTEMS_COMPATIBLE);
	if (ret < 0)
		goto exit_error;

	list_for_each_entry(subsystem, &luo_subsystems_list, list) {
		ret = fdt_setprop(fdt, node_offset, subsystem->name,
				  &zero_data, sizeof(zero_data));
		if (ret < 0)
			goto exit_error;
	}

	luo_fdt_out = fdt;
	return 0;
exit_error:
	pr_err("Failed to setup 'subsystems' node to FDT: %s\n",
	       fdt_strerror(ret));
	return -ENOSPC;
}

/**
 * luo_subsystems_startup - Validates the LUO subsystems FDT node at startup.
 * @fdt: Pointer to the LUO FDT blob passed from the previous kernel.
 *
 * This __init function checks the existence and validity of the '/subsystems'
 * node in the FDT. This node is considered mandatory.
 */
void __init luo_subsystems_startup(void *fdt)
{
	int ret, node_offset;

	guard(mutex)(&luo_subsystem_list_mutex);
	node_offset = fdt_subnode_offset(fdt, 0, LUO_SUBSYSTEMS_NODE_NAME);
	if (node_offset < 0)
		luo_restore_fail("Failed to find /subsystems node\n");

	ret = fdt_node_check_compatible(fdt, node_offset,
					LUO_SUBSYSTEMS_COMPATIBLE);
	if (ret) {
		luo_restore_fail("FDT '%s' is incompatible with '%s' [%d]\n",
				 LUO_SUBSYSTEMS_NODE_NAME,
				 LUO_SUBSYSTEMS_COMPATIBLE, ret);
	}
	luo_fdt_in = fdt;
}

static void __luo_do_subsystems_cancel_calls(struct liveupdate_subsystem *boundary_subsystem)
{
	struct liveupdate_subsystem *subsystem;

	list_for_each_entry(subsystem, &luo_subsystems_list, list) {
		if (subsystem == boundary_subsystem)
			break;

		if (subsystem->ops->cancel) {
			subsystem->ops->cancel(subsystem,
					       subsystem->private_data);
		}
		subsystem->private_data = 0;
	}
}

static void luo_subsystems_retrieve_data_from_fdt(void)
{
	struct liveupdate_subsystem *subsystem;
	int node_offset, prop_len;
	const void *prop;

	if (!luo_fdt_in)
		return;

	node_offset = fdt_subnode_offset(luo_fdt_in, 0,
					 LUO_SUBSYSTEMS_NODE_NAME);
	list_for_each_entry(subsystem, &luo_subsystems_list, list) {
		prop = fdt_getprop(luo_fdt_in, node_offset,
				   subsystem->name, &prop_len);

		if (!prop || prop_len != sizeof(u64)) {
			luo_restore_fail("In FDT node '/%s' can't find property '%s': %s\n",
					 LUO_SUBSYSTEMS_NODE_NAME,
					 subsystem->name,
					 fdt_strerror(node_offset));
		}
		memcpy(&subsystem->private_data, prop, sizeof(u64));
	}
}

static int luo_subsystems_commit_data_to_fdt(void)
{
	struct liveupdate_subsystem *subsystem;
	int ret, node_offset;

	node_offset = fdt_subnode_offset(luo_fdt_out, 0,
					 LUO_SUBSYSTEMS_NODE_NAME);
	list_for_each_entry(subsystem, &luo_subsystems_list, list) {
		ret = fdt_setprop(luo_fdt_out, node_offset, subsystem->name,
				  &subsystem->private_data, sizeof(u64));
		if (ret < 0) {
			pr_err("Failed to set FDT property for subsystem '%s' %s\n",
			       subsystem->name, fdt_strerror(ret));
			return -ENOENT;
		}
	}

	return 0;
}

static int luo_get_subsystem_data(struct liveupdate_subsystem *h, u64 *data)
{
	int node_offset, prop_len;
	const void *prop;

	node_offset = fdt_subnode_offset(luo_fdt_in, 0,
					 LUO_SUBSYSTEMS_NODE_NAME);
	prop = fdt_getprop(luo_fdt_in, node_offset, h->name, &prop_len);
	if (!prop || prop_len != sizeof(u64)) {
		luo_state_read_exit();
		return -ENOENT;
	}
	memcpy(data, prop, sizeof(u64));

	return 0;
}

/**
 * luo_do_subsystems_prepare_calls - Calls prepare callbacks and updates FDT
 * if all prepares succeed. Handles cancellation on failure.
 *
 * Phase 1: Calls 'prepare' for all subsystems and stores results temporarily.
 * If any 'prepare' fails, calls 'cancel' on previously prepared subsystems
 * and returns the error.
 * Phase 2: If all 'prepare' calls succeeded, writes the stored data to the FDT.
 * If any FDT write fails, calls 'cancel' on *all* prepared subsystems and
 * returns the FDT error.
 *
 * Returns: 0 on success. Negative errno on failure.
 */
int luo_do_subsystems_prepare_calls(void)
{
	struct liveupdate_subsystem *subsystem;
	int ret;

	guard(mutex)(&luo_subsystem_list_mutex);
	list_for_each_entry(subsystem, &luo_subsystems_list, list) {
		if (!subsystem->ops->prepare)
			continue;

		ret = subsystem->ops->prepare(subsystem,
					      &subsystem->private_data);
		if (ret < 0) {
			pr_err("Subsystem '%s' prepare callback failed [%d]\n",
			       subsystem->name, ret);
			__luo_do_subsystems_cancel_calls(subsystem);

			return ret;
		}
	}

	ret = luo_subsystems_commit_data_to_fdt();
	if (ret)
		__luo_do_subsystems_cancel_calls(NULL);

	return ret;
}

/**
 * luo_do_subsystems_freeze_calls - Calls freeze callbacks and updates FDT
 * if all freezes succeed. Handles cancellation on failure.
 *
 * Phase 1: Calls 'freeze' for all subsystems and stores results temporarily.
 * If any 'freeze' fails, calls 'cancel' on previously called subsystems
 * and returns the error.
 * Phase 2: If all 'freeze' calls succeeded, writes the stored data to the FDT.
 * If any FDT write fails, calls 'cancel' on *all* subsystems and
 * returns the FDT error.
 *
 * Returns: 0 on success. Negative errno on failure.
 */
int luo_do_subsystems_freeze_calls(void)
{
	struct liveupdate_subsystem *subsystem;
	int ret;

	guard(mutex)(&luo_subsystem_list_mutex);
	list_for_each_entry(subsystem, &luo_subsystems_list, list) {
		if (!subsystem->ops->freeze)
			continue;

		ret = subsystem->ops->freeze(subsystem,
					     &subsystem->private_data);
		if (ret < 0) {
			pr_err("Subsystem '%s' freeze callback failed [%d]\n",
			       subsystem->name, ret);
			__luo_do_subsystems_cancel_calls(subsystem);

			return ret;
		}
	}

	ret = luo_subsystems_commit_data_to_fdt();
	if (ret)
		__luo_do_subsystems_cancel_calls(NULL);

	return ret;
}

/**
 * luo_do_subsystems_finish_calls- Calls finish callbacks for all subsystems.
 *
 * This function is called at the end of live update cycle to do the final
 * clean-up or housekeeping of the post-live update states.
 */
void luo_do_subsystems_finish_calls(void)
{
	struct liveupdate_subsystem *subsystem;

	guard(mutex)(&luo_subsystem_list_mutex);
	luo_subsystems_retrieve_data_from_fdt();

	list_for_each_entry(subsystem, &luo_subsystems_list, list) {
		if (subsystem->ops->finish) {
			subsystem->ops->finish(subsystem,
					       subsystem->private_data);
		}
		subsystem->private_data = 0;
	}
}

/**
 * luo_do_subsystems_cancel_calls - Calls cancel callbacks for all subsystems.
 *
 * This function is typically called when the live update process needs to be
 * aborted externally, for example, after the prepare phase may have run but
 * before actual reboot. It iterates through all registered subsystems and calls
 * the 'cancel' callback for those that implement it and likely completed
 * prepare.
 */
void luo_do_subsystems_cancel_calls(void)
{
	guard(mutex)(&luo_subsystem_list_mutex);
	__luo_do_subsystems_cancel_calls(NULL);
	luo_subsystems_commit_data_to_fdt();
}

/**
 * liveupdate_register_subsystem - Register a kernel subsystem handler with LUO
 * @h: Pointer to the liveupdate_subsystem structure allocated and populated
 * by the calling subsystem.
 *
 * Registers a subsystem handler that provides callbacks for different events
 * of the live update cycle. Registration is typically done during the
 * subsystem's module init or core initialization.
 *
 * Can only be called when LUO is in the NORMAL or UPDATED states.
 * The provided name (@h->name) must be unique among registered subsystems.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int liveupdate_register_subsystem(struct liveupdate_subsystem *h)
{
	struct liveupdate_subsystem *iter;
	int ret = 0;

	luo_state_read_enter();
	if (!liveupdate_state_normal() && !liveupdate_state_updated()) {
		luo_state_read_exit();
		return -EBUSY;
	}

	guard(mutex)(&luo_subsystem_list_mutex);
	list_for_each_entry(iter, &luo_subsystems_list, list) {
		if (iter == h) {
			pr_warn("Subsystem '%s' (%p) already registered.\n",
				h->name, h);
			ret = -EEXIST;
			goto out_unlock;
		}

		if (!strcmp(iter->name, h->name)) {
			pr_err("Subsystem with name '%s' already registered.\n",
			       h->name);
			ret = -EEXIST;
			goto out_unlock;
		}
	}

	if (!try_module_get(h->ops->owner)) {
		pr_warn("Subsystem '%s' unable to get reference.\n", h->name);
		ret = -EAGAIN;
		goto out_unlock;
	}

	INIT_LIST_HEAD(&h->list);
	list_add_tail(&h->list, &luo_subsystems_list);

out_unlock:
	/*
	 * If we are booting during live update, and subsystem provided a boot
	 * callback, do it now, since we know that subsystem has already
	 * initialized.
	 */
	if (!ret && liveupdate_state_updated() && h->ops->boot) {
		u64 data;

		ret = luo_get_subsystem_data(h, &data);
		if (!WARN_ON_ONCE(ret))
			h->ops->boot(h, data);
	}

	luo_state_read_exit();

	return ret;
}

/**
 * liveupdate_unregister_subsystem - Unregister a kernel subsystem handler from
 * LUO
 * @h: Pointer to the same liveupdate_subsystem structure that was used during
 * registration.
 *
 * Unregisters a previously registered subsystem handler. Typically called
 * during module exit or subsystem teardown. LUO removes the structure from its
 * internal list; the caller is responsible for any necessary memory cleanup
 * of the structure itself.
 *
 * Return: 0 on success, negative error code otherwise.
 * -EINVAL if h is NULL.
 * -ENOENT if the specified handler @h is not found in the registration list.
 * -EBUSY if LUO is not in the NORMAL state.
 */
int liveupdate_unregister_subsystem(struct liveupdate_subsystem *h)
{
	struct liveupdate_subsystem *iter;
	bool found = false;
	int ret = 0;

	luo_state_read_enter();
	if (!liveupdate_state_normal() && !liveupdate_state_updated()) {
		luo_state_read_exit();
		return -EBUSY;
	}

	guard(mutex)(&luo_subsystem_list_mutex);
	list_for_each_entry(iter, &luo_subsystems_list, list) {
		if (iter == h) {
			found = true;
			break;
		}
	}

	if (found) {
		list_del_init(&h->list);
	} else {
		pr_warn("Subsystem handler '%s' not found for unregistration.\n",
			h->name);
		ret = -ENOENT;
	}

	module_put(h->ops->owner);
	luo_state_read_exit();

	return ret;
}

/**
 * liveupdate_get_subsystem_data - Retrieve raw private data for a subsystem
 * from FDT.
 * @h:      Pointer to the liveupdate_subsystem structure representing the
 * subsystem instance. The 'name' field is used to find the property.
 * @data:   Output pointer where the subsystem's raw private u64 data will be
 * stored via memcpy.
 *
 * Reads the 8-byte data property associated with the subsystem @h->name
 * directly from the '/subsystems' node within the globally accessible
 * 'luo_fdt_in' blob. Returns appropriate error codes if inputs are invalid, or
 * nodes/properties are missing or invalid.
 *
 * Return:  0 on success. -ENOENT on error.
 */
int liveupdate_get_subsystem_data(struct liveupdate_subsystem *h, u64 *data)
{
	int ret;

	luo_state_read_enter();
	if (WARN_ON_ONCE(!luo_fdt_in || !liveupdate_state_updated())) {
		luo_state_read_exit();
		return -ENOENT;
	}

	scoped_guard(mutex, &luo_subsystem_list_mutex)
		ret = luo_get_subsystem_data(h, data);
	luo_state_read_exit();

	return ret;
}
