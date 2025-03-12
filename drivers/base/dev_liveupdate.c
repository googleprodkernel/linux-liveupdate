// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: Device Live Update
 *
 * Provides infrastructure for preserving device state across a system update.
 *
 * This subsystem allows drivers and buses to save and restore device state,
 * enabling a seamless transition during a live update.
 *
 * The core idea is to identify a set of devices whose state needs to be
 * preserved. For each such device, the associated driver and bus can implement
 * callbacks to save the device's state before the update and restore it
 * afterwards.
 *
 * Userspace can interact with this subsystem via sysfs attributes exposed
 * under each device directory (e.g., ``/sys/devices/.../liveupdate/``).
 * This directory contains the following attributes:
 *
 * ``requested``
 *   A read-write attribute allowing userspace to control whether a device
 *   should participate in the live update sequence. Writing "1" requests the
 *   device and its ancestors (that support live update) be preserved.
 *   Writing "0" requests the device be excluded. This attribute can only be
 *   modified when LUO is in the ``normal`` state.
 * ``preserved``
 *   A read-only attribute indicating whether the device's state was
 *   preserved during the ``prepare`` and ``reboot`` stages.
 * ``reclaimed``
 *   A read-only attribute indicating whether the device was successfully
 *   re-attached and resumed operation in the new kernel after an update.
 *   For example, a VM to which this device was passthrough has been resumed.
 *
 * By default, devices do not participate in the live update. Userspace can
 * explicitly request participation by writing "1" to the ``requested`` file.
 *
 * The live update process typically involves the following stages,
 * reflected in the ``liveupdate_event`` enum:
 *
 * ``LIVEUPDATE_PREPARE``
 *   Prepare devices for the upcoming state transition. Drivers and buses should
 *   save the necessary device state. Happens before blackouts.
 * ``LIVEUPDATE_REBOOT``
 *   A final notification before the system jumps to the new kernel. Called
 *   during blackout from reboot() syscall.
 * ``LIVEUPDATE_FINISH``
 *   The system has completed a transition. Drivers and buses should have
 *   already restored the previously saved state. Clean up, reset unreclaimed
 *   devices.
 * ``LIVEUPDATE_CANCEL``
 *   Cancel the live update process. Drivers and buses should clean up any saved
 *   state if necessary.
 *
 * Documentation/admin-guide/liveupdate.rst contains more details.
 *
 * The global state of the live update subsystem can be accessed and
 * controlled via a separate sysfs interface (e.g., ``/sys/kernel/liveupdate/``)
 * via Live Update Orchestrator.
 */

#undef pr_fmt
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/dev_liveupdate.h>
#include <linux/list_sort.h>
#include <linux/kobject.h>
#include <linux/device.h>
#include <linux/pci.h>
#include "base.h"

static const char liveupdate_group_name[] = "liveupdate";

/**
 * is_liveupdate_possible() - Check if a device can participate in live update
 * @dev: The device to check.
 *
 * This function verifies if the given device and all its ancestors (up to
 * the root device or until a missing callback is found) are capable of
 * participating in a live update.
 *
 * It checks for the presence of the ``liveupdate`` callback in the device's
 * driver and bus, and performs the same check for all parent devices. If any
 * device in the hierarchy (including the device itself)
 * lacks a ``liveupdate`` callback in either its driver or bus, the function
 * returns false.
 *
 * Return: True if the device and all its relevant ancestors have the
 * liveupdate callback, false otherwise.
 */
static bool is_liveupdate_possible(struct device *dev)
{
	struct device *parent_dev;
	bool is_possible = true;

	dev = get_device(dev);
	for (; ;) {
		if (dev->driver) {
			is_possible = !!dev->driver->liveupdate;
			if (!is_possible) {
				dev_warn(dev, "driver[%s] no liveupdate callback\n",
					 dev->driver->name);
				break;
			}
		}

		if (dev->bus) {
			is_possible = !!dev->bus->liveupdate;
			if (!is_possible) {
				dev_warn(dev, "bus[%s] no liveupdate callback\n",
					 dev->bus->name);
				break;
			}
		}

		if (!dev->parent)
			break;

		parent_dev = get_device(dev->parent);
		put_device(dev);
		dev = parent_dev;
	}
	put_device(dev);

	return is_possible;
}

