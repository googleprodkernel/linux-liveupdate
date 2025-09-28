// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdarg.h>

#include "luo_test_utils.h"
#include "../kselftest.h"

/* The fail_exit function is now a macro in the header. */

int luo_open_device(void)
{
	return open(LUO_DEVICE, O_RDWR);
}

int luo_create_session(int luo_fd, const char *name)
{
	struct liveupdate_ioctl_create_session arg = { .size = sizeof(arg) };

	snprintf((char *)arg.name, LIVEUPDATE_SESSION_NAME_LENGTH, "%.*s",
		 LIVEUPDATE_SESSION_NAME_LENGTH - 1, name);
	if (ioctl(luo_fd, LIVEUPDATE_IOCTL_CREATE_SESSION, &arg) < 0)
		return -errno;
	return arg.fd;
}

int luo_retrieve_session(int luo_fd, const char *name)
{
	struct liveupdate_ioctl_retrieve_session arg = { .size = sizeof(arg) };

	snprintf((char *)arg.name, LIVEUPDATE_SESSION_NAME_LENGTH, "%.*s",
		 LIVEUPDATE_SESSION_NAME_LENGTH - 1, name);
	if (ioctl(luo_fd, LIVEUPDATE_IOCTL_RETRIEVE_SESSION, &arg) < 0)
		return -errno;
	return arg.fd;
}

int create_and_preserve_memfd(int session_fd, int token, const char *data)
{
	struct liveupdate_session_preserve_fd arg = { .size = sizeof(arg) };
	long page_size = sysconf(_SC_PAGE_SIZE);
	void *map = MAP_FAILED;
	int mfd = -1, ret = -1;

	mfd = memfd_create("test_mfd", 0);
	if (mfd < 0)
		return -errno;

	if (ftruncate(mfd, page_size) != 0)
		goto out;

	map = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED, mfd, 0);
	if (map == MAP_FAILED)
		goto out;

	snprintf(map, page_size, "%s", data);
	munmap(map, page_size);

	arg.fd = mfd;
	arg.token = token;
	if (ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, &arg) < 0)
		goto out;

	ret = 0; /* Success */
out:
	if (ret != 0 && errno != 0)
		ret = -errno;
	if (mfd >= 0)
		close(mfd);
	return ret;
}

int restore_and_verify_memfd(int session_fd, int token,
			     const char *expected_data)
{
	struct liveupdate_session_restore_fd arg = { .size = sizeof(arg) };
	long page_size = sysconf(_SC_PAGE_SIZE);
	void *map = MAP_FAILED;
	int mfd = -1, ret = -1;

	arg.token = token;
	if (ioctl(session_fd, LIVEUPDATE_SESSION_RESTORE_FD, &arg) < 0)
		return -errno;
	mfd = arg.fd;

	map = mmap(NULL, page_size, PROT_READ, MAP_SHARED, mfd, 0);
	if (map == MAP_FAILED)
		goto out;

	if (expected_data && strcmp(expected_data, map) != 0) {
		ksft_print_msg("Data mismatch for token %d!\n", token);
		ret = -EINVAL;
		goto out_munmap;
	}

	ret = mfd; /* Success, return the new fd */
out_munmap:
	munmap(map, page_size);
out:
	if (ret < 0 && errno != 0)
		ret = -errno;
	if (ret < 0 && mfd >= 0)
		close(mfd);
	return ret;
}

int luo_set_session_event(int session_fd, enum liveupdate_event event)
{
	struct liveupdate_session_set_event arg = { .size = sizeof(arg) };

	arg.event = event;
	return ioctl(session_fd, LIVEUPDATE_SESSION_SET_EVENT, &arg);
}

int luo_set_global_event(int luo_fd, enum liveupdate_event event)
{
	struct liveupdate_ioctl_set_event arg = { .size = sizeof(arg) };

	arg.event = event;
	return ioctl(luo_fd, LIVEUPDATE_IOCTL_SET_EVENT, &arg);
}

int luo_get_global_state(int luo_fd, enum liveupdate_state *state)
{
	struct liveupdate_ioctl_get_state arg = { .size = sizeof(arg) };

	if (ioctl(luo_fd, LIVEUPDATE_IOCTL_GET_STATE, &arg) < 0)
		return -errno;
	*state = arg.state;
	return 0;
}

void create_state_file(int luo_fd, int next_stage)
{
	char buf[32];
	int state_session_fd;

	state_session_fd = luo_create_session(luo_fd, STATE_SESSION_NAME);
	if (state_session_fd < 0)
		fail_exit("luo_create_session failed");

	snprintf(buf, sizeof(buf), "%d", next_stage);
	if (create_and_preserve_memfd(state_session_fd,
				      STATE_MEMFD_TOKEN, buf) < 0) {
		fail_exit("create_and_preserve_memfd failed");
	}
}

int restore_and_read_state(int luo_fd, int *stage)
{
	char buf[32] = {0};
	int state_session_fd, mfd;

	state_session_fd = luo_retrieve_session(luo_fd, STATE_SESSION_NAME);
	if (state_session_fd < 0)
		return state_session_fd;

	mfd = restore_and_verify_memfd(state_session_fd, STATE_MEMFD_TOKEN,
				       NULL);
	if (mfd < 0)
		fail_exit("failed to restore state memfd");

	if (read(mfd, buf, sizeof(buf) - 1) < 0)
		fail_exit("failed to read state mfd");

	*stage = atoi(buf);

	close(mfd);
	return state_session_fd;
}

void update_state_file(int session_fd, int next_stage)
{
	char buf[32];
	struct liveupdate_session_unpreserve_fd arg = { .size = sizeof(arg) };

	arg.token = STATE_MEMFD_TOKEN;
	if (ioctl(session_fd, LIVEUPDATE_SESSION_UNPRESERVE_FD, &arg) < 0)
		fail_exit("unpreserve failed");

	snprintf(buf, sizeof(buf), "%d", next_stage);
	if (create_and_preserve_memfd(session_fd, STATE_MEMFD_TOKEN, buf) < 0)
		fail_exit("create_and_preserve failed");
}

void reinit_all_sessions(struct session_info *sessions, int num)
{
	int i, j;

	for (i = 0; i < num; i++) {
		snprintf(sessions[i].name, sizeof(sessions[i].name),
			 "session-%c", 'A' + i);
		for (j = 0; j < 2; j++) {
			sessions[i].file_tokens[j] = (i * 100) + j;
			snprintf(sessions[i].file_data[j],
				 sizeof(sessions[i].file_data[j]),
				 "Data for %.*s-File%d",
				 LIVEUPDATE_SESSION_NAME_LENGTH,
				 sessions[i].name, j);
		}
	}
}

int verify_session_and_get_fd(int luo_fd, struct session_info *s)
{
	int i, session_fd;

	ksft_print_msg("  - Verifying session '%s'...\n", s->name);

	session_fd = luo_retrieve_session(luo_fd, s->name);
	if (session_fd < 0)
		fail_exit("luo_retrieve_session for %s", s->name);

	for (i = 0; i < 2; i++) {
		int mfd = restore_and_verify_memfd(session_fd,
						   s->file_tokens[i],
						   s->file_data[i]);
		if (mfd < 0) {
			fail_exit("restore_and_verify_memfd for token %d",
				  s->file_tokens[i]);
		}
		close(mfd);
	}
	ksft_print_msg("    Success. All files verified.\n");
	return session_fd;
}
