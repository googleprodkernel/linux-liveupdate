// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: Live Update Orchestrator (LUO)
 *
 * Live Update is a specialized, kexec-based reboot process that allows a
 * running kernel to be updated from one version to another while preserving
 * the state of selected resources and keeping designated hardware devices
 * operational. For these devices, DMA activity may continue throughout the
 * kernel transition.
 *
 * While the primary use case driving this work is supporting live updates of
 * the Linux kernel when it is used as a hypervisor in cloud environments, the
 * LUO framework itself is designed to be workload-agnostic. Much like Kernel
 * Live Patching, which applies security fixes regardless of the workload,
 * Live Update facilitates a full kernel version upgrade for any type of system.
 *
 * For example, a non-hypervisor system running an in-memory cache like
 * memcached with many gigabytes of data can use LUO. The userspace service
 * can place its cache into a memfd, have its state preserved by LUO, and
 * restore it immediately after the kernel kexec.
 *
 * Whether the system is running virtual machines, containers, a
 * high-performance database, or networking services, LUO's primary goal is to
 * enable a full kernel update by preserving critical userspace state and
 * keeping essential devices operational.
 *
 * The core of LUO is a state machine that tracks the progress of a live update,
 * along with a callback API that allows other kernel subsystems to participate
 * in the process. Example subsystems that can hook into LUO include: kvm,
 * iommu, interrupts, vfio, participating filesystems, and memory management.
 *
 * LUO uses Kexec Handover to transfer memory state from the current kernel to
 * the next kernel. For more details see
 * Documentation/core-api/kho/concepts.rst.
 *
 * The LUO state machine ensures that operations are performed in the correct
 * sequence and provides a mechanism to track and recover from potential
 * failures.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/kobject.h>
#include <linux/liveupdate.h>
#include <linux/rwsem.h>
#include <linux/string.h>
#include "luo_internal.h"

static DECLARE_RWSEM(luo_state_rwsem);

static enum liveupdate_state luo_state = LIVEUPDATE_STATE_UNDEFINED;

static const char *const luo_state_str[] = {
	[LIVEUPDATE_STATE_UNDEFINED]	= "undefined",
	[LIVEUPDATE_STATE_NORMAL]	= "normal",
	[LIVEUPDATE_STATE_PREPARED]	= "prepared",
	[LIVEUPDATE_STATE_FROZEN]	= "frozen",
	[LIVEUPDATE_STATE_UPDATED]	= "updated",
};

static bool luo_enabled;

static int __init early_liveupdate_param(char *buf)
{
	return kstrtobool(buf, &luo_enabled);
}
early_param("liveupdate", early_liveupdate_param);

/* Return true if the current state is equal to the provided state */
static inline bool is_current_luo_state(enum liveupdate_state expected_state)
{
	return liveupdate_get_state() == expected_state;
}

static void __luo_set_state(enum liveupdate_state state)
{
	WRITE_ONCE(luo_state, state);
}

static inline void luo_set_state(enum liveupdate_state state)
{
	pr_info("Switched from [%s] to [%s] state\n",
		luo_current_state_str(), luo_state_str[state]);
	__luo_set_state(state);
}

static int luo_do_freeze_calls(void)
{
	return 0;
}

static void luo_do_finish_calls(void)
{
}

/* Get the current state as a string */
const char *luo_current_state_str(void)
{
	return luo_state_str[liveupdate_get_state()];
}

enum liveupdate_state liveupdate_get_state(void)
{
	return READ_ONCE(luo_state);
}

int luo_prepare(void)
{
	return 0;
}

/**
 * luo_freeze() - Initiate the final freeze notification phase for live update.
 *
 * Attempts to transition the live update orchestrator state from
 * %LIVEUPDATE_STATE_PREPARED to %LIVEUPDATE_STATE_FROZEN. This function is
 * typically called just before the actual reboot system call (e.g., kexec)
 * is invoked, either directly by the orchestration tool or potentially from
 * within the reboot syscall path itself.
 *
 * @return  0: Success. Negative error otherwise. State is reverted to
 * %LIVEUPDATE_STATE_NORMAL in case of an error during callbacks, and everything
 * is canceled via cancel notifcation.
 */
