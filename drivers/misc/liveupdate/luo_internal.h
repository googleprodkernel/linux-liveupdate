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

extern const char *const luo_state_str[];

/* Get the current state as a string */
#define LUO_STATE_STR luo_state_str[READ_ONCE(luo_state)]

extern enum liveupdate_state luo_state;

#endif /* _LINUX_LUO_INTERNAL_H */