/*
 * dev->{driver, bus}->liveupdate->{prepare, reboot} callback
 * Warn if liveupdate not present, this is an internal error, and should never
 * be the case.
 * return callback result, or 0 if callback is not implemented.
 */
#define DEV_LIVEUPDATE_RET_CALLBACK(_dev, _drv_or_bus, _func) ({	\
	int rv = 0;							\
									\
	if ((_dev)->_drv_or_bus &&					\
	    !WARN_ON(!(_dev)->_drv_or_bus->liveupdate) &&		\
	    (_dev)->_drv_or_bus->liveupdate->_func) {			\
		rv = (_dev)->_drv_or_bus->liveupdate->_func(_dev);	\
	}								\
	rv;								\
})

/*
 * A void variant of the previous macro
 * dev->{driver, bus}->liveupdate->{cancel, finish} callback
 * Warn if liveupdate not present, this is an internal error, and should never
 * be the case.
 */
#define DEV_LIVEUPDATE_CALLBACK(_dev, _drv_or_bus, _func) do {		\
	if ((_dev)->_drv_or_bus &&					\
	    !WARN_ON(!(_dev)->_drv_or_bus->liveupdate) &&		\
	    (_dev)->_drv_or_bus->liveupdate->_func) {			\
		(_dev)->_drv_or_bus->liveupdate->_func(_dev);		\
	}								\
} while (0)

static ssize_t preserved_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", dev_liveupdate_preserved(dev));
}
static DEVICE_ATTR_RO(preserved);

static ssize_t reclaimed_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "%d\n", dev_liveupdate_reclaimed(dev));
}
static DEVICE_ATTR_RO(reclaimed);

static ssize_t requested_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "%d\n", dev_liveupdate_requested(dev));
}

/**
 * requested_store() - Store function for the ``requested`` sysfs attribute
 * @dev: The device associated with the attribute.
 * @attr: The device attribute structure.
 * @buf: The buffer containing the value written by the user.
 * @count: The number of bytes written.
 *
 * Allows userspace to request that a device be included in or excluded from
 * the live update process. Writing "1" requests the device to be preserved
 * during live update, and writing "0" requests it to be excluded.
 *
 * This function checks if the live update system is in the 'normal' state
 * before allowing changes. It also verifies that the device supports
 * live update before setting the requested state.
 *
 * Return: The number of bytes written on success, ``-EINVAL`` if the input is
 * invalid or if the live update system is not in the 'normal' state, or
 * ``-EAGAIN`` if the operation was interrupted.
 */
static ssize_t requested_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	long val;

	if (kstrtol(buf, 0, &val) < 0)
		return -EINVAL;

	if (val != 1 && val != 0)
		return -EINVAL;

	/* if state does not change, ignore */
	if (dev_liveupdate_requested(dev) == !!val)
		return count;

	if (liveupdate_read_state_enter_killable()) {
		dev_warn(dev, "Changing requested state Canceled by user\n");
		return -EAGAIN;
	}

	if (!liveupdate_state_normal()) {
		dev_warn(dev, "Participation can be requested only in [normal] state\n");
		liveupdate_read_state_exit();
		return -EINVAL;
	}

	if (!val) {
		dev_liveupdate_set_requested(dev, false);
		list_del_init(&dev->lu.liveupdate_entry);
		liveupdate_read_state_exit();
		return count;
	}

	if (!is_liveupdate_possible(dev)) {
		liveupdate_read_state_exit();
		return -EINVAL;
	}

	dev_liveupdate_set_requested(dev, true);
	liveupdate_read_state_exit();

	return count;
}
static DEVICE_ATTR_RW(requested);

