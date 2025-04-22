// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/liveupdate.h>

#include "../kselftest.h"
#include "../kselftest_harness.h"
#include "../../../../drivers/misc/liveupdate/luo_selftests.h"

struct subsystem_info {
	void *data_page;
	void *verify_page;
	char test_name[LUO_NAME_LENGTH];
	bool registered;
};

FIXTURE(subsystem) {
	enum liveupdate_state state;
	int fd;
	struct subsystem_info si[LUO_MAX_SUBSYSTEMS];
};

FIXTURE(state) {
	enum liveupdate_state state;
	int fd;
};

#define LUO_DEVICE	"/dev/liveupdate"
#define LUO_SYSFS_STATE	"/sys/kernel/liveupdate/state"
static size_t page_size;

const char *const luo_state_str[] = {
	[LIVEUPDATE_STATE_NORMAL]   = "normal",
	[LIVEUPDATE_STATE_PREPARED] = "prepared",
	[LIVEUPDATE_STATE_FROZEN]   = "frozen",
	[LIVEUPDATE_STATE_UPDATED]  = "updated",
};

static int run_luo_selftest_cmd(int fd, __u64 cmd_code,
				struct luo_arg_subsystem *subsys_arg)
{
	struct liveupdate_selftest k_arg;

	if (fd < 0) {
		errno = EBADF;
		return -1;
	}

	k_arg.cmd = cmd_code;
	k_arg.arg = (__u64)(unsigned long)subsys_arg;

	return ioctl(fd, LIVEUPDATE_IOCTL_SELFTESTS, &k_arg);
}

static int __register_subsystem(int fd, char *name, void *data_page)
{
	struct luo_arg_subsystem subsys_arg;

	memset(&subsys_arg, 0, sizeof(subsys_arg));
	snprintf(subsys_arg.name, LUO_NAME_LENGTH, "%s", name);
	subsys_arg.data_page = data_page;

	return run_luo_selftest_cmd(fd, LUO_CMD_SUBSYSTEM_REGISTER,
				    &subsys_arg);
}

static int __unregister_subsystem(int fd, char *name)
{
	struct luo_arg_subsystem subsys_arg;

	memset(&subsys_arg, 0, sizeof(subsys_arg));
	snprintf(subsys_arg.name, LUO_NAME_LENGTH, "%s", name);

	return run_luo_selftest_cmd(fd, LUO_CMD_SUBSYSTEM_UNREGISTER,
				    &subsys_arg);
}

static int get_sysfs_state(void)
{
	char buf[64];
	ssize_t len;
	int fd, i;

	fd = open(LUO_SYSFS_STATE, O_RDONLY);
	if (fd < 0) {
		ksft_print_msg("Failed to open sysfs state file '%s': %s\n",
			       LUO_SYSFS_STATE, strerror(errno));
		return -errno;
	}

	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	if (len <= 0) {
		ksft_print_msg("Failed to read sysfs state file '%s': %s\n",
			       LUO_SYSFS_STATE, strerror(errno));
		return -errno;
	}
	if (buf[len - 1] == '\n')
		buf[len - 1] = '\0';
	else
		buf[len] = '\0';

	for (i = 0; i < ARRAY_SIZE(luo_state_str); i++) {
		if (!strcmp(buf, luo_state_str[i]))
			return i;
	}

	return -EIO;
}

FIXTURE_SETUP(state)
{
	page_size = sysconf(_SC_PAGE_SIZE);
	self->fd = open(LUO_DEVICE, O_RDWR);
	if (self->fd < 0) {
		ksft_exit_skip("Setup: Cannot open %s (errno %d).\n",
			       LUO_DEVICE, errno);
	}
	self->state = LIVEUPDATE_STATE_NORMAL;
}

FIXTURE_TEARDOWN(state)
{
	page_size = sysconf(_SC_PAGE_SIZE);
	if (self->state != LIVEUPDATE_STATE_NORMAL)
		ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_CANCEL, NULL);
	close(self->fd);
}