int luo_freeze(void)
{
	int ret;

	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn("[freeze] event canceled by user\n");
		return -EAGAIN;
	}

	if (!is_current_luo_state(LIVEUPDATE_STATE_PREPARED)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_FROZEN],
			luo_current_state_str());
		up_write(&luo_state_rwsem);

		return -EINVAL;
	}

	ret = luo_do_freeze_calls();
	if (!ret)
		luo_set_state(LIVEUPDATE_STATE_FROZEN);
	else
		luo_set_state(LIVEUPDATE_STATE_NORMAL);

	up_write(&luo_state_rwsem);

	return ret;
}

/**
 * luo_finish - Finalize the live update process in the new kernel.
 *
 * This function is called  after a successful live update reboot into a new
 * kernel, once the new kernel is ready to transition to the normal operational
 * state. It signals the completion of the live update sequence to subsystems.
 *
 * @return 0 on success, ``-EAGAIN`` if the state change was cancelled by the
 * user while waiting for the lock, or ``-EINVAL`` if the orchestrator is not in
 * the updated state.
 */
int luo_finish(void)
{
	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn("[finish] event canceled by user\n");
		return -EAGAIN;
	}

	if (!is_current_luo_state(LIVEUPDATE_STATE_UPDATED)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_NORMAL],
			luo_current_state_str());
		up_write(&luo_state_rwsem);

		return -EINVAL;
	}

	luo_do_finish_calls();
	luo_set_state(LIVEUPDATE_STATE_NORMAL);

	up_write(&luo_state_rwsem);

	return 0;
}

int luo_cancel(void)
{
	return 0;
}

void luo_state_read_enter(void)
{
	down_read(&luo_state_rwsem);
}

void luo_state_read_exit(void)
{
	up_read(&luo_state_rwsem);
}

static int __init luo_startup(void)
{
	__luo_set_state(LIVEUPDATE_STATE_NORMAL);

	return 0;
}
early_initcall(luo_startup);

/* Public Functions */

/**
 * liveupdate_reboot() - Kernel reboot notifier for live update final
 * serialization.
 *
 * This function is invoked directly from the reboot() syscall pathway if a
 * reboot is initiated while the live update state is %LIVEUPDATE_STATE_PREPARED
 * (i.e., if the user did not explicitly trigger the frozen state). It handles
 * the implicit transition into the final frozen state.
 *
 * It triggers the %LIVEUPDATE_REBOOT event callbacks for participating
 * subsystems. These callbacks must perform final state saving very quickly as
 * they execute during the blackout period just before kexec.
 *
 * If any %LIVEUPDATE_FREEZE callback fails, this function triggers the
 * %LIVEUPDATE_CANCEL event for all participants to revert their state, aborts
 * the live update, and returns an error.
 */
int liveupdate_reboot(void)
{
	if (!is_current_luo_state(LIVEUPDATE_STATE_PREPARED))
		return 0;

	return luo_freeze();
}

/**
 * liveupdate_state_updated - Check if the system is in the live update
 * 'updated' state.
 *
 * This function checks if the live update orchestrator is in the
 * ``LIVEUPDATE_STATE_UPDATED`` state. This state indicates that the system has
 * successfully rebooted into a new kernel as part of a live update, and the
 * preserved devices are expected to be in the process of being reclaimed.
 *
 * This is typically used by subsystems during early boot of the new kernel
 * to determine if they need to attempt to restore state from a previous
 * live update.
 *
 * @return true if the system is in the ``LIVEUPDATE_STATE_UPDATED`` state,
 * false otherwise.
 */
bool liveupdate_state_updated(void)
{
	return is_current_luo_state(LIVEUPDATE_STATE_UPDATED);
}

/**
 * liveupdate_state_normal - Check if the system is in the live update 'normal'
 * state.
 *
 * This function checks if the live update orchestrator is in the
 * ``LIVEUPDATE_STATE_NORMAL`` state. This state indicates that no live update
 * is in progress. It represents the default operational state of the system.
 *
 * This can be used to gate actions that should only be performed when no
 * live update activity is occurring.
 *
 * @return true if the system is in the ``LIVEUPDATE_STATE_NORMAL`` state,
 * false otherwise.
 */
bool liveupdate_state_normal(void)
{
	return is_current_luo_state(LIVEUPDATE_STATE_NORMAL);
}

/**
 * liveupdate_enabled - Check if the live update feature is enabled.
 *
 * This function returns the state of the live update feature flag, which
 * can be controlled via the ``liveupdate`` kernel command-line parameter.
 *
 * @return true if live update is enabled, false otherwise.
 */
bool liveupdate_enabled(void)
{
	return luo_enabled;
}
