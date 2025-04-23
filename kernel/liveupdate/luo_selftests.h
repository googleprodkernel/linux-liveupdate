/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _LINUX_LUO_SELFTESTS_H
#define _LINUX_LUO_SELFTESTS_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Maximum number of subsystem self-test can register */
#define LUO_MAX_SUBSYSTEMS		16
#define LUO_NAME_LENGTH			32

#define LUO_CMD_SUBSYSTEM_REGISTER	0
#define LUO_CMD_SUBSYSTEM_UNREGISTER	1
#define LUO_CMD_SUBSYSTEM_GETDATA	2
struct luo_arg_subsystem {
	char name[LUO_NAME_LENGTH];
	void *data_page;
};

/*
 * Test name prefixes:
 * normal: prepare and freeze callbacks do not fail
 * prepare_fail: prepare callback fails for this test.
 * freeze_fail: freeze callback fails for this test
 */
#define NAME_NORMAL		"ksft_luo"
#define NAME_PREPARE_FAIL	"ksft_prepare_fail"
#define NAME_FREEZE_FAIL	"ksft_freeze_fail"

/**
 * struct liveupdate_selftest - Holds directions for the self-test operations.
 * @cmd:    Selftest comman defined in luo_selftests.h.
 * @arg:    Argument for the self test command.
 *
 * This structure is used only for the selftest purposes.
 */
struct liveupdate_selftest {
	__u64		cmd;
	__u64		arg;
};

/**
 * LIVEUPDATE_IOCTL_FREEZE - Notify subsystems of imminent reboot
 * transition.
 *
 * Argument: None.
 *
 * Notifies the live update subsystem and associated components that the kernel
 * is about to execute the final reboot transition into the new kernel (e.g.,
 * via kexec). This action triggers the internal %LIVEUPDATE_FREEZE kernel
 * event. This event provides subsystems a final, brief opportunity (within the
 * "blackout window") to save critical state or perform last-moment quiescing.
 * Any remaining or deferred state saving for items marked via the PRESERVE
 * ioctls typically occurs in response to the %LIVEUPDATE_FREEZE event.
 *
 * This ioctl should only be called when the system is in the
 * %LIVEUPDATE_STATE_PREPARED state. This command does not transfer data.
 *
 * Return: 0 if the notification is successfully processed by the kernel (but
 * reboot follows). Returns a negative error code if the notification fails
 * or if the system is not in the %LIVEUPDATE_STATE_PREPARED state.
 */
#define LIVEUPDATE_IOCTL_FREEZE						\
	_IO(LIVEUPDATE_IOCTL_TYPE, 0x05)

/**
 * LIVEUPDATE_IOCTL_SELFTESTS - Interface for the LUO selftests
 *
 * Argument: Pointer to &struct liveupdate_selftest.
 *
 * Use by LUO selftests, commands are declared in luo_selftests.h
 *
 * Return: 0 on success, negative error code on failure (e.g., invalid token).
 */
#define LIVEUPDATE_IOCTL_SELFTESTS					\
	_IOWR(LIVEUPDATE_IOCTL_TYPE, 0x08, struct liveupdate_selftest)

#endif /* _LINUX_LUO_SELFTESTS_H */
