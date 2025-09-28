/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef LUO_TEST_UTILS_H
#define LUO_TEST_UTILS_H

#include <errno.h>
#include <string.h>
#include <linux/liveupdate.h>
#include "../kselftest.h"

#define LUO_DEVICE "/dev/liveupdate"
#define STATE_SESSION_NAME "state_session"
#define STATE_MEMFD_TOKEN 999

#define MAX_FILES_PER_SESSION 5

struct session_info {
	char name[LIVEUPDATE_SESSION_NAME_LENGTH];
	int fd;
	int file_tokens[MAX_FILES_PER_SESSION];
	char file_data[MAX_FILES_PER_SESSION][128];
};

#define fail_exit(fmt, ...)						\
	ksft_exit_fail_msg("[%s] " fmt " (errno: %s)\n",		\
			   __func__, ##__VA_ARGS__, strerror(errno))

int luo_open_device(void);

int luo_create_session(int luo_fd, const char *name);
int luo_retrieve_session(int luo_fd, const char *name);

int create_and_preserve_memfd(int session_fd, int token, const char *data);
int restore_and_verify_memfd(int session_fd, int token, const char *expected_data);
int verify_session_and_get_fd(int luo_fd, struct session_info *s);

int luo_set_session_event(int session_fd, enum liveupdate_event event);
int luo_set_global_event(int luo_fd, enum liveupdate_event event);
int luo_get_global_state(int luo_fd, enum liveupdate_state *state);

void create_state_file(int luo_fd, int next_stage);
int restore_and_read_state(int luo_fd, int *stage);
void update_state_file(int session_fd, int next_stage);
void reinit_all_sessions(struct session_info *sessions, int num);

#endif /* LUO_TEST_UTILS_H */