FIXTURE_SETUP(subsystem)
{
	int i;

	page_size = sysconf(_SC_PAGE_SIZE);
	memset(&self->si, 0, sizeof(self->si));
	self->fd = open(LUO_DEVICE, O_RDWR);
	if (self->fd < 0) {
		ksft_exit_skip("Setup: Cannot open %s (errno %d).\n",
			       LUO_DEVICE, errno);
	}
	self->state = LIVEUPDATE_STATE_NORMAL;

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		snprintf(self->si[i].test_name, LUO_NAME_LENGTH,
			 "ksft_luo_%d.%d", getpid(), i);

		self->si[i].data_page = mmap(NULL, page_size,
					     PROT_READ | PROT_WRITE,
					     MAP_PRIVATE | MAP_ANONYMOUS,
					     -1, 0);

		if (self->si[i].data_page == MAP_FAILED) {
			ksft_print_msg("Setup: mmap data_page failed\n");
			goto exit_fail;
		}
		memset(self->si[i].data_page, 'A' + i, page_size);

		self->si[i].verify_page = mmap(NULL, page_size,
					       PROT_READ | PROT_WRITE,
					       MAP_PRIVATE | MAP_ANONYMOUS,
					       -1, 0);
		if (self->si[i].verify_page == MAP_FAILED) {
			ksft_print_msg("Setup: mmap verify_page failed\n");
			goto exit_fail;
		}
		memset(self->si[i].verify_page, 0, page_size);
	}

	return;
exit_fail:
	close(self->fd);

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		void *page;

		page = self->si[i].data_page;
		if (page && page != MAP_FAILED)
			munmap(page, page_size);

		page = self->si[i].verify_page;
		if (page && page != MAP_FAILED)
			munmap(page, page_size);
	}
	ksft_exit_fail();
}

FIXTURE_TEARDOWN(subsystem)
{
	int i;

	if (self->state != LIVEUPDATE_STATE_NORMAL)
		ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_CANCEL, NULL);

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		if (self->si[i].registered) {
			struct luo_arg_subsystem subsys_arg;

			memset(&subsys_arg, 0, sizeof(subsys_arg));
			snprintf(subsys_arg.name, LUO_NAME_LENGTH, "%s",
				 self->si[i].test_name);
			subsys_arg.data_page = NULL;
			run_luo_selftest_cmd(self->fd, LUO_CMD_SUBSYSTEM_UNREGISTER,
					     &subsys_arg);
		}
		munmap(self->si[i].data_page, page_size);
		munmap(self->si[i].verify_page, page_size);
	}

	close(self->fd);
}

TEST_F(state, normal)
{
	enum liveupdate_state state;
	int ret;

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &state);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(state, LIVEUPDATE_STATE_NORMAL);
}

TEST_F(state, prepared)
{
	enum liveupdate_state state;
	int ret;

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_PREPARE, NULL);
	ASSERT_EQ(0, ret);
	self->state = LIVEUPDATE_STATE_PREPARED;

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &state);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(state, LIVEUPDATE_STATE_PREPARED);

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_CANCEL, NULL);
	ASSERT_EQ(0, ret);
	self->state = LIVEUPDATE_STATE_NORMAL;

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &state);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(state, LIVEUPDATE_STATE_NORMAL);
}

TEST_F(state, sysfs_normal)
{
	int state = get_sysfs_state();

	if (state < 0) {
		if (state == -ENOENT || state == -EACCES) {
			ksft_test_result_skip("Sysfs state file not accessible (%d)\n",
					      state);
			return;
		}
	}

	ASSERT_EQ(LIVEUPDATE_STATE_NORMAL, state);
}

TEST_F(state, sysfs_prepared)
{
	int ret, state;

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_PREPARE, NULL);
	ASSERT_EQ(0, ret);
	self->state = LIVEUPDATE_STATE_PREPARED;

	state = get_sysfs_state();
	if (state < 0) {
		if (state == -ENOENT || state == -EACCES) {
			ksft_test_result_skip("Sysfs state file not accessible (%d)\n",
					      state);
			ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_CANCEL, NULL);
			self->state = LIVEUPDATE_STATE_NORMAL;
			return;
		}
	}
	ASSERT_EQ(LIVEUPDATE_STATE_PREPARED, state);

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_CANCEL, NULL);
	ASSERT_EQ(0, ret);
	self->state = LIVEUPDATE_STATE_NORMAL;
	state = get_sysfs_state();
	ASSERT_EQ(LIVEUPDATE_STATE_NORMAL, state);
}

