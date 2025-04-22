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
#include "../../../../kernel/liveupdate/luo_selftests.h"

struct subsystem_info {
	void *data_page;
	void *verify_page;
	char test_name[LUO_NAME_LENGTH];
	bool registered;
};

FIXTURE(subsystem) {
	int fd;
	int fd_dbg;
	struct subsystem_info si[LUO_MAX_SUBSYSTEMS];
};

FIXTURE(state) {
	int fd;
	int fd_dbg;
};

#define LUO_DEVICE	"/dev/liveupdate"
#define LUO_DBG_DEVICE	"/sys/kernel/debug/liveupdate/luo_selftest"
static size_t page_size;

const char *const luo_state_str[] = {
	[LIVEUPDATE_STATE_UNDEFINED]   = "undefined",
	[LIVEUPDATE_STATE_NORMAL]   = "normal",
	[LIVEUPDATE_STATE_PREPARED] = "prepared",
	[LIVEUPDATE_STATE_FROZEN]   = "frozen",
	[LIVEUPDATE_STATE_UPDATED]  = "updated",
};

static int run_luo_selftest_cmd(int fd_dbg, __u64 cmd_code,
				struct luo_arg_subsystem *subsys_arg)
{
	struct liveupdate_selftest k_arg;

	k_arg.cmd = cmd_code;
	k_arg.arg = (__u64)(unsigned long)subsys_arg;

	return ioctl(fd_dbg, LIVEUPDATE_IOCTL_SELFTESTS, &k_arg);
}

static int register_subsystem(int fd_dbg, struct subsystem_info *si)
{
	struct luo_arg_subsystem subsys_arg;
	int ret;

	memset(&subsys_arg, 0, sizeof(subsys_arg));
	snprintf(subsys_arg.name, LUO_NAME_LENGTH, "%s", si->test_name);
	subsys_arg.data_page = si->data_page;

	ret = run_luo_selftest_cmd(fd_dbg, LUO_CMD_SUBSYSTEM_REGISTER,
				   &subsys_arg);
	if (!ret)
		si->registered = true;

	return ret;
}

static int unregister_subsystem(int fd_dbg, struct subsystem_info *si)
{
	struct luo_arg_subsystem subsys_arg;
	int ret;

	memset(&subsys_arg, 0, sizeof(subsys_arg));
	snprintf(subsys_arg.name, LUO_NAME_LENGTH, "%s", si->test_name);

	ret = run_luo_selftest_cmd(fd_dbg, LUO_CMD_SUBSYSTEM_UNREGISTER,
				   &subsys_arg);
	if (!ret)
		si->registered = false;

	return ret;
}

FIXTURE_SETUP(state)
{
	page_size = sysconf(_SC_PAGE_SIZE);
	self->fd = open(LUO_DEVICE, O_RDWR);
	if (self->fd < 0)
		SKIP(return, "open(%s) failed [%d]", LUO_DEVICE, errno);

	self->fd_dbg = open(LUO_DBG_DEVICE, O_RDWR);
	ASSERT_GE(self->fd_dbg, 0);
}

FIXTURE_TEARDOWN(state)
{
	struct liveupdate_ioctl_set_event cancel = {
		.size = sizeof(cancel),
		.event = LIVEUPDATE_CANCEL,
	};
	struct liveupdate_ioctl_get_state ligs = {.size = sizeof(ligs)};

	ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &ligs);
	if (ligs.state != LIVEUPDATE_STATE_NORMAL)
		ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &cancel);
	close(self->fd);
}

