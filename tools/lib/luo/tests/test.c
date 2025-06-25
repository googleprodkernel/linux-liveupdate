// SPDX-License-Identifier: LGPL-3.0-or-later
#define _GNU_SOURCE
/**
 * @file test.c
 * @brief Test program for the LibLUO library
 *
 * This program tests the basic functionality of the LibLUO library.
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Author: Pratyush Yadav <ptyadav@amazon.de>
 */

#include <libluo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <getopt.h>

/* Path to store token for kexec test */
#define TOKEN_FILE		"libluo_test_token"
#define TEST_DATA_FILE		"libluo_test_data"
#define MEMFD_NAME		"libluo_test_memfd"

/* Size of the random data buffer (1 MiB) */
#define RANDOM_BUFFER_SIZE	(1 << 20)
static char random_buffer[RANDOM_BUFFER_SIZE];

/* Test IDs */
#define TEST_INIT_CLEANUP	(1 << 0)
#define TEST_GET_STATE		(1 << 1)
#define TEST_FD_PRESERVE	(1 << 2)
#define TEST_ERROR_HANDLING	(1 << 3)
#define TEST_FD_KEXEC		(1 << 4)
#define TEST_FD_PREPARED	(1 << 5)
#define TEST_STATE_TRANSITIONS	(1 << 6)
#define TEST_ALL		(TEST_INIT_CLEANUP | TEST_GET_STATE | \
				 TEST_FD_PRESERVE | TEST_ERROR_HANDLING | \
				 TEST_FD_KEXEC | TEST_FD_PREPARED | \
				 TEST_STATE_TRANSITIONS)

/*
 * luo_fd_preserve() needs a unique token. Generate a monotonically increasing
 * token.
 */
static uint64_t next_token()
{
	static uint64_t token = 0;

	return token++;
}

/* Read exactly specified size from fd. Any less results in error. */
static int read_size(int fd, char *buffer, size_t size)
{
	size_t remain = size;
	ssize_t bytes_read;

	while (remain) {
		bytes_read = read(fd, buffer, remain);
		if (bytes_read == 0)
			return -ENODATA;
		if (bytes_read < 0)
			return -errno;

		remain -= bytes_read;
	}

	return 0;
}

/* Write exactly specified size from fd. Any less results in error. */
static int write_size(int fd, const char *buffer, size_t size)
{
	size_t remain = size;
	ssize_t written;

	while (remain) {
		written = write(fd, buffer, remain);
		if (written == 0)
			return -EIO;
		if (written < 0)
			return -errno;

		remain -= written;
	}

	return 0;
}

static int generate_random_data(char *buffer, size_t size)
{
	int fd, ret;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -errno;

	ret = read_size(fd, buffer, size);
	close(fd);
	return ret;
}

static int save_test_data(const char *buffer, size_t size)
{
	int fd, ret;

	fd = open(TEST_DATA_FILE, O_RDWR);
	if (fd < 0)
		return -errno;

	ret = write_size(fd, buffer, size);
	close(fd);
	return ret;
}

static int load_test_data(char *buffer, size_t size)
{
	int fd, ret;

	fd = open(TEST_DATA_FILE, O_RDONLY);
	if (fd < 0)
		return -errno;

	ret = read_size(fd, buffer, size);
	close(fd);
	return ret;
}

/* Create and initialize a memfd with random data. */
static int create_test_fd(const char *memfd_name, char *buffer, size_t size)
{
	int fd;
	int ret;

	fd = memfd_create(memfd_name, 0);
	if (fd < 0)
		return -errno;

	ret = generate_random_data(buffer, size);
	if (ret < 0) {
		close(fd);
		return ret;
	}

	if (write_size(fd, buffer, size) < 0) {
		close(fd);
		return -errno;
	}

	/* Reset file position to beginning */
	if (lseek(fd, 0, SEEK_SET) < 0) {
		close(fd);
		return -errno;
	}

	return fd;
}