static struct attribute *liveupdate_attrs[] = {
	&dev_attr_preserved.attr,
	&dev_attr_reclaimed.attr,
	&dev_attr_requested.attr,
	NULL,
};

static const struct attribute_group liveupdate_attr_group = {
	.name	= liveupdate_group_name,
	.attrs	= liveupdate_attrs,
};

static int dev_liveupdate_sysfs_add(struct device *dev)
{
	int rv;

	rv = sysfs_create_group(&dev->kobj, &liveupdate_attr_group);

	return rv;
}

static int dev_liveupdate_get_depth(struct device *current_dev)
{
	struct device *dev;
	int depth = 0;

	for (dev = current_dev; dev; dev = dev->parent)
		depth++;

	return depth;
}

/**
 * LIST_HEAD(dev_liveupdate_preserve_list) - List of devices to preserve during
 * live update
 * @dev_liveupdate_preserve_list: This section is about this list.
 *
 * This list holds devices that need to have their state preserved across a
 * live update. It is populated during the ``LIVEUPDATE_PREPARE`` stage by
 * dev_liveupdate_build_preserve_list() with devices explicitly requested
 * for live update and their ancestors. The list is sorted by device depth
 * to ensure correct processing order: children before parents.
 *
 * Functions like __dev_liveupdate_reboot_prepare() iterate through this list
 * to notify drivers and buses about the upcoming update or reboot.
 * __dev_liveupdate_cancel() uses this list to perform cancellation.
 * The list is cleared by dev_liveupdate_destroy_preserve_list() when it's
 * no longer needed.
 *
 * The list is protected by ``luo_state_rwsem`` as it is used only during
 * prepare and reboot callbacks when this lock is taken as writer.
 */
static LIST_HEAD(dev_liveupdate_preserve_list);

/**
 * __find_ancestors_and_depth() - Add a device and its ancestors to the preserve
 * list
 * @current_dev: The device to start with.
 *
 * This function adds the @current_dev and all its ancestors to the
 * dev_liveupdate_preserve_list. It also calculates and sets the
 * liveupdate_depth for each device added, relative to the @current_dev.
 *
 * The function iterates from @current_dev up to the root device. For each
 * device in the path, if it's not already in the preserve list (checked via
 * the liveupdate_depth field), it's added to the list, its depth is set,
 * and a reference is taken using get_device() (unless it's the initial
 * @current_dev, which already has a reference).
 *
 * The list to which the devices are added (dev_liveupdate_preserve_list) is
 * expected to be sorted later.
 */
static void __find_ancestors_and_depth(struct device *current_dev)
{
	struct device *dev;
	int depth = 0;

	/*
	 * If depth is set, it means this devices was already included as an
	 * ancestor of another requested device.
	 */
	if (current_dev->lu.liveupdate_depth)
		return;

	depth = dev_liveupdate_get_depth(dev);

	for (dev = current_dev; dev; dev = dev->parent) {
		/*
		 * This ancestor, and all above are already in the
		 * dev_liveupdate_preserve_list
		 */
		if (dev->lu.liveupdate_depth)
			break;

		if (dev != current_dev)
			get_device(dev);

		/* Ancestor might be in the request_list */
		list_del_init(&dev->lu.liveupdate_entry);
		dev->lu.liveupdate_depth = depth;
		list_add_tail(&dev->lu.liveupdate_entry,
			      &dev_liveupdate_preserve_list);
		depth--;
	}
}

static int dev_depth_cmp(void *priv,
			 const struct list_head *head_a,
			 const struct list_head *head_b)
{
	struct device *dev_a, *dev_b;

	dev_a = container_of(head_a, struct device, lu.liveupdate_entry);
	dev_b = container_of(head_b, struct device, lu.liveupdate_entry);

	if (dev_a->lu.liveupdate_depth > dev_b->lu.liveupdate_depth)
		return -1;

	if (dev_a->lu.liveupdate_depth < dev_b->lu.liveupdate_depth)
		return 1;

	return 0;
}

