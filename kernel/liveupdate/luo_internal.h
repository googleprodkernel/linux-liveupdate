/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _LINUX_LUO_INTERNAL_H
#define _LINUX_LUO_INTERNAL_H

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

struct luo_session;

int luo_retrieve_file(u64 token, struct file **filep);
int luo_register_file(struct luo_session *s, u64 token, int fd);
int luo_unregister_file(struct luo_session *s, u64 token);

struct luo_session *luo_create_session(void);
void luo_destroy_session(struct luo_session *s);

#ifdef CONFIG_LIVEUPDATE_SYSFS_API
void luo_sysfs_notify(void);
#else
static inline void luo_sysfs_notify(void) {}
#endif

#ifdef CONFIG_KEXEC_HANDOVER_DEBUG
extern struct dentry *liveupdate_debugfs_root;
#endif

#endif /* _LINUX_LUO_INTERNAL_H */