/*
 * Make sure fd contains expected data up to size. Returns 0 on success, 1 on
 * data mismatch, -errno on error.
 */
static int verify_fd_content(int fd, const char *expected_data, size_t size)
{
	char buffer[size];
	int ret;

	/* Reset file position to beginning */
	if (lseek(fd, 0, SEEK_SET) < 0)
		return -errno;

	ret = read_size(fd, buffer, size);
	if (ret < 0)
		return ret;

	if (memcmp(buffer, expected_data, size) != 0)
		return 1;

	return 0;
}

/* Save token to file for kexec test. */
static int save_token(uint64_t token)
{
	FILE *file = fopen(TOKEN_FILE, "w");

	if (!file)
		return -errno;

	if (fprintf(file, "%lu", token) < 0) {
		fclose(file);
		return -errno;
	}

	fclose(file);
	return 0;
}

/* Load token from file for kexec test. */
static int load_token(uint64_t *token)
{
	FILE *file = fopen(TOKEN_FILE, "r");

	if (!file)
		return -errno;

	if (fscanf(file, "%lu", token) != 1) {
		fclose(file);
		return -EINVAL;
	}

	fclose(file);
	return 0;
}

/* Test initialization and cleanup */
static void test_init_cleanup(void)
{
	int ret;

	printf("Testing initialization and cleanup... ");

	ret = luo_init();
	if (ret < 0) {
		printf("FAILED (init: %s)\n", strerror(-ret));
		return;
	}

	luo_cleanup();
	printf("PASSED\n");
}

/* Test getting LUO state */
static void test_get_state(void)
{
	int ret;
	enum liveupdate_state state;

	printf("Testing get_state... ");

	ret = luo_init();
	if (ret < 0) {
		printf("FAILED (init: %s)\n", strerror(-ret));
		return;
	}

	ret = luo_get_state(&state);
	if (ret < 0) {
		printf("FAILED (get_state: %s)\n", strerror(-ret));
		luo_cleanup();
		return;
	}

	printf("PASSED (current state: %s)\n", luo_state_to_string(state));
	luo_cleanup();
}

/* Test preserving and unpreserving a file descriptor with prepare and cancel */
static void test_fd_preserve_unpreserve(void)
{
	uint64_t token = next_token();
	int ret, fd = -1;

	printf("Testing fd_preserve with freeze and cancel... ");

	ret = luo_init();
	if (ret < 0) {
		printf("FAILED (init: %s)\n", strerror(-ret));
		return;
	}

	fd = create_test_fd(MEMFD_NAME, random_buffer, sizeof(random_buffer));
	if (fd < 0) {
		ret = fd;
		printf("FAILED (create_test_fd: %s)\n", strerror(-ret));
		goto out_cleanup;
	}

	ret = luo_fd_preserve(fd, token);
	if (ret < 0) {
		printf("FAILED (preserve: %s)\n", strerror(-ret));
		goto out_close_fd;
	}

	ret = luo_prepare();
	if (ret < 0) {
		printf("FAILED (prepare: %s)\n", strerror(-ret));
		goto out_unpreserve;
	}

	ret = luo_cancel();
	if (ret < 0) {
		printf("FAILED (cancel: %s)\n", strerror(-ret));
		goto out_unpreserve;
	}

	ret = luo_fd_unpreserve(token);
	if (ret < 0) {
		printf("FAILED (unpreserve: %s)\n", strerror(-ret));
		goto out_close_fd;
	}

	ret = verify_fd_content(fd, random_buffer, sizeof(random_buffer));
	if (ret < 0) {
		printf("FAILED (verify_fd_content: %s)\n",
		       ret == 1 ? "data mismatch" : strerror(-ret));
		goto out_close_fd;
	}

	printf("PASSED\n");
	goto out_close_fd;

out_unpreserve:
	luo_fd_unpreserve(token);
out_close_fd:
	close(fd);
out_cleanup:
	luo_cleanup();
}

