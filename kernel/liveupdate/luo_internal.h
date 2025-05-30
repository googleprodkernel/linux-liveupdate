/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _LINUX_LUO_INTERNAL_H
#define _LINUX_LUO_INTERNAL_H

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
extern struct rw_semaphore luo_state_rwsem;

const char *luo_current_state_str(void);

void *luo_contig_alloc_preserve(size_t size);
void luo_contig_free_unpreserve(void *mem, size_t size);
void luo_contig_free_restore(void *mem, size_t size);

#endif /* _LINUX_LUO_INTERNAL_H */
