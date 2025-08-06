/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _LINUX_LUO_INTERNAL_H
#define _LINUX_LUO_INTERNAL_H

#include <uapi/linux/liveupdate.h>

/*
 * Handles a deserialization failure: devices and memory is in unpredictable
 * state.
 *
 * Continuing the boot process after a failure is dangerous because it could
 * lead to leaks of private data.
 */
#define luo_restore_fail(__fmt, ...) panic(__fmt, ##__VA_ARGS__)

int luo_cancel(void);
int luo_prepare(void);
int luo_freeze(void);
int luo_finish(void);

void luo_state_read_enter(void);
void luo_state_read_exit(void);

const char *luo_current_state_str(void);

void luo_subsystems_startup(void *fdt);
int luo_subsystems_fdt_setup(void *fdt);
int luo_do_subsystems_prepare_calls(void);
int luo_do_subsystems_freeze_calls(void);
void luo_do_subsystems_finish_calls(void);
void luo_do_subsystems_cancel_calls(void);

int luo_retrieve_file(u64 token, struct file **filep);
int luo_register_file(u64 token, int fd);
int luo_unregister_file(u64 token);
void luo_unregister_all_files(void);

int luo_file_get_state(u64 token, enum liveupdate_state *statep, bool incoming);
int luo_file_prepare(u64 token);
int luo_file_freeze(u64 token);
int luo_file_cancel(u64 token);
int luo_file_finish(u64 token);

#endif /* _LINUX_LUO_INTERNAL_H */