TEST_F(state, sysfs_frozen)
{
	int ret, state;

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_PREPARE, NULL);
	ASSERT_EQ(0, ret);
	self->state = LIVEUPDATE_STATE_PREPARED;

	state = get_sysfs_state();
	if (state < 0) {
		if (state == -ENOENT || state == -EACCES) {
			ksft_test_result_skip("Sysfs state file not accessible (%d)\n", state);
			ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_CANCEL, NULL);
			self->state = LIVEUPDATE_STATE_NORMAL;
			return;
		}
	}
	ASSERT_EQ(LIVEUPDATE_STATE_PREPARED, state);

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_FREEZE, NULL);
	ASSERT_EQ(0, ret);
	self->state = LIVEUPDATE_STATE_FROZEN;
	state = get_sysfs_state();
	ASSERT_EQ(LIVEUPDATE_STATE_FROZEN, state);

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_CANCEL, NULL);
	ASSERT_EQ(0, ret);
	self->state = LIVEUPDATE_STATE_NORMAL;
	state = get_sysfs_state();
	ASSERT_EQ(LIVEUPDATE_STATE_NORMAL, state);
}

TEST_F(subsystem, register_unregister)
{
	int ret;

	ret = __register_subsystem(self->fd, self->si[0].test_name,
				   self->si[0].data_page);
	ASSERT_EQ(0, ret);
	self->si[0].registered = true;

	ret = __unregister_subsystem(self->fd, self->si[0].test_name);
	ASSERT_EQ(0, ret);
	self->si[0].registered = false;
}

TEST_F(subsystem, double_unregister)
{
	int ret;

	ret = __register_subsystem(self->fd, self->si[0].test_name,
				   self->si[0].data_page);
	ASSERT_EQ(0, ret);
	self->si[0].registered = true;

	ret = __unregister_subsystem(self->fd, self->si[0].test_name);
	ASSERT_EQ(0, ret);
	self->si[0].registered = false;

	ret = __unregister_subsystem(self->fd, self->si[0].test_name);
	EXPECT_NE(0, ret);
	EXPECT_TRUE(errno == EINVAL || errno == ENOENT);
	self->si[0].registered = false;
}

TEST_F(subsystem, register_unregister_many)
{
	int ret;
	int i;

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		ret = __register_subsystem(self->fd, self->si[i].test_name,
					   self->si[i].data_page);
		ASSERT_EQ(0, ret);
		self->si[i].registered = true;
	}

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		ret = __unregister_subsystem(self->fd, self->si[i].test_name);
		ASSERT_EQ(0, ret);
		self->si[i].registered = false;
	}

}

TEST_F(subsystem, getdata_verify)
{
	enum liveupdate_state state;
	int ret;
	int i;

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		ret = __register_subsystem(self->fd, self->si[i].test_name,
					   self->si[i].data_page);
		ASSERT_EQ(0, ret);
		self->si[i].registered = true;
	}

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_PREPARE, NULL);
	ASSERT_EQ(0, ret);
	self->state = LIVEUPDATE_STATE_PREPARED;

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &state);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(state, LIVEUPDATE_STATE_PREPARED);

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		struct luo_arg_subsystem subsys_arg;

		memset(&subsys_arg, 0, sizeof(subsys_arg));
		snprintf(subsys_arg.name, LUO_NAME_LENGTH, "%s",
			 self->si[i].test_name);
		subsys_arg.data_page = self->si[i].verify_page;

		ret = run_luo_selftest_cmd(self->fd, LUO_CMD_SUBSYSTEM_GETDATA,
					   &subsys_arg);

		ASSERT_EQ(0, ret);
		ASSERT_EQ(0, memcmp(self->si[i].data_page,
				    self->si[i].verify_page,
				    page_size));
	}

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_EVENT_CANCEL, NULL);
	ASSERT_EQ(0, ret);
	self->state = LIVEUPDATE_STATE_NORMAL;

	ret = ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &state);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(state, LIVEUPDATE_STATE_NORMAL);

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		ret = __unregister_subsystem(self->fd, self->si[i].test_name);
		ASSERT_EQ(0, ret);
		self->si[i].registered = false;
	}
}

TEST_HARNESS_MAIN
