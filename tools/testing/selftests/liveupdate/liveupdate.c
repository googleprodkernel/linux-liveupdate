// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/*
 * Selftests for the Live Update Orchestrator.
 * This test suite verifies the functionality and behavior of the
 * /dev/liveupdate character device and its session management capabilities.
 *
 * Tests include:
 * - Device access: basic open/close, and enforcement of exclusive access.
 * - Session management: creation of unique sessions, and duplicate name detection.
 * - Resource preservation: successfully preserving individual and multiple memfds,
 *   verifying contents remain accessible.
 * - Complex multi-session scenarios involving mixed empty and populated files.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libliveupdate.h>
#include <linux/liveupdate.h>

#include "../kselftest.h"
#include "../kselftest_harness.h"

#define LIVEUPDATE_DEV "/dev/liveupdate"

FIXTURE(liveupdate_device) {
	int fd1;
	int fd2;
};

FIXTURE_SETUP(liveupdate_device)
{
	self->fd1 = -1;
	self->fd2 = -1;
}

FIXTURE_TEARDOWN(liveupdate_device)
{
	if (self->fd1 >= 0)
		close(self->fd1);
	if (self->fd2 >= 0)
		close(self->fd2);
}

/*
 * Test Case: Basic Open and Close
 *
 * Verifies that the /dev/liveupdate device can be opened and subsequently
 * closed without errors. Skips if the device does not exist.
 */
TEST_F(liveupdate_device, basic_open_close)
{
	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);

	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist.", LIVEUPDATE_DEV);

	ASSERT_GE(self->fd1, 0);
	ASSERT_EQ(close(self->fd1), 0);
	self->fd1 = -1;
}

/*
 * Test Case: Exclusive Open Enforcement
 *
 * Verifies that the /dev/liveupdate device can only be opened by one process
 * at a time. It checks that a second attempt to open the device fails with
 * the EBUSY error code.
 */
TEST_F(liveupdate_device, exclusive_open)
{
	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);

	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist.", LIVEUPDATE_DEV);

	ASSERT_GE(self->fd1, 0);
	self->fd2 = open(LIVEUPDATE_DEV, O_RDWR);
	EXPECT_LT(self->fd2, 0);
	EXPECT_EQ(errno, EBUSY);
}

/*
 * Test Case: Create Duplicate Session
 *
 * Verifies that attempting to create two sessions with the same name fails
 * on the second attempt with EEXIST.
 */
TEST_F(liveupdate_device, create_duplicate_session)
{
	int session_fd1, session_fd2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);

	ASSERT_GE(self->fd1, 0);

	session_fd1 = luo_create_session(self->fd1, "duplicate-session-test");
	ASSERT_GE(session_fd1, 0);

	session_fd2 = luo_create_session(self->fd1, "duplicate-session-test");
	EXPECT_LT(session_fd2, 0);
	EXPECT_EQ(-session_fd2, EEXIST);

	ASSERT_EQ(close(session_fd1), 0);
}

/*
 * Test Case: Create Distinct Sessions
 *
 * Verifies that creating two sessions with different names succeeds.
 */
TEST_F(liveupdate_device, create_distinct_sessions)
{
	int session_fd1, session_fd2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);

	ASSERT_GE(self->fd1, 0);

	session_fd1 = luo_create_session(self->fd1, "distinct-session-1");
	ASSERT_GE(session_fd1, 0);

	session_fd2 = luo_create_session(self->fd1, "distinct-session-2");
	ASSERT_GE(session_fd2, 0);

	ASSERT_EQ(close(session_fd1), 0);
	ASSERT_EQ(close(session_fd2), 0);
}

/*
 * Test Case: Preserve MemFD
 *
 * Verifies that a valid memfd can be successfully preserved in a session and
 * that its contents remain intact after the preservation call.
 */
TEST_F(liveupdate_device, preserve_memfd)
{
	const char *test_str = "hello liveupdate";
	char read_buf[64] = {};
	int session_fd, mem_fd;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "preserve-memfd-test");
	ASSERT_GE(session_fd, 0);

	mem_fd = memfd_create("test-memfd", 0);
	ASSERT_GE(mem_fd, 0);

	ASSERT_EQ(write(mem_fd, test_str, strlen(test_str)), strlen(test_str));
	ASSERT_EQ(luo_session_preserve_fd(session_fd, mem_fd, 0x1234), 0);
	ASSERT_EQ(close(session_fd), 0);

	ASSERT_EQ(lseek(mem_fd, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd, read_buf, sizeof(read_buf)), strlen(test_str));
	ASSERT_STREQ(read_buf, test_str);
	ASSERT_EQ(close(mem_fd), 0);
}

