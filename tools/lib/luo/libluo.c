// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Author: Pratyush Yadav <ptyadav@amazon.de>
 */
#include <libluo.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * The liveupdate header is not mainline right now, so it is not available on
 * the system include path. It is copied from Linux tree and put in include/.
 *
 * This can be removed when liveupdate hits mainline.
 */
#include <liveupdate.h>

#define LUO_DEVICE_PATH	"/dev/liveupdate"

/* File descriptor for the LUO device */
static int luo_fd = -1;

#define ARRAY_SIZE(arr)	(sizeof(arr) / sizeof((arr)[0]))

int luo_init(void)
{
	if (luo_fd >= 0)
		/* Already initialized */
		return 0;

	luo_fd = open(LUO_DEVICE_PATH, O_RDWR);
	if (luo_fd < 0) {
		int err = -errno;

		fprintf(stderr, "Failed to open %s: %s\n",
			LUO_DEVICE_PATH, strerror(errno));
		return err;
	}

	return 0;
}

void luo_cleanup(void)
{
	if (luo_fd >= 0) {
		close(luo_fd);
		luo_fd = -1;
	}
}

bool luo_is_available(void)
{
	struct stat st;

	/* Use stat() to check if the device file exists and is accessible */
	if (stat(LUO_DEVICE_PATH, &st) < 0)
		return false;

	/* Verify it's a character device file.  */
	if (!S_ISCHR(st.st_mode))
		return false;

	return true;
}

int luo_get_state(enum liveupdate_state *state)
{
	int ret;

	if (!state)
		return -EINVAL;

	if (luo_fd < 0)
		return -EBADF;

	ret = ioctl(luo_fd, LIVEUPDATE_IOCTL_GET_STATE, state);
	if (ret < 0)
		return -errno;

	return 0;
}

int luo_fd_preserve(int fd, uint64_t token)
{
	struct liveupdate_fd fd_data;
	int ret;

	if (fd < 0)
		return -EINVAL;

	if (luo_fd < 0)
		return -EBADF;

	fd_data.fd = fd;
	fd_data.flags = 0;  /* Must be set to 0 as per API documentation */
	fd_data.token = token;

	ret = ioctl(luo_fd, LIVEUPDATE_IOCTL_FD_PRESERVE, &fd_data);
	if (ret < 0)
		return -errno;

	return 0;
}

int luo_fd_unpreserve(uint64_t token)
{
	int ret;

	if (luo_fd < 0)
		return -EBADF;

	ret = ioctl(luo_fd, LIVEUPDATE_IOCTL_FD_UNPRESERVE, &token);
	if (ret < 0)
		return -errno;

	return 0;
}

int luo_fd_restore(uint64_t token, int *fd)
{
	struct liveupdate_fd fd_data;
	int ret;

	if (!fd)
		return -EINVAL;

	if (luo_fd < 0)
		return -EBADF;

	fd_data.fd = -1;    /* Will be filled by the kernel */
	fd_data.flags = 0;  /* Must be set to 0 as per API documentation */
	fd_data.token = token;

	ret = ioctl(luo_fd, LIVEUPDATE_IOCTL_FD_RESTORE, &fd_data);
	if (ret < 0)
		return -errno;

	*fd = fd_data.fd;
	return 0;
}

int luo_prepare(void)
{
	int ret;

	if (luo_fd < 0)
		return -EBADF;

	ret = ioctl(luo_fd, LIVEUPDATE_IOCTL_PREPARE);
	if (ret < 0)
		return -errno;

	return 0;
}

int luo_cancel(void)
{
	int ret;

	if (luo_fd < 0)
		return -EBADF;

	ret = ioctl(luo_fd, LIVEUPDATE_IOCTL_CANCEL);
	if (ret < 0)
		return -errno;

	return 0;
}

int luo_finish(void)
{
	int ret;

	if (luo_fd < 0)
		return -EBADF;

	ret = ioctl(luo_fd, LIVEUPDATE_IOCTL_FINISH);
	if (ret < 0)
		return -errno;

	return 0;
}

const char *luo_state_to_string(enum liveupdate_state state)
{
	static const char * const state_strings[] = {
		[LIVEUPDATE_STATE_UNDEFINED] = "undefined",
		[LIVEUPDATE_STATE_NORMAL] = "normal",
		[LIVEUPDATE_STATE_PREPARED] = "prepared",
		[LIVEUPDATE_STATE_FROZEN] = "frozen",
		[LIVEUPDATE_STATE_UPDATED] = "updated"
	};

	if (state >= 0 && state < ARRAY_SIZE(state_strings) && state_strings[state])
		return state_strings[state];

	return "UNKNOWN";
}
