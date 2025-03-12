/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef _LINUX_DEV_LIVEUPDATE_H
#define _LINUX_DEV_LIVEUPDATE_H

#include <linux/liveupdate.h>

#ifdef CONFIG_LIVEUPDATE

/**
 * struct dev_liveupdate - Device state for live update operations
 * @liveupdate_entry:     List head for linking the device into live update
 *                        related lists (e.g., a list of devices participating
 *                        in a live update sequence).
 * @liveupdate_requested: Set if a live update has been requested for this
 *                        device (i.e. device will participate in live update).
 * @liveupdate_preserved: Set if the device's state has been successfully
 *                        preserved during a live update prepare phase.
 * @liveupdate_reclaimed: Set if resources or state associated with a
 *                        previous live update attempt have been reclaimed.
 *                        Device has been re-attached to previous work and
 *                        resumed its operation.
 * @liveupdate_depth:     The hierarchical depth of the device, used for
 *                        ordering live update operations. Lower values
 *                        indicate devices closer to the root.
 *
 * This structure holds the state information required for performing
 * live update operations on a device. It is embedded within a struct device.
 */
struct dev_liveupdate {
	struct list_head liveupdate_entry;
	bool liveupdate_requested:1;
	bool liveupdate_preserved:1;
	bool liveupdate_reclaimed:1;
	int liveupdate_depth:28;
};

/**
 * struct dev_liveupdate_cbs - Live Update callback functions
 * @prepare:     Prepare device for the upcoming state transition. Driver and
 *               buse should save the necessary device state. Happens before
 *               blackouts.
 * @reboot:      A final notification before the system jumps to the new kernel.
 *               Called during blackout from reboot() syscall.
 * @finish:      The system has completed a transition. Drivers and buses should
 *               have already restored the previously saved device state.
 *               Clean-up any saved state or reset unreclaimed device.
 * @cancel:      Cancel the live update process. Driver should clean
 *               up any saved state if necessary.
 *
 * This structure is used by drivers and buses to hold the callback from LUO.
 */
struct dev_liveupdate_cbs {
	int (*prepare)(struct device *dev);
	int (*reboot)(struct device *dev);
	void (*finish)(struct device *dev);
	void (*cancel)(struct device *dev);
};

void dev_liveupdate_init(struct device *dev);
void dev_liveupdate_add_device(struct device *dev);
int dev_liveupdate_sysfs_change_owner(struct device *dev,
				      kuid_t kuid,
				      kgid_t kgid);

bool dev_liveupdate_preserved(struct device *dev);
bool dev_liveupdate_reclaimed(struct device *dev);
bool dev_liveupdate_requested(struct device *dev);
void dev_liveupdate_set_requested(struct device *dev, bool val);

#else /* CONFIG_LIVEUPDATE */

static inline void dev_liveupdate_init(struct devie *dev);
static inline void dev_liveupdate_add_device(struct device *dev) { }

static inline int dev_liveupdate_sysfs_change_owner(struct device *dev,
						    kuid_t kuid,
						    kgid_t kgid)
{
	return 0;
}

static inline bool dev_liveupdate_preserved(struct device *dev)
{
	return false;
}

static inline bool dev_liveupdate_reclaimed(struct device *dev)
{
	return false;
}

static inline bool dev_liveupdate_requested(struct device *dev)
{
	return false;
}

static inline void dev_liveupdate_set_requested(struct device *dev, bool val)
{
}

static inline void dev_liveupdate_set_reclaimed(struct device *dev);

#endif /* CONFIG_LIVEUPDATE */
#endif /* _LINUX_DEV_LIVEUPDATE_H */