/*
 * Test Case: Preserve Multiple MemFDs
 *
 * Verifies that multiple memfds can be preserved in a single session,
 * each with a unique token, and that their contents remain distinct and
 * correct after preservation.
 */
TEST_F(liveupdate_device, preserve_multiple_memfds)
{
	const char *test_str1 = "data for memfd one";
	const char *test_str2 = "data for memfd two";
	char read_buf[64] = {};
	int session_fd, mem_fd1, mem_fd2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "preserve-multi-memfd-test");
	ASSERT_GE(session_fd, 0);

	mem_fd1 = memfd_create("test-memfd-1", 0);
	ASSERT_GE(mem_fd1, 0);
	mem_fd2 = memfd_create("test-memfd-2", 0);
	ASSERT_GE(mem_fd2, 0);

	ASSERT_EQ(write(mem_fd1, test_str1, strlen(test_str1)), strlen(test_str1));
	ASSERT_EQ(write(mem_fd2, test_str2, strlen(test_str2)), strlen(test_str2));

	ASSERT_EQ(luo_session_preserve_fd(session_fd, mem_fd1, 0xAAAA), 0);
	ASSERT_EQ(luo_session_preserve_fd(session_fd, mem_fd2, 0xBBBB), 0);

	memset(read_buf, 0, sizeof(read_buf));
	ASSERT_EQ(lseek(mem_fd1, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd1, read_buf, sizeof(read_buf)), strlen(test_str1));
	ASSERT_STREQ(read_buf, test_str1);

	memset(read_buf, 0, sizeof(read_buf));
	ASSERT_EQ(lseek(mem_fd2, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd2, read_buf, sizeof(read_buf)), strlen(test_str2));
	ASSERT_STREQ(read_buf, test_str2);

	ASSERT_EQ(close(mem_fd1), 0);
	ASSERT_EQ(close(mem_fd2), 0);
	ASSERT_EQ(close(session_fd), 0);
}

/*
 * Test Case: Preserve Complex Scenario
 *
 * Verifies a more complex scenario with multiple sessions and a mix of empty
 * and non-empty memfds distributed across them.
 */
TEST_F(liveupdate_device, preserve_complex_scenario)
{
	const char *data1 = "data for session 1";
	const char *data2 = "data for session 2";
	char read_buf[64] = {};
	int session_fd1, session_fd2;
	int mem_fd_data1, mem_fd_empty1, mem_fd_data2, mem_fd_empty2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd1 = luo_create_session(self->fd1, "complex-session-1");
	ASSERT_GE(session_fd1, 0);
	session_fd2 = luo_create_session(self->fd1, "complex-session-2");
	ASSERT_GE(session_fd2, 0);

	mem_fd_data1 = memfd_create("data1", 0);
	ASSERT_GE(mem_fd_data1, 0);
	ASSERT_EQ(write(mem_fd_data1, data1, strlen(data1)), strlen(data1));

	mem_fd_empty1 = memfd_create("empty1", 0);
	ASSERT_GE(mem_fd_empty1, 0);

	mem_fd_data2 = memfd_create("data2", 0);
	ASSERT_GE(mem_fd_data2, 0);
	ASSERT_EQ(write(mem_fd_data2, data2, strlen(data2)), strlen(data2));

	mem_fd_empty2 = memfd_create("empty2", 0);
	ASSERT_GE(mem_fd_empty2, 0);

	ASSERT_EQ(luo_session_preserve_fd(session_fd1, mem_fd_data1, 0x1111), 0);
	ASSERT_EQ(luo_session_preserve_fd(session_fd1, mem_fd_empty1, 0x2222), 0);
	ASSERT_EQ(luo_session_preserve_fd(session_fd2, mem_fd_data2, 0x3333), 0);
	ASSERT_EQ(luo_session_preserve_fd(session_fd2, mem_fd_empty2, 0x4444), 0);

	ASSERT_EQ(lseek(mem_fd_data1, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd_data1, read_buf, sizeof(read_buf)), strlen(data1));
	ASSERT_STREQ(read_buf, data1);

	memset(read_buf, 0, sizeof(read_buf));
	ASSERT_EQ(lseek(mem_fd_data2, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd_data2, read_buf, sizeof(read_buf)), strlen(data2));
	ASSERT_STREQ(read_buf, data2);

	ASSERT_EQ(lseek(mem_fd_empty1, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd_empty1, read_buf, sizeof(read_buf)), 0);

	ASSERT_EQ(lseek(mem_fd_empty2, 0, SEEK_SET), 0);
	ASSERT_EQ(read(mem_fd_empty2, read_buf, sizeof(read_buf)), 0);

	ASSERT_EQ(close(mem_fd_data1), 0);
	ASSERT_EQ(close(mem_fd_empty1), 0);
	ASSERT_EQ(close(mem_fd_data2), 0);
	ASSERT_EQ(close(mem_fd_empty2), 0);
	ASSERT_EQ(close(session_fd1), 0);
	ASSERT_EQ(close(session_fd2), 0);
}