/**
 * dev_liveupdate_build_preserve_list() - Build a list of devices to preserve
 *
 * This function constructs a list ``dev_liveupdate_preserve_list`` of devices
 * that require state preservation during a live update.
 *
 * It first iterates through all devices and identifies those for which a live
 * update has been explicitly requested using dev_liveupdate_requested().
 * These devices are added to a temporary list.
 *
 * Then, for each device in the temporary list, the function calls
 * __find_ancestors_and_depth() to add the device and all its ancestors to the
 * global ``dev_liveupdate_preserve_list`` and calculate their respective
 * depths.
 *
 * Finally, the ``dev_liveupdate_preserve_list`` is sorted by device depth using
 * dev_depth_cmp() to ensure a correct preservation order (e.g., children before
 * parents). A reference count is maintained for each device added to the
 * preserve list using get_device().
 */
static void dev_liveupdate_build_preserve_list(void)
{
	LIST_HEAD(request_list);
	struct device *dev;

	spin_lock(&devices_kset->list_lock);
	list_for_each_entry(dev, &devices_kset->list, kobj.entry) {
		get_device(dev);
		spin_unlock(&devices_kset->list_lock);
		if (dev_liveupdate_requested(dev)) {
			list_add_tail(&dev->lu.liveupdate_entry,
				      &request_list);
		} else {
			put_device(dev);
		}
		spin_lock(&devices_kset->list_lock);
	}
	spin_unlock(&devices_kset->list_lock);

	while (!list_empty(&request_list)) {
		dev = list_first_entry(&request_list,
				       struct device,
				       lu.liveupdate_entry);
		list_del_init(&dev->lu.liveupdate_entry);
		__find_ancestors_and_depth(dev);
	}

	list_sort(NULL, &dev_liveupdate_preserve_list, dev_depth_cmp);
}

/**
 * dev_liveupdate_destroy_preserve_list() - Destroy the live update preserve
 * list
 *
 * This function iterates through the ``dev_liveupdate_preserve_list``, which
 * contains devices ordered by depth, and performs cleanup for each device.
 * For each device in the list, it:
 *
 * 1. Removes the device from the list and reinitializes its list head.
 * 2. Resets the liveupdate_depth field to 0.
 * 3. Calls put_device() to decrement the device's reference count.
 *
 * This function is typically called after the preserve list is no longer
 * needed, such as after the reboot phase of a live update or during
 * cancellation.
 */
static void dev_liveupdate_destroy_preserve_list(void)
{
	struct device *dev;

	while (!list_empty(&dev_liveupdate_preserve_list)) {
		dev = list_first_entry(&dev_liveupdate_preserve_list,
				       struct device,
				       lu.liveupdate_entry);
		list_del_init(&dev->lu.liveupdate_entry);
		dev->lu.liveupdate_depth = 0;
		put_device(dev);
	}
}

/**
 * __dev_liveupdate_cancel() - Cancel live update for devices
 * @dev: The device from which to start the cancellation (or NULL to cancel
 * all).
 *
 * This function cancels the ongoing live update process for devices starting
 * from the position just before the given @dev in the
 * ``dev_liveupdate_preserve_list`` and proceeding backwards to the beginning of
 * the list. If @dev is ``NULL``, the cancellation is performed for all devices
 * in the list.
 *
 * It iterates through the relevant devices in reverse order, calling the
 * ``LIVEUPDATE_CANCEL`` handler for each device's bus and driver (if
 * available). After processing the devices, it clears the liveupdate_preserved
 * flag for each device and finally destroys the
 * ``dev_liveupdate_preserve_list``.
 */
static void __dev_liveupdate_cancel(struct device *dev)
{
	dev = list_prepare_entry(dev, &dev_liveupdate_preserve_list,
				 lu.liveupdate_entry);

	list_for_each_entry_continue_reverse(dev, &dev_liveupdate_preserve_list,
					     lu.liveupdate_entry) {
		DEV_LIVEUPDATE_CALLBACK(dev, bus, cancel);
		DEV_LIVEUPDATE_CALLBACK(dev, driver, cancel);

		dev->lu.liveupdate_preserved = false;
	}

	dev_liveupdate_destroy_preserve_list();
}