/* Test error handling with invalid inputs. */
static void test_error_handling(void)
{
	int ret;

	printf("Testing error handling... ");

	ret = luo_init();
	if (ret < 0) {
		printf("FAILED (init: %s)\n", strerror(-ret));
		return;
	}

	/* Test with invalid file descriptor */
	ret = luo_fd_preserve(-1, next_token());
	if (ret != -EINVAL) {
		printf("FAILED (expected EINVAL for invalid fd, got %d)\n", ret);
		luo_cleanup();
		return;
	}

	/* Test with NULL state pointer */
	ret = luo_get_state(NULL);
	if (ret != -EINVAL) {
		printf("FAILED (expected EINVAL for NULL state, got %d)\n", ret);
		luo_cleanup();
		return;
	}

	luo_cleanup();
	printf("PASSED\n");
}

/* Test preserving a file descriptor for kexec reboot */
static void test_fd_preserve_for_kexec(void)
{
	enum liveupdate_state state;
	int fd = -1, ret;
	uint64_t token;

	ret = luo_init();
	if (ret < 0) {
		printf("FAILED (init: %s)\n", strerror(-ret));
		return;
	}

	/* Check if we're in post-kexec state */
	ret = luo_get_state(&state);
	if (ret < 0) {
		printf("FAILED (get_state: %s)\n", strerror(-ret));
		goto out_cleanup;
	}

	if (state == LIVEUPDATE_STATE_UPDATED) {
		/* Post-kexec: restore the file descriptor */
		printf("Testing memfd restore after kexec... ");

		ret = load_token(&token);
		if (ret < 0) {
			printf("FAILED (load_token: %s)\n", strerror(-ret));
			goto out_cleanup;
		}

		ret = load_test_data(random_buffer, RANDOM_BUFFER_SIZE);
		if (ret < 0) {
			printf("FAILED (load_test_data: %s)\n", strerror(-ret));
			goto out_cleanup;
		}

		ret = luo_fd_restore(token, &fd);
		if (ret < 0) {
			printf("FAILED (restore: %s)\n", strerror(-ret));
			goto out_cleanup;
		}

		/* Verify the file descriptor content with stored data. */
		ret = verify_fd_content(fd, random_buffer, RANDOM_BUFFER_SIZE);
		if (ret) {
			printf("FAILED (verify_fd_content: %s)\n",
			       ret == 1 ? "data mismatch" : strerror(-ret));
			goto out_close_fd;
		}

		ret = luo_finish();
		if (ret < 0) {
			printf("FAILED (finish: %s)\n", strerror(-ret));
			goto out_close_fd;
		}

		printf("PASSED\n");
		goto out_close_fd;
	} else {
		/* Pre-kexec: preserve the file descriptor */
		printf("Testing fd preserve for kexec... ");

		fd = create_test_fd(MEMFD_NAME, random_buffer, RANDOM_BUFFER_SIZE);
		if (fd < 0) {
			ret = fd;
			printf("FAILED (create_test_fd: %s)\n", strerror(-ret));
			goto out_cleanup;
		}

		/* Save random data to file for post-kexec verification */
		ret = save_test_data(random_buffer, RANDOM_BUFFER_SIZE);
		if (ret < 0) {
			printf("FAILED (save_test_data: %s)\n", strerror(-ret));
			goto out_close_fd;
		}

		token = next_token();
		ret = luo_fd_preserve(fd, token);
		if (ret < 0) {
			printf("FAILED (preserve: %s)\n", strerror(-ret));
			goto out_close_fd;
		}

		/* Save token to file for post-kexec restoration */
		ret = save_token(token);
		if (ret < 0) {
			printf("FAILED (save_token: %s)\n", strerror(-ret));
			goto out_unpreserve;
		}

		ret = luo_prepare();
		if (ret < 0) {
			printf("FAILED (prepare: %s)\n", strerror(-ret));
			goto out_unpreserve;
		}

		printf("READY FOR KEXEC (token: %lu)\n", token);
		printf("Run kexec now and then run this test again to complete.\n");

		/* Note: At this point, the system should perform kexec reboot.
		 * The test will continue in the new kernel with the
		 * LIVEUPDATE_STATE_UPDATED state.
		 *
		 * Since the FD is now preserved, we can close it.
		 */
		goto out_close_fd;
	}

out_unpreserve:
	luo_fd_unpreserve(token);
out_close_fd:
	close(fd);
out_cleanup:
	luo_cleanup();
}