/*
 * Test Case: Preserve Unsupported File Descriptor
 *
 * Verifies that attempting to preserve a file descriptor that does not have
 * a registered Live Update handler fails gracefully.
 * Uses /dev/null as a representative of a file type (character device)
 * that is not supported by the orchestrator.
 */
TEST_F(liveupdate_device, preserve_unsupported_fd)
{
	int session_fd, unsupported_fd;
	int ret;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "unsupported-fd-test");
	ASSERT_GE(session_fd, 0);

	unsupported_fd = open("/dev/null", O_RDWR);
	ASSERT_GE(unsupported_fd, 0);

	ret = luo_session_preserve_fd(session_fd, unsupported_fd, 0xDEAD);
	EXPECT_EQ(ret, -ENOENT);

	ASSERT_EQ(close(unsupported_fd), 0);
	ASSERT_EQ(close(session_fd), 0);
}

/*
 * Test Case: Prevent Double Preservation
 *
 * Verifies that a file (memfd) can only be preserved once across all active
 * sessions. Attempting to preserve it a second time, whether in the same or
 * a different session, should fail with EBUSY.
 */
TEST_F(liveupdate_device, prevent_double_preservation)
{
	int session_fd1, session_fd2, mem_fd;
	int ret;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd1 = luo_create_session(self->fd1, "double-preserve-session-1");
	ASSERT_GE(session_fd1, 0);
	session_fd2 = luo_create_session(self->fd1, "double-preserve-session-2");
	ASSERT_GE(session_fd2, 0);

	mem_fd = memfd_create("test-memfd", 0);
	ASSERT_GE(mem_fd, 0);

	/* First preservation should succeed */
	ASSERT_EQ(luo_session_preserve_fd(session_fd1, mem_fd, 0x1111), 0);

	/* Second preservation in a different session should fail with EBUSY */
	ret = luo_session_preserve_fd(session_fd2, mem_fd, 0x2222);
	EXPECT_EQ(ret, -EBUSY);

	/* Second preservation in the same session (different token) should fail with EBUSY */
	ret = luo_session_preserve_fd(session_fd1, mem_fd, 0x3333);
	EXPECT_EQ(ret, -EBUSY);

	ASSERT_EQ(close(mem_fd), 0);
	ASSERT_EQ(close(session_fd1), 0);
	ASSERT_EQ(close(session_fd2), 0);
}

/*
 * Test Case: Create Session with No Null Termination
 *
 * Verifies that filling the entire 64-byte name field with non-null characters
 * (no '\0' terminator) is rejected by the kernel with EINVAL.
 */
TEST_F(liveupdate_device, create_session_no_null_termination)
{
	struct liveupdate_ioctl_create_session args = {};

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	/* Fill entire name field with 'X', no null terminator */
	args.size = sizeof(args);
	memset(args.name, 'X', sizeof(args.name));

	EXPECT_LT(ioctl(self->fd1, LIVEUPDATE_IOCTL_CREATE_SESSION, &args), 0);
	EXPECT_EQ(errno, EINVAL);
}

/*
 * Test Case: Create Session with Empty Name
 *
 * Verifies that creating a session with an empty string name fails
 * with EINVAL.
 */
TEST_F(liveupdate_device, create_session_empty_name)
{
	int session_fd;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "");
	EXPECT_EQ(session_fd, -EINVAL);
}

/*
 * Test Case: Get Session Name
 *
 * Verifies that the full session name can be retrieved from a session file
 * descriptor via ioctl.
 */