/**
 * __dev_liveupdate_reboot_prepare() - Notify drivers and buses of a
 * prepare/reboot event
 * @event: The live update event, either ``LIVEUPDATE_PREPARE`` or
 * ``LIVEUPDATE_REBOOT``.
 *
 * This function iterates through the list of devices to be preserved
 * (``dev_liveupdate_preserve_list``) and calls the liveupdate() callback for
 * the driver and bus of each device with the specified event.
 *
 * If a driver or bus  callback returns an error, a warning is logged,
 * and the function attempts to cancel the live update for the remaining devices
 * using __dev_liveupdate_cancel().
 *
 * Upon successful completion for a device, the ``liveupdate_preserved`` flag
 * for that device is set to true.
 *
 * Return: 0 on success, or the error code from the failing driver/bus
 * liveupdate->{prepare, reboot} callback.
 */
static int __dev_liveupdate_reboot_prepare(enum liveupdate_event event)
{
	struct device *dev;
	int rv;

	rv = 0;
	list_for_each_entry(dev, &dev_liveupdate_preserve_list,
			    lu.liveupdate_entry) {
		if (event == LIVEUPDATE_PREPARE)
			rv = DEV_LIVEUPDATE_RET_CALLBACK(dev, driver, prepare);
		else
			rv = DEV_LIVEUPDATE_RET_CALLBACK(dev, driver, reboot);

		if (rv) {
			dev_warn(dev, "driver live update failed\n");
			goto err_cancel;
		}

		if (event == LIVEUPDATE_PREPARE)
			rv = DEV_LIVEUPDATE_RET_CALLBACK(dev, bus, prepare);
		else
			rv = DEV_LIVEUPDATE_RET_CALLBACK(dev, bus, reboot);

		if (rv) {
			dev_warn(dev, "bus live update failed\n");
			goto err_cancel_bus;
		}

		dev->lu.liveupdate_preserved = true;
	}

	return 0;

err_cancel_bus:
	DEV_LIVEUPDATE_CALLBACK(dev, driver, cancel);

err_cancel:
	__dev_liveupdate_cancel(dev);

	return rv;
}

/**
 * device_liveupdate_prepare() - Prepare devices for a live update
 *
 * This function is called as part of the ``LIVEUPDATE_PREPARE`` stage.
 * It first calls dev_liveupdate_build_preserve_list() to construct a list
 * of devices that need their state preserved during the update.
 * Then, it calls the internal function __dev_liveupdate_reboot_prepare()
 * with the ``LIVEUPDATE_PREPARE`` event to notify drivers and buses to prepare
 * for the upcoming update.
 *
 * Return: The return value from __dev_liveupdate_reboot_prepare().
 */
static int device_liveupdate_prepare(void)
{
	dev_liveupdate_build_preserve_list();

	return __dev_liveupdate_reboot_prepare(LIVEUPDATE_PREPARE);
}

/**
 * device_liveupdate_reboot() - Prepare devices for the reboot stage of a live
 * update
 *
 * This function is called as part of the ``LIVEUPDATE_REBOOT`` stage, from
 * reboot() syscall. It calls the internal function
 * __dev_liveupdate_reboot_prepare() with the LIVEUPDATE_REBOOT event to notify
 * drivers and buses to perform any actions needed before the reboot.  If the
 * reboot preparation is successful (returns 0), it then calls
 * dev_liveupdate_destroy_preserve_list() to free the list of devices that was
 * built during the prepare stage.
 *
 * Return: The return value from __dev_liveupdate_reboot_prepare().
 */