/*
 * Test that prepared memfd can't grow or shrink, but reads and writes still
 * work.
 */
static void test_fd_prepared_operations(void)
{
	char write_buffer[128] = {'A'};
	size_t initial_size, file_size;
	int ret, fd = -1;
	uint64_t token;

	printf("Testing operations on prepared memfd... ");

	ret = luo_init();
	if (ret < 0) {
		printf("FAILED (init: %s)\n", strerror(-ret));
		return;
	}

	/* Create and initialize test file descriptor */
	fd = create_test_fd(MEMFD_NAME, random_buffer, sizeof(random_buffer));
	if (fd < 0) {
		ret = fd;
		printf("FAILED (create_test_fd: %s)\n", strerror(-ret));
		goto out_cleanup;
	}

	/* Get initial file size */
	ret = lseek(fd, 0, SEEK_END);
	if (ret < 0) {
		printf("FAILED (lseek to end: %s)\n", strerror(errno));
		goto out_close_fd;
	}
	initial_size = (size_t)ret;

	token = next_token();
	ret = luo_fd_preserve(fd, token);
	if (ret < 0) {
		printf("FAILED (preserve: %s)\n", strerror(-ret));
		goto out_close_fd;
	}

	ret = luo_prepare();
	if (ret < 0) {
		printf("FAILED (prepare: %s)\n", strerror(-ret));
		goto out_unpreserve;
	}

	/* Test 1: Write to the prepared file descriptor (within existing size) */
	if (lseek(fd, 0, SEEK_SET) < 0) {
		printf("FAILED (lseek before write: %s)\n", strerror(errno));
		goto out_cancel;
	}

	/* Write buffer is smaller than total file size. */
	ret = write_size(fd, write_buffer, sizeof(write_buffer));
	if (ret < 0) {
		printf("FAILED (write to prepared fd: %s)\n", strerror(errno));
		goto out_cancel;
	}

	ret = verify_fd_content(fd, write_buffer, sizeof(write_buffer));
	if (ret) {
		printf("FAILED (verify_fd_content after write: %s)\n",
		       ret == 1 ? "data mismatch" : strerror(-ret));
		goto out_cancel;
	}

	/* Test 2: Try to grow the file using write(). */

	/* First, seek to one byte behind initial size. */
	ret = lseek(fd, initial_size - 1, SEEK_SET);
	if (ret < 0) {
		printf("FAILED: (lseek after write verification: %s)\n",
		       strerror(errno));
	}

	/*
	 * Then, write some data that should increase the file size. This should
	 * fail.
	 */
	ret = write_size(fd, write_buffer, sizeof(write_buffer));
	if (ret == 0) {
		printf("FAILED: (write beyond initial size succeeded)\n");
		goto out_cancel;
	}

	ret = lseek(fd, 0, SEEK_END);
	if (ret < 0) {
		printf("FAILED (lseek after larger write: %s)\n", strerror(errno));
		goto out_cancel;
	}
	file_size = (size_t)ret;

	if (file_size != initial_size) {
		printf("FAILED (file grew beyond initial size: %zu != %zu)\n",
		       (size_t)file_size, initial_size);
		goto out_cancel;
	}

	/* Test 3: Try to shrink the file using truncate */
	ret = ftruncate(fd, initial_size / 2);
	if (ret == 0) {
		printf("FAILED (file was truncated)\n");
		goto out_cancel;
	}

	ret = lseek(fd, 0, SEEK_END);
	if (ret < 0) {
		printf("FAILED (lseek after shrink attempt: %s)\n", strerror(errno));
		goto out_cancel;
	}
	file_size = (size_t)ret;

	if (file_size != initial_size) {
		printf("FAILED (file shrunk from initial size: %zu != %zu)\n",
		       (size_t)file_size, initial_size);
		goto out_cancel;
	}

	ret = luo_cancel();
	if (ret < 0) {
		printf("FAILED (cancel: %s)\n", strerror(-ret));
		goto out_unpreserve;
	}

	ret = luo_fd_unpreserve(token);
	if (ret < 0) {
		printf("FAILED (unpreserve: %s)\n", strerror(-ret));
		goto out_close_fd;
	}

	printf("PASSED\n");
	goto out_close_fd;

out_cancel:
	luo_cancel();
out_unpreserve:
	luo_fd_unpreserve(token);
out_close_fd:
	close(fd);
out_cleanup:
	luo_cleanup();
}