TEST_F(liveupdate_device, get_session_name)
{
	char name_buf[LIVEUPDATE_SESSION_NAME_LENGTH] = {};
	const char *session_name = "get-name-test-session";
	int session_fd;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, session_name);
	ASSERT_GE(session_fd, 0);

	ASSERT_EQ(luo_get_session_name(session_fd, name_buf, sizeof(name_buf)), 0);
	ASSERT_STREQ(name_buf, session_name);

	ASSERT_EQ(close(session_fd), 0);
}

/*
 * Test Case: Get Session Name at Maximum Length
 *
 * Verifies that a session name using the full LIVEUPDATE_SESSION_NAME_LENGTH
 * (minus the null terminator) can be correctly retrieved.
 */
TEST_F(liveupdate_device, get_session_name_max_length)
{
	char name_buf[LIVEUPDATE_SESSION_NAME_LENGTH] = {};
	char long_name[LIVEUPDATE_SESSION_NAME_LENGTH];
	int session_fd;

	memset(long_name, 'A', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, long_name);
	ASSERT_GE(session_fd, 0);

	ASSERT_EQ(luo_get_session_name(session_fd, name_buf, sizeof(name_buf)), 0);
	ASSERT_STREQ(name_buf, long_name);

	ASSERT_EQ(close(session_fd), 0);
}

/*
 * Test Case: Manage Many Sessions
 *
 * Verifies that a large number of sessions can be created and then
 * destroyed during normal system operation. This specifically tests the
 * dynamic block allocation and reuse logic for session metadata management
 * without preserving any files.
 */
TEST_F(liveupdate_device, preserve_many_sessions)
{
#define MANY_SESSIONS 2000
	int session_fds[MANY_SESSIONS];
	int ret, i;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	ret = luo_ensure_nofile_limit(MANY_SESSIONS);
	if (ret == -EPERM)
		SKIP(return, "Insufficient privileges to set RLIMIT_NOFILE");
	ASSERT_EQ(ret, 0);

	for (i = 0; i < MANY_SESSIONS; i++) {
		char name[64];

		snprintf(name, sizeof(name), "many-session-%d", i);
		session_fds[i] = luo_create_session(self->fd1, name);
		ASSERT_GE(session_fds[i], 0);
	}

	for (i = 0; i < MANY_SESSIONS; i++)
		ASSERT_EQ(close(session_fds[i]), 0);
}

/*
 * Test Case: Preserve Many Files
 *
 * Verifies that a large number of files can be preserved in a single session
 * and then destroyed during normal system operation. This tests the dynamic
 * block allocation and management for outgoing files.
 */
TEST_F(liveupdate_device, preserve_many_files)
{
#define MANY_FILES 500
	int mem_fds[MANY_FILES];
	int session_fd, ret, i;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "many-files-test");
	ASSERT_GE(session_fd, 0);

	ret = luo_ensure_nofile_limit(MANY_FILES + 10);
	if (ret == -EPERM)
		SKIP(return, "Insufficient privileges to set RLIMIT_NOFILE");
	ASSERT_EQ(ret, 0);

	for (i = 0; i < MANY_FILES; i++) {
		mem_fds[i] = memfd_create("test-memfd", 0);
		ASSERT_GE(mem_fds[i], 0);
		ASSERT_EQ(luo_session_preserve_fd(session_fd, mem_fds[i], i), 0);
	}

	for (i = 0; i < MANY_FILES; i++)
		ASSERT_EQ(close(mem_fds[i]), 0);

	ASSERT_EQ(close(session_fd), 0);
}

/*
 * Test Case: Session Freeze
 *
 * Verifies the LIVEUPDATE_SESSION_FREEZE ioctl on an outgoing session.
 */
TEST_F(liveupdate_device, session_freeze)
{
	struct liveupdate_session_freeze bad_arg = {
		.size = sizeof(bad_arg),
		.reserved = 1,
	};
	int session_fd, mem_fd1, mem_fd2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "session-freeze-test");
	ASSERT_GE(session_fd, 0);

	mem_fd1 = memfd_create("freeze-memfd-1", 0);
	ASSERT_GE(mem_fd1, 0);
	ASSERT_EQ(luo_session_preserve_fd(session_fd, mem_fd1, 0x1111), 0);

	/* Non-zero reserved field must fail with -EINVAL */
	EXPECT_EQ(ioctl(session_fd, LIVEUPDATE_SESSION_FREEZE, &bad_arg), -1);
	EXPECT_EQ(errno, EINVAL);

	/* Freeze the session early */
	ASSERT_EQ(luo_session_freeze(session_fd), 0);

	/* Subsequent freeze attempt on an already frozen session must fail with -EBUSY */
	EXPECT_EQ(luo_session_freeze(session_fd), -EBUSY);

	/* Preserving a file after freeze must fail with -EBUSY */
	mem_fd2 = memfd_create("freeze-memfd-2", 0);
	ASSERT_GE(mem_fd2, 0);
	EXPECT_EQ(luo_session_preserve_fd(session_fd, mem_fd2, 0x2222), -EBUSY);

	ASSERT_EQ(close(session_fd), 0);
	ASSERT_EQ(close(mem_fd1), 0);
	ASSERT_EQ(close(mem_fd2), 0);
}