static int device_liveupdate_reboot(void)
{
	int rv;

	rv = __dev_liveupdate_reboot_prepare(LIVEUPDATE_REBOOT);
	if (!rv)
		dev_liveupdate_destroy_preserve_list();

	return rv;
}

/**
 * device_liveupdate_finish() - Finalize the device live update process
 *
 * This function is called as part of the ``LIVEUPDATE_FINISH`` stage. It
 * iterates through all registered devices, identifies devices that were
 * preserved during the prepare phase, sorts them by depth.
 *
 * After sorting, the function iterates through the list. For each device, it
 * logs a warning about unreclaimed device and call the
 * ``{driver, bus}->liveupdate->finish()`` handler for ever device's driver and
 * bus on the list. Finally, it resets the live update related fields in the
 * device's ``dev_liveupdate`` structure, effectively removing it from the live
 * update tracking.
 *
 * Note: this function must not fail.
 *
 * Return: Always returns 0.
 */
static int device_liveupdate_finish(void)
{
	LIST_HEAD(preserved_list);
	struct device *dev;

	spin_lock(&devices_kset->list_lock);
	list_for_each_entry(dev, &devices_kset->list, kobj.entry) {
		get_device(dev);
		spin_unlock(&devices_kset->list_lock);
		if (!dev_liveupdate_preserved(dev)) {
			put_device(dev);
			spin_lock(&devices_kset->list_lock);
			continue;
		}

		list_add_tail(&dev->lu.liveupdate_entry, &preserved_list);
		dev->lu.liveupdate_depth = dev_liveupdate_get_depth(dev);
		spin_lock(&devices_kset->list_lock);
	}
	spin_unlock(&devices_kset->list_lock);

	list_sort(NULL, &preserved_list, dev_depth_cmp);

	while (!list_empty(&preserved_list)) {
		dev = list_first_entry(&preserved_list, struct device,
				       lu.liveupdate_entry);

		if (!dev_liveupdate_reclaimed(dev))
			dev_warn(dev, "Device was not reclaimed during live update\n");

		DEV_LIVEUPDATE_CALLBACK(dev, driver, finish);
		DEV_LIVEUPDATE_CALLBACK(dev, bus, finish);

		/* Reset live update fields to their default values */
		list_del_init(&dev->lu.liveupdate_entry);
		dev->lu.liveupdate_reclaimed = false;
		dev->lu.liveupdate_preserved = false;
		dev->lu.liveupdate_depth = 0;
		put_device(dev);
	}

	return 0;
}

/**
 * device_liveupdate_cancel() - Cancel the ongoing device live update process
 *
 * This function is called as part of the ``LIVEUPDATE_CANCEL`` stage. It
 * initiates the cancellation of the live update process by calling the
 * internal function __dev_liveupdate_cancel() with a NULL argument,
 * indicating a global cancellation.
 *
 * Note: this function must not fail.
 *
 * Return: Always returns 0.
 */
static int device_liveupdate_cancel(void)
{
	__dev_liveupdate_cancel(NULL);

	return 0;
}

LIVEUPDATE_DECLARE_NOTIFIER(device, LIVEUPDATE_CB_PRIO_WITH_DEVICES);

/**
 * dev_liveupdate_startup() - Register device live update notifier
 *
 * This function is called during the late initialization phase of the kernel.
 * It registers a notifier for devices subsystem with live update orchestrator.
 *
 * If registration fails, a warning message is printed to the kernel log.
 *
 * Return: 0 on success (notifier registration is void, so only failure
 * is explicitly handled).
 */
static int __init dev_liveupdate_startup(void)
{
	int rv;

	rv = LIVEUPDATE_REGISTER_NOTIFIER(device);
	if (rv) {
		pr_warn("Failed to register devices with live update orchestrator [%d]\n",
			rv);
	}

	return 0;
}
late_initcall(dev_liveupdate_startup);

/* Public Interfaces */

