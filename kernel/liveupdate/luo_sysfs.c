// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: LUO sysfs interface
 *
 * Provides a sysfs interface at ``/sys/kernel/liveupdate/`` for monitoring LUO
 * state.  Live update allows rebooting the kernel (via kexec) while preserving
 * designated device state for attached workloads (e.g., VMs), useful for
 * minimizing downtime during hypervisor updates.
 *
 * /sys/kernel/liveupdate/state
 * ----------------------------
 * - Permissions:  Read-only
 * - Description:  Displays the current LUO state string.
 * - Valid States:
 *     @normal
 *       Idle state.
 *     @prepared
 *       Preparation phase complete (triggered via '/dev/liveupdate'). Resources
 *       checked, state saving initiated via %LIVEUPDATE_PREPARE event.
 *       Workloads mostly running but may be restricted. Ready forreboot
 *       trigger.
 *     @frozen
 *       Final reboot notification sent (triggered via 'reboot'). Corresponds to
 *       %LIVEUPDATE_REBOOT event. Final state saving. Workloads must be
 *       suspended. System about to kexec ("blackout window").
 *     @updated
 *       New kernel booted via live update. Awaiting 'finish' signal.
 *
 * Userspace Interaction & Blackout Window Reduction
 * -------------------------------------------------
 * Userspace monitors the ``state`` file to coordinate actions:
 *   - Suspend workloads before @frozen state is entered.
 *   - Initiate resource restoration upon entering @updated state.
 *   - Resume workloads after restoration, minimizing downtime.
 */

#include <linux/kobject.h>
#include <linux/liveupdate.h>
#include <linux/sysfs.h>
#include "luo_internal.h"

static bool luo_sysfs_initialized;

#define LUO_DIR_NAME	"liveupdate"

void luo_sysfs_notify(void)
{
	if (luo_sysfs_initialized)
		sysfs_notify(kernel_kobj, LUO_DIR_NAME, "state");
}

/* Show the current live update state */
static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return sysfs_emit(buf, "%s\n", luo_current_state_str());
}

static struct kobj_attribute state_attribute = __ATTR_RO(state);

static struct attribute *luo_attrs[] = {
	&state_attribute.attr,
	NULL
};

static struct attribute_group luo_attr_group = {
	.attrs = luo_attrs,
	.name = LUO_DIR_NAME,
};

static int __init luo_init(void)
{
	int ret;

	ret = sysfs_create_group(kernel_kobj, &luo_attr_group);
	if (ret) {
		pr_err("Failed to create group\n");
		return ret;
	}

	luo_sysfs_initialized = true;
	pr_info("Initialized\n");

	return 0;
}
subsys_initcall(luo_init);