/*
 * Test Case: Preserve MemFD with User Argument
 *
 * Verifies that passing a non-zero user argument to memfd preservation fails
 * with -EOPNOTSUPP, while passing a zero argument or using the minimum struct
 * size without the arg field succeeds.
 */
TEST_F(liveupdate_device, preserve_memfd_with_arg)
{
	struct liveupdate_session_preserve_fd min_arg = {};
	int session_fd, mem_fd1, mem_fd2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	session_fd = luo_create_session(self->fd1, "preserve-arg-test");
	ASSERT_GE(session_fd, 0);

	mem_fd1 = memfd_create("arg-memfd-1", 0);
	ASSERT_GE(mem_fd1, 0);

	/* Non-zero arg is not supported by memfd handler */
	EXPECT_EQ(luo_session_preserve_fd_arg(session_fd, mem_fd1, 0x1111, 1),
		  -EOPNOTSUPP);

	/* Legacy struct size (up to token) should zero-fill arg and succeed */
	min_arg.size = offsetof(struct liveupdate_session_preserve_fd, arg);
	min_arg.fd = mem_fd1;
	min_arg.token = 0x1111;
	ASSERT_EQ(ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, &min_arg), 0);

	/* Explicit zero arg with full struct size should also succeed */
	mem_fd2 = memfd_create("arg-memfd-2", 0);
	ASSERT_GE(mem_fd2, 0);
	ASSERT_EQ(luo_session_preserve_fd_arg(session_fd, mem_fd2, 0x2222, 0), 0);

	ASSERT_EQ(close(session_fd), 0);
	ASSERT_EQ(close(mem_fd1), 0);
	ASSERT_EQ(close(mem_fd2), 0);
}

static int find_hotplug_cpu(void)
{
	char path[128];
	int cpu;

	for (cpu = 1; cpu < 256; cpu++) {
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/preserve", cpu);
		if (access(path, R_OK) == 0) {
			snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpu);
			if (access(path, R_OK) == 0)
				return cpu;
		}
	}
	return -1;
}

static int read_cpu_online(int cpu)
{
	char path[128], buf[16];
	int fd, val;

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpu);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	memset(buf, 0, sizeof(buf));
	if (read(fd, buf, sizeof(buf) - 1) <= 0) {
		close(fd);
		return -1;
	}
	close(fd);
	val = atoi(buf);
	return val;
}

static int write_cpu_online(int cpu, int val)
{
	char path[128], buf[16];
	int fd, ret;

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpu);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;
	snprintf(buf, sizeof(buf), "%d\n", val);
	ret = write(fd, buf, strlen(buf));
	if (ret < 0) {
		ret = -errno;
		close(fd);
		return ret;
	}
	close(fd);
	return 0;
}