/**
 * dev_liveupdate_init() - Initialize the dev_liveupdate structure
 * @dev: Pointer to the dev_liveupdate structure to initialize.
 *
 * This function initializes the fields of the dev_liveupdate structure
 * to their default states. The list head is initialized, and the
 * boolean flags are cleared. The depth is initialized to 0.
 */
void dev_liveupdate_init(struct device *dev)
{
	INIT_LIST_HEAD(&dev->lu.liveupdate_entry);
	dev->lu.liveupdate_requested = false;
	dev->lu.liveupdate_preserved = false;
	dev->lu.liveupdate_reclaimed = false;
	dev->lu.liveupdate_depth = 0;
}
EXPORT_SYMBOL_GPL(dev_liveupdate_init);

/**
 * dev_liveupdate_add_device() - Add live update sysfs interface to a new device
 * @dev: The device to add to the live update system.
 *
 * This function checks if live update functionality is enabled. If it is,
 * it attempts to add the live update sysfs interface for the given device.
 * If the sysfs group creation fails, a warning message is logged.
 */
void dev_liveupdate_add_device(struct device *dev)
{
	if (!liveupdate_enabled())
		return;

	if (dev_liveupdate_sysfs_add(dev))
		dev_warn(dev, "Failed to create liveupdate sysfs group\n");
}
EXPORT_SYMBOL_GPL(dev_liveupdate_add_device);

/**
 * dev_liveupdate_sysfs_change_owner() - Change the owner of the liveupdate
 * sysfs group
 * @dev: The device whose liveupdate sysfs group owner is to be changed.
 * @kuid: The user ID for the new owner.
 * @kgid: The group ID for the new owner.
 *
 * This function changes the ownership of the sysfs attribute group associated
 * with the live update interface for the given device. It uses the
 * sysfs_group_change_owner() function to update the owner to the specified
 * user ID (@kuid) and group ID (@kgid).
 *
 * Return: 0 on success, or a negative error code returned by
 * sysfs_group_change_owner().
 */
int dev_liveupdate_sysfs_change_owner(struct device *dev,
				      kuid_t kuid,
				      kgid_t kgid)
{
	return sysfs_group_change_owner(&dev->kobj, &liveupdate_attr_group,
					kuid, kgid);
}
EXPORT_SYMBOL_GPL(dev_liveupdate_sysfs_change_owner);

/**
 * dev_liveupdate_preserved() - Check if a device's live update state is
 * preserved
 * @dev: The device to check.
 *
 * Returns: true if the device's live update state has been preserved,
 * false otherwise.
 */
bool dev_liveupdate_preserved(struct device *dev)
{
	return dev->lu.liveupdate_preserved;
}
EXPORT_SYMBOL_GPL(dev_liveupdate_preserved);

/**
 * dev_liveupdate_reclaimed() - Check if a device was reclaimed after live
 * update
 * @dev: The device to check.
 *
 * Returns: true if the device has been reclaimed, false otherwise.
 */
bool dev_liveupdate_reclaimed(struct device *dev)
{
	return dev->lu.liveupdate_reclaimed;
}
EXPORT_SYMBOL_GPL(dev_liveupdate_reclaimed);

/**
 * dev_liveupdate_requested() - Check if a live update has been requested for
 * the device
 * @dev: The device to check.
 *
 * Returns: true if a live update has been requested for the device (i.e.
 * device and its ancestors are going to participate in live update), false
 * otherwise.
 */
bool dev_liveupdate_requested(struct device *dev)
{
	return dev->lu.liveupdate_requested;
}
EXPORT_SYMBOL_GPL(dev_liveupdate_requested);

/**
 * dev_liveupdate_set_requested() - Set the live update requested state for a
 * device
 * @dev: The device to modify.
 * @val: The boolean value to set the requested state to (true or false).
 *
 * Sets the ``liveupdate_requested`` flag for the given device to the
 * specified value.
 */
void dev_liveupdate_set_requested(struct device *dev, bool val)
{
	dev->lu.liveupdate_requested = val;
}
EXPORT_SYMBOL_GPL(dev_liveupdate_set_requested);