FIXTURE_SETUP(subsystem)
{
	int i;

	page_size = sysconf(_SC_PAGE_SIZE);
	memset(&self->si, 0, sizeof(self->si));
	self->fd = open(LUO_DEVICE, O_RDWR);
	if (self->fd < 0)
		SKIP(return, "open(%s) failed [%d]", LUO_DEVICE, errno);

	self->fd_dbg = open(LUO_DBG_DEVICE, O_RDWR);
	ASSERT_GE(self->fd_dbg, 0);

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		snprintf(self->si[i].test_name, LUO_NAME_LENGTH,
			 NAME_NORMAL ".%d", i);

		self->si[i].data_page = mmap(NULL, page_size,
					     PROT_READ | PROT_WRITE,
					     MAP_PRIVATE | MAP_ANONYMOUS,
					     -1, 0);
		ASSERT_NE(MAP_FAILED, self->si[i].data_page);
		memset(self->si[i].data_page, 'A' + i, page_size);

		self->si[i].verify_page = mmap(NULL, page_size,
					       PROT_READ | PROT_WRITE,
					       MAP_PRIVATE | MAP_ANONYMOUS,
					       -1, 0);
		ASSERT_NE(MAP_FAILED, self->si[i].verify_page);
		memset(self->si[i].verify_page, 0, page_size);
	}
}

FIXTURE_TEARDOWN(subsystem)
{
	struct liveupdate_ioctl_set_event cancel = {
		.size = sizeof(cancel),
		.event = LIVEUPDATE_CANCEL,
	};
	enum liveupdate_state state = LIVEUPDATE_STATE_NORMAL;
	int i;

	ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &state);
	if (state != LIVEUPDATE_STATE_NORMAL)
		ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &cancel);

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		if (self->si[i].registered)
			unregister_subsystem(self->fd_dbg, &self->si[i]);
		munmap(self->si[i].data_page, page_size);
		munmap(self->si[i].verify_page, page_size);
	}

	close(self->fd);
}

TEST_F(state, normal)
{
	struct liveupdate_ioctl_get_state ligs = {.size = sizeof(ligs)};

	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &ligs));
	ASSERT_EQ(ligs.state, LIVEUPDATE_STATE_NORMAL);
}

TEST_F(state, prepared)
{
	struct liveupdate_ioctl_get_state ligs = {.size = sizeof(ligs)};
	struct liveupdate_ioctl_set_event prepare = {
		.size = sizeof(prepare),
		.event = LIVEUPDATE_PREPARE,
	};
	struct liveupdate_ioctl_set_event cancel = {
		.size = sizeof(cancel),
		.event = LIVEUPDATE_CANCEL,
	};

	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &prepare));

	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &ligs));
	ASSERT_EQ(ligs.state, LIVEUPDATE_STATE_PREPARED);

	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &cancel));

	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &ligs));
	ASSERT_EQ(ligs.state, LIVEUPDATE_STATE_NORMAL);
}

TEST_F(state, sysfs_prepared)
{
	struct liveupdate_ioctl_set_event prepare = {
		.size = sizeof(prepare),
		.event = LIVEUPDATE_PREPARE,
	};
	struct liveupdate_ioctl_set_event cancel = {
		.size = sizeof(cancel),
		.event = LIVEUPDATE_CANCEL,
	};

	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &prepare));
	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &cancel));
}

TEST_F(state, sysfs_frozen)
{
	struct liveupdate_ioctl_set_event prepare = {
		.size = sizeof(prepare),
		.event = LIVEUPDATE_PREPARE,
	};
	struct liveupdate_ioctl_set_event cancel = {
		.size = sizeof(cancel),
		.event = LIVEUPDATE_CANCEL,
	};

	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &prepare));
	ASSERT_EQ(0, ioctl(self->fd_dbg, LIVEUPDATE_IOCTL_FREEZE, NULL));
	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &cancel));
}

TEST_F(subsystem, register_unregister)
{
	ASSERT_EQ(0, register_subsystem(self->fd_dbg, &self->si[0]));
	ASSERT_EQ(0, unregister_subsystem(self->fd_dbg, &self->si[0]));
}

