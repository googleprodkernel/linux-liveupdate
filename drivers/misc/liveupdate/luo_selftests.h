/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _LINUX_LUO_SELFTESTS_H
#define _LINUX_LUO_SELFTESTS_H

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

#endif /* _LINUX_LUO_SELFTESTS_H */
