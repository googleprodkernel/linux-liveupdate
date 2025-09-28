// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#include "luo_test_utils.h"
#include "../kselftest.h"

#define KEXEC_SCRIPT "./do_kexec.sh"

#define SESSION_NAME "unreclaimed_session"
#define TOKEN_A 100
#define TOKEN_B 200
#define DATA_A "This is file A, the one we retrieve."
#define DATA_B "This is file B, the one we abandon."

static void run_pre_kexec(int luo_fd)
{
	int session_fd;

	ksft_print_msg("[PRE-KEXEC] Starting workload...\n");

	session_fd = luo_create_session(luo_fd, SESSION_NAME);
	if (session_fd < 0)
		fail_exit("Failed to create session '%s'", SESSION_NAME);

	ksft_print_msg("[PRE-KEXEC] Preserving memfd A (to be restored).\n");
	if (create_and_preserve_memfd(session_fd, TOKEN_A, DATA_A) < 0)
		fail_exit("Failed to preserve memfd A");

	ksft_print_msg("[PRE-KEXEC] Preserving memfd B (to be abandoned).\n");
	if (create_and_preserve_memfd(session_fd, TOKEN_B, DATA_B) < 0)
		fail_exit("Failed to preserve memfd B");

	if (luo_set_global_event(luo_fd, LIVEUPDATE_PREPARE) < 0)
		fail_exit("Failed to set global PREPARE event");

	ksft_print_msg("[PRE-KEXEC] System is ready. Executing kexec...\n");
	if (system(KEXEC_SCRIPT) != 0)
		fail_exit("kexec script failed");

	sleep(10);
	exit(EXIT_FAILURE);
}

static void run_post_kexec(int luo_fd)
{
	int session_fd, mfd_a;

	ksft_print_msg("[POST-KEXEC] Starting workload...\n");

	session_fd = luo_retrieve_session(luo_fd, SESSION_NAME);
	if (session_fd < 0)
		fail_exit("Failed to retrieve session '%s'", SESSION_NAME);

	ksft_print_msg("[POST-KEXEC] Restoring and verifying memfd A (token %d)...\n",
		       TOKEN_A);
	mfd_a = restore_and_verify_memfd(session_fd, TOKEN_A, DATA_A);
	if (mfd_a < 0)
		fail_exit("Failed to restore or verify memfd A");
	close(mfd_a);
	ksft_print_msg("  Data verification PASSED for memfd A.\n");

	ksft_print_msg("[POST-KEXEC] NOT restoring memfd B (token %d) to test cleanup.\n",
		       TOKEN_B);

	ksft_print_msg("[POST-KEXEC] Driving global state to FINISH...\n");
	if (luo_set_global_event(luo_fd, LIVEUPDATE_FINISH) < 0)
		fail_exit("Failed to set global FINISH event");

	close(session_fd);

	ksft_print_msg("\n--- TEST PASSED ---\n");
	ksft_print_msg("Check dmesg for cleanup log of token %d in session '%s'.\n",
		       TOKEN_B, SESSION_NAME);
}

int main(int argc, char *argv[])
{
	enum liveupdate_state state;
	int luo_fd;

	luo_fd = luo_open_device();
	if (luo_fd < 0) {
		ksft_exit_skip("Failed to open %s. Is the luo module loaded?\n",
			       LUO_DEVICE);
	}

	if (luo_get_global_state(luo_fd, &state) < 0)
		fail_exit("Failed to get LUO state");

	switch (state) {
	case LIVEUPDATE_STATE_NORMAL:
		run_pre_kexec(luo_fd);
		break;
	case LIVEUPDATE_STATE_UPDATED:
		run_post_kexec(luo_fd);
		break;
	default:
		fail_exit("Test started in an unexpected state: %d", state);
	}

	close(luo_fd);
	ksft_exit_pass();
}