static int read_cpu_preserve(int cpu)
{
	char path[128], buf[16];
	int fd;

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/preserve", cpu);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	memset(buf, 0, sizeof(buf));
	if (read(fd, buf, sizeof(buf) - 1) <= 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return atoi(buf);
}

static int read_preserved_mask(char *buf, size_t sz)
{
	int fd;
	ssize_t n;

	fd = open("/sys/devices/system/cpu/preserved", O_RDONLY);
	if (fd < 0)
		return -1;
	memset(buf, 0, sz);
	n = read(fd, buf, sz - 1);
	close(fd);
	return n > 0 ? 0 : -1;
}

/*
 * Test Case: CPU Preservation Basic
 *
 * Verifies that a CPU preserve sysfs file descriptor can be preserved in a
 * LUO session, offlining the CPU from scheduler and returning it online upon
 * session teardown.
 */
TEST_F(liveupdate_device, cpu_preserve_basic)
{
	char path[128], mask[64], expected[16];
	int target_cpu;
	int session_fd, cpu_fd;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	target_cpu = find_hotplug_cpu();
	if (target_cpu < 0)
		SKIP(return, "No hotpluggable CPU with preserve attribute found");

	ASSERT_EQ(read_cpu_preserve(target_cpu), 0);

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/preserve", target_cpu);
	cpu_fd = open(path, O_RDONLY);
	ASSERT_GE(cpu_fd, 0);

	session_fd = luo_create_session(self->fd1, "cpu-preserve-test");
	ASSERT_GE(session_fd, 0);

	ASSERT_EQ(luo_session_preserve_fd(session_fd, cpu_fd, 0x42), 0);
	close(cpu_fd);

	/* Verify target CPU is now offline and marked preserved */
	ASSERT_EQ(read_cpu_online(target_cpu), 0);
	ASSERT_EQ(read_cpu_preserve(target_cpu), 1);
	ASSERT_EQ(read_preserved_mask(mask, sizeof(mask)), 0);
	snprintf(expected, sizeof(expected), "%d\n", target_cpu);
	EXPECT_STREQ(mask, expected);

	/* Closing session unpreserves and brings CPU back online */
	ASSERT_EQ(close(session_fd), 0);
	ASSERT_EQ(read_cpu_online(target_cpu), 1);
	ASSERT_EQ(read_cpu_preserve(target_cpu), 0);
	ASSERT_EQ(read_preserved_mask(mask, sizeof(mask)), 0);
	EXPECT_STREQ(mask, "\n");
}

/*
 * Test Case: CPU Preservation Freeze and Unfreeze
 *
 * Verifies that freezing a session containing a preserved CPU succeeds and
 * closing the frozen session unfreezes and unpreserves the CPU cleanly.
 */
TEST_F(liveupdate_device, cpu_preserve_freeze_unfreeze)
{
	char path[128];
	int target_cpu;
	int session_fd, cpu_fd;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	target_cpu = find_hotplug_cpu();
	if (target_cpu < 0)
		SKIP(return, "No hotpluggable CPU with preserve attribute found");

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/preserve", target_cpu);
	cpu_fd = open(path, O_RDONLY);
	ASSERT_GE(cpu_fd, 0);

	session_fd = luo_create_session(self->fd1, "cpu-preserve-freeze");
	ASSERT_GE(session_fd, 0);

	ASSERT_EQ(luo_session_preserve_fd(session_fd, cpu_fd, 0x43), 0);
	close(cpu_fd);

	ASSERT_EQ(luo_session_freeze(session_fd), 0);
	ASSERT_EQ(read_cpu_preserve(target_cpu), 1);

	ASSERT_EQ(close(session_fd), 0);
	ASSERT_EQ(read_cpu_online(target_cpu), 1);
	ASSERT_EQ(read_cpu_preserve(target_cpu), 0);
}

/*
 * Test Case: CPU Preservation Sysfs Online/Offline Rejection
 *
 * Verifies that while a CPU is preserved, attempting to bring it online
 * via sysfs fails with -EBUSY. After session closure, normal online/offline
 * hotplug operations succeed.
 */
TEST_F(liveupdate_device, cpu_preserve_sysfs_online_offline)
{
	char path[128];
	int target_cpu;
	int session_fd, cpu_fd;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	target_cpu = find_hotplug_cpu();
	if (target_cpu < 0)
		SKIP(return, "No hotpluggable CPU with preserve attribute found");

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/preserve", target_cpu);
	cpu_fd = open(path, O_RDONLY);
	ASSERT_GE(cpu_fd, 0);

	session_fd = luo_create_session(self->fd1, "cpu-preserve-sysfs");
	ASSERT_GE(session_fd, 0);

	ASSERT_EQ(luo_session_preserve_fd(session_fd, cpu_fd, 0x100), 0);
	close(cpu_fd);

	/* Target CPU is offline while preserved */
	ASSERT_EQ(read_cpu_online(target_cpu), 0);

	/* Attempting to bring the preserved CPU online via sysfs must fail with EBUSY */
	EXPECT_EQ(write_cpu_online(target_cpu, 1), -EBUSY);
	ASSERT_EQ(read_cpu_online(target_cpu), 0);

	/* Writing 0 to online (offline) should succeed since it is already offline */
	EXPECT_EQ(write_cpu_online(target_cpu, 0), 0);

	/* Close session -> unpreserve and restore online */
	ASSERT_EQ(close(session_fd), 0);
	ASSERT_EQ(read_cpu_online(target_cpu), 1);

	/* Verify normal offline and online now succeed */
	ASSERT_EQ(write_cpu_online(target_cpu, 0), 0);
	ASSERT_EQ(read_cpu_online(target_cpu), 0);
	ASSERT_EQ(write_cpu_online(target_cpu, 1), 0);
	ASSERT_EQ(read_cpu_online(target_cpu), 1);
}

/*
 * Test Case: Prevent Double CPU Preservation
 *
 * Verifies that a CPU cannot be preserved twice across the same or different
 * sessions simultaneously.
 */
TEST_F(liveupdate_device, cpu_preserve_prevent_double)
{
	char path[128];
	int target_cpu;
	int session_fd1, session_fd2, cpu_fd1, cpu_fd2;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	target_cpu = find_hotplug_cpu();
	if (target_cpu < 0)
		SKIP(return, "No hotpluggable CPU with preserve attribute found");

	session_fd1 = luo_create_session(self->fd1, "cpu-double-1");
	ASSERT_GE(session_fd1, 0);
	session_fd2 = luo_create_session(self->fd1, "cpu-double-2");
	ASSERT_GE(session_fd2, 0);

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/preserve", target_cpu);
	cpu_fd1 = open(path, O_RDONLY);
	ASSERT_GE(cpu_fd1, 0);

	ASSERT_EQ(luo_session_preserve_fd(session_fd1, cpu_fd1, 0x10), 0);
	close(cpu_fd1);

	/* Attempting to preserve the same CPU in session2 must fail with -EBUSY */
	cpu_fd2 = open(path, O_RDONLY);
	ASSERT_GE(cpu_fd2, 0);
	EXPECT_EQ(luo_session_preserve_fd(session_fd2, cpu_fd2, 0x20), -EBUSY);
	close(cpu_fd2);

	/* Attempting to preserve again in session1 must also fail with -EBUSY */
	cpu_fd2 = open(path, O_RDONLY);
	ASSERT_GE(cpu_fd2, 0);
	EXPECT_EQ(luo_session_preserve_fd(session_fd1, cpu_fd2, 0x30), -EBUSY);
	close(cpu_fd2);

	ASSERT_EQ(close(session_fd1), 0);
	ASSERT_EQ(close(session_fd2), 0);
	ASSERT_EQ(read_cpu_online(target_cpu), 1);
}

/*
 * Test Case: Multiple Preserved CPUs
 *
 * Verifies preserving multiple distinct CPUs simultaneously in the same session.
 */
TEST_F(liveupdate_device, cpu_preserve_multiple_cpus)
{
	char path[128];
	int target_cpus[8];
	int num_targets = 0;
	int session_fd, cpu_fd, cpu, i;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	for (cpu = 1; cpu < 256 && num_targets < 8; cpu++) {
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/preserve", cpu);
		if (access(path, R_OK) == 0) {
			snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpu);
			if (access(path, R_OK) == 0)
				target_cpus[num_targets++] = cpu;
		}
	}

	if (num_targets < 2)
		SKIP(return, "Need at least 2 hotpluggable CPUs for multiple CPU preserve test");

	session_fd = luo_create_session(self->fd1, "multi-cpu-preserve");
	ASSERT_GE(session_fd, 0);

	for (i = 0; i < num_targets; i++) {
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/preserve", target_cpus[i]);
		cpu_fd = open(path, O_RDONLY);
		ASSERT_GE(cpu_fd, 0);
		ASSERT_EQ(luo_session_preserve_fd(session_fd, cpu_fd, 0x1000 + i), 0);
		close(cpu_fd);
	}

	/* Verify all targets are offline and reject sysfs online */
	for (i = 0; i < num_targets; i++) {
		ASSERT_EQ(read_cpu_online(target_cpus[i]), 0);
		EXPECT_EQ(write_cpu_online(target_cpus[i], 1), -EBUSY);
	}

	/* Close session and verify all targets return online */
	ASSERT_EQ(close(session_fd), 0);
	for (i = 0; i < num_targets; i++)
		ASSERT_EQ(read_cpu_online(target_cpus[i]), 1);
}