static int test_prepare_cancel_sequence(const char *sequence_name)
{
	int ret;
	enum liveupdate_state state;

	/* Initial state should be NORMAL */
	ret = luo_get_state(&state);
	if (ret < 0) {
		printf("FAILED (%s get initial state failed: %s)\n",
		       sequence_name, strerror(-ret));
		return ret;
	}

	if (state != LIVEUPDATE_STATE_NORMAL) {
		printf("FAILED (%s unexpected initial state: %s)\n",
		       sequence_name, luo_state_to_string(state));
		return -EINVAL;
	}

	/* Test NORMAL -> PREPARED transition */
	ret = luo_prepare();
	if (ret < 0) {
		printf("FAILED (%s prepare failed: %s)\n",
		       sequence_name, strerror(-ret));
		return ret;
	}

	ret = luo_get_state(&state);
	if (ret < 0) {
		printf("FAILED (%s get state after prepare failed: %s)\n",
		       sequence_name, strerror(-ret));
		goto out_cancel;
	}

	if (state != LIVEUPDATE_STATE_PREPARED) {
		printf("FAILED (%s expected PREPARED state, got %s)\n",
		       sequence_name, luo_state_to_string(state));
		ret = -EINVAL;
		goto out_cancel;
	}

	/* Test PREPARED -> NORMAL transition via cancel */
	ret = luo_cancel();
	if (ret < 0) {
		printf("FAILED (%s cancel failed: %s)\n",
		       sequence_name, strerror(-ret));
		return ret;
	}

	ret = luo_get_state(&state);
	if (ret < 0) {
		printf("FAILED (%s get state after cancel failed: %s)\n",
		       sequence_name, strerror(-ret));
		return ret;
	}

	if (state != LIVEUPDATE_STATE_NORMAL) {
		printf("FAILED (%s expected NORMAL state after cancel, got %s)\n",
		       sequence_name, luo_state_to_string(state));
		return -EINVAL;
	}

	return 0;

out_cancel:
	luo_cancel();
	return ret;
}

/* Test all state transitions */
static void test_state_transitions(void)
{
	int ret;

	printf("Testing state transitions... ");

	ret = luo_init();
	if (ret < 0) {
		printf("FAILED (init failed: %s)\n", strerror(-ret));
		return;
	}

	/* Test first prepare -> cancel sequence */
	ret = test_prepare_cancel_sequence("first");
	if (ret < 0)
		goto out;

	/*
	 * Test second prepare -> freeze -> cancel sequence in case the
	 * previous cancellation left some side effects.
	 */
	ret = test_prepare_cancel_sequence("second");
	if (ret < 0)
		goto out;

	printf("PASSED\n");

out:
	luo_cleanup();
}