TEST_F(subsystem, double_unregister)
{
	ASSERT_EQ(0, register_subsystem(self->fd_dbg, &self->si[0]));
	ASSERT_EQ(0, unregister_subsystem(self->fd_dbg, &self->si[0]));
	EXPECT_NE(0, unregister_subsystem(self->fd_dbg, &self->si[0]));
	EXPECT_TRUE(errno == EINVAL || errno == ENOENT);
}

TEST_F(subsystem, register_unregister_many)
{
	int i;

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++)
		ASSERT_EQ(0, register_subsystem(self->fd_dbg, &self->si[i]));

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++)
		ASSERT_EQ(0, unregister_subsystem(self->fd_dbg, &self->si[i]));
}

TEST_F(subsystem, getdata_verify)
{
	struct liveupdate_ioctl_get_state ligs = {.size = sizeof(ligs), .state = 0};
	struct liveupdate_ioctl_set_event prepare = {
		.size = sizeof(prepare),
		.event = LIVEUPDATE_PREPARE,
	};
	struct liveupdate_ioctl_set_event cancel = {
		.size = sizeof(cancel),
		.event = LIVEUPDATE_CANCEL,
	};
	int i;

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++)
		ASSERT_EQ(0, register_subsystem(self->fd_dbg, &self->si[i]));

	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &prepare));
	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &ligs));
	ASSERT_EQ(ligs.state, LIVEUPDATE_STATE_PREPARED);

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		struct luo_arg_subsystem subsys_arg;

		memset(&subsys_arg, 0, sizeof(subsys_arg));
		snprintf(subsys_arg.name, LUO_NAME_LENGTH, "%s",
			 self->si[i].test_name);
		subsys_arg.data_page = self->si[i].verify_page;

		ASSERT_EQ(0, run_luo_selftest_cmd(self->fd_dbg,
						  LUO_CMD_SUBSYSTEM_GETDATA,
						  &subsys_arg));
		ASSERT_EQ(0, memcmp(self->si[i].data_page,
				    self->si[i].verify_page,
				    page_size));
	}

	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &cancel));
	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_GET_STATE, &ligs));
	ASSERT_EQ(ligs.state, LIVEUPDATE_STATE_NORMAL);

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++)
		ASSERT_EQ(0, unregister_subsystem(self->fd_dbg, &self->si[i]));
}

TEST_F(subsystem, prepare_fail)
{
	struct liveupdate_ioctl_set_event prepare = {
		.size = sizeof(prepare),
		.event = LIVEUPDATE_PREPARE,
	};
	struct liveupdate_ioctl_set_event cancel = {
		.size = sizeof(cancel),
		.event = LIVEUPDATE_CANCEL,
	};
	int i;

	snprintf(self->si[LUO_MAX_SUBSYSTEMS - 1].test_name, LUO_NAME_LENGTH,
		 NAME_PREPARE_FAIL ".%d", LUO_MAX_SUBSYSTEMS - 1);

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++)
		ASSERT_EQ(0, register_subsystem(self->fd_dbg, &self->si[i]));

	ASSERT_EQ(-1, ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &prepare));

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++)
		ASSERT_EQ(0, unregister_subsystem(self->fd_dbg, &self->si[i]));

	snprintf(self->si[LUO_MAX_SUBSYSTEMS - 1].test_name, LUO_NAME_LENGTH,
		 NAME_NORMAL ".%d", LUO_MAX_SUBSYSTEMS - 1);

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++)
		ASSERT_EQ(0, register_subsystem(self->fd_dbg, &self->si[i]));

	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &prepare));
	ASSERT_EQ(0, ioctl(self->fd_dbg, LIVEUPDATE_IOCTL_FREEZE, NULL));
	ASSERT_EQ(0, ioctl(self->fd, LIVEUPDATE_IOCTL_SET_EVENT, &cancel));

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++)
		ASSERT_EQ(0, unregister_subsystem(self->fd_dbg, &self->si[i]));
}

TEST_HARNESS_MAIN