struct cpu_job_data {
	uint64_t input;
	uint64_t output;
};

extern const char __start_cpu_job_test[];
extern const char __stop_cpu_job_test[];

static void __attribute__((noinline, used, section("cpu_job_test")))
cpu_job_entry_fn(void *arg)
{
	struct cpu_job_data *data = arg;

	__atomic_store_n(&data->output, ~data->input, __ATOMIC_RELEASE);
}

/*
 * Test Case: CPU Preservation User Job on Freeze
 *
 * Verifies that preserving a CPU with struct liveupdate_cpu_preserve_arg
 * validates the argument and, upon session freeze, executes the job at
 * entry_offset in the identity-mapped preserved memfd with arg_offset.
 */
TEST_F(liveupdate_device, cpu_preserve_job_on_freeze)
{
	const uint64_t magic = 0x52505550435f4b4fULL;
	struct liveupdate_cpu_preserve_arg job_arg = {};
	size_t code_size = __stop_cpu_job_test - __start_cpu_job_test;
	size_t data_offset = (code_size + 63) & ~63UL;
	long page_size = getpagesize();
	int session_fd, cpu_fd, mem_fd;
	struct cpu_job_data *job_data;
	int target_cpu, i;
	char path[128];
	void *map;

	self->fd1 = open(LIVEUPDATE_DEV, O_RDWR);
	if (self->fd1 < 0 && errno == ENOENT)
		SKIP(return, "%s does not exist", LIVEUPDATE_DEV);
	ASSERT_GE(self->fd1, 0);

	target_cpu = find_hotplug_cpu();
	if (target_cpu < 0)
		SKIP(return, "No hotpluggable CPU with preserve attribute found");

	session_fd = luo_create_session(self->fd1, "cpu-job-on-freeze");
	ASSERT_GE(session_fd, 0);

	mem_fd = memfd_create("cpu-job-mem", 0);
	ASSERT_GE(mem_fd, 0);
	ASSERT_EQ(ftruncate(mem_fd, page_size), 0);

	map = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, 0);
	ASSERT_NE(map, MAP_FAILED);
	ASSERT_LT(data_offset + sizeof(*job_data), (size_t)page_size);
	memcpy(map, __start_cpu_job_test, code_size);
	job_data = (struct cpu_job_data *)((char *)map + data_offset);
	job_data->input = magic;
	job_data->output = 0;

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/preserve", target_cpu);
	cpu_fd = open(path, O_RDONLY);
	ASSERT_GE(cpu_fd, 0);

	/* Unpreserved mem_token must fail with -ENOENT */
	job_arg.size = sizeof(job_arg);
	job_arg.mem_token = 0x100;
	job_arg.entry_offset = (uintptr_t)cpu_job_entry_fn -
			       (uintptr_t)__start_cpu_job_test;
	job_arg.arg_offset = data_offset;
	EXPECT_EQ(luo_session_preserve_fd_arg(session_fd, cpu_fd, 0x200,
					      (uintptr_t)&job_arg), -ENOENT);

	ASSERT_EQ(luo_session_preserve_fd(session_fd, mem_fd, 0x100), 0);

	/* Non-zero flags must fail with -EOPNOTSUPP */
	job_arg.flags = 1;
	EXPECT_EQ(luo_session_preserve_fd_arg(session_fd, cpu_fd, 0x200,
					      (uintptr_t)&job_arg), -EOPNOTSUPP);
	job_arg.flags = 0;

	/* Out-of-bounds offset must fail with -EINVAL */
	job_arg.entry_offset = page_size;
	EXPECT_EQ(luo_session_preserve_fd_arg(session_fd, cpu_fd, 0x200,
					      (uintptr_t)&job_arg), -EINVAL);
	job_arg.entry_offset = (uintptr_t)cpu_job_entry_fn -
			       (uintptr_t)__start_cpu_job_test;

	ASSERT_EQ(luo_session_preserve_fd_arg(session_fd, cpu_fd, 0x200,
					      (uintptr_t)&job_arg), 0);
	close(cpu_fd);

	/* Job must not run before session freeze */
	EXPECT_EQ(__atomic_load_n(&job_data->output, __ATOMIC_ACQUIRE), 0ULL);

	ASSERT_EQ(luo_session_freeze(session_fd), 0);

	for (i = 0; i < 1000 && !__atomic_load_n(&job_data->output, __ATOMIC_ACQUIRE); i++)
		usleep(1000);
	EXPECT_EQ(__atomic_load_n(&job_data->output, __ATOMIC_ACQUIRE), ~magic);

	ASSERT_EQ(close(session_fd), 0);
	ASSERT_EQ(munmap(map, page_size), 0);
	ASSERT_EQ(close(mem_fd), 0);
	ASSERT_EQ(read_cpu_online(target_cpu), 1);
}

TEST_HARNESS_MAIN