/* Test name to flag mapping */
struct test {
	const char *name;
	void (*fn)(void);
	unsigned int flag;
};

/* Array of test names and their corresponding flags */
static struct test tests[] = {
	{"init", test_init_cleanup, TEST_INIT_CLEANUP},
	{"state", test_get_state, TEST_GET_STATE},
	{"transitions", test_state_transitions, TEST_STATE_TRANSITIONS},
	{"preserve", test_fd_preserve_unpreserve, TEST_FD_PRESERVE},
	{"prepared", test_fd_prepared_operations, TEST_FD_PREPARED},
	{"error", test_error_handling, TEST_ERROR_HANDLING},
	{"kexec", test_fd_preserve_for_kexec, TEST_FD_KEXEC},
	{NULL, NULL, 0}
};

static int parse_test_names(char *arg, unsigned int *flags)
{
	char *name;
	struct test *test;

	*flags = 0;
	name = strtok(arg, ",");

	while (name != NULL) {
		test = tests;
		while (test->name) {
			if (strcmp(name, test->name) == 0) {
				*flags |= test->flag;
				break;
			}
			test++;
		}

		/* Check if we found a match */
		if (!test->name) {
			printf("Unknown test: %s\n", name);
			return 1;
		}

		name = strtok(NULL, ",");
	}

	return 0;
}

static void usage(const char *program_name)
{
	printf("Usage: %s [options]\n", program_name);
	printf("Options:\n");
	printf("  -h, --help                 Show this help message\n");
	printf("  -t, --test=TEST_ID         Run specific test(s)\n");
	printf("  -e, --exclude=TEST_ID      Exclude specific test(s)\n");
	printf("\n");
	printf("Test IDs:\n");
	printf("  init        - Test initialization and cleanup\n");
	printf("  state       - Test getting LUO state\n");
	printf("  preserve    - Test memfd preserve/unpreserve with freeze/cancel\n");
	printf("  prepared    - Test memfd functions can read/write but not grow after prepare\n");
	printf("  transitions - Test all state transitions (NORMAL->PREPARED->FROZEN->NORMAL)\n");
	printf("  error       - Test error handling\n");
	printf("  kexec       - Test memfd preserve for kexec\n");
	printf("\n");
	printf("Multiple tests can be specified with comma separation.\n");
	printf("Example: %s --test=init,state --exclude=kexec\n", program_name);
	printf("By default, all tests are run.\n");
}

int main(int argc, char *argv[])
{
	unsigned int tests_to_run = TEST_ALL;
	unsigned int tests_to_exclude = 0;
	struct option long_options[] = {
		{"help", no_argument, 0, 'h'},
		{"test", required_argument, 0, 't'},
		{"exclude", required_argument, 0, 'e'},
		{0, 0, 0, 0}
	};
	struct test *test;
	int opt;

	printf("LibLUO Test Suite\n");
	printf("=================\n\n");

	if (!luo_is_available()) {
		printf("LUO is not available on this system. Skipping tests.\n");
		return 0;
	}

	while ((opt = getopt_long(argc, argv, "ht:e:", long_options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage(argv[0]);
			return 0;
		case 't':
			if (parse_test_names(optarg, &tests_to_run))
				return 1;
			break;
		case 'e':
			if (parse_test_names(optarg, &tests_to_exclude))
				return 1;
			break;
		default:
			printf("Try '%s --help' for more information.\n", argv[0]);
			return 1;
		}
	}

	/* Apply exclusions to the tests to run */
	tests_to_run &= ~tests_to_exclude;
	if (!tests_to_run) {
		printf("ERROR: all tests excluded\n");
		return 1;
	}

	/* Run selected tests */
	test = tests;
	while (test->name) {
		if (tests_to_run & test->flag)
			test->fn();
		test++;
	}

	printf("\nAll requested tests completed.\n");
	return 0;
}
