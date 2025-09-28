// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#include "luo_test_utils.h"

#define KEXEC_SCRIPT "./do_kexec.sh"

#define NUM_SESSIONS 3

/* Helper to set up one session and all its files */
static void setup_session(int luo_fd, struct session_info *s, int session_idx)
{
	int i;

	snprintf(s->name, sizeof(s->name), "session-%c", 'A' + session_idx);

	s->fd = luo_create_session(luo_fd, s->name);
	if (s->fd < 0)
		fail_exit("luo_create_session for %s", s->name);

	for (i = 0; i < 2; i++) {
		s->file_tokens[i] = (session_idx * 100) + i;
		snprintf(s->file_data[i], sizeof(s->file_data[i]),
			 "Data for %.*s-File%d",
			 (int)sizeof(s->name), s->name, i);

		if (create_and_preserve_memfd(s->fd, s->file_tokens[i],
					      s->file_data[i]) < 0)
			fail_exit("create_and_preserve_memfd for token %d",
				  s->file_tokens[i]);
	}
}

/* Run before the first kexec */
static void run_stage_1(int luo_fd)
{
	struct session_info sessions[NUM_SESSIONS] = {0};
	int i;

	ksft_print_msg("[STAGE 1] Creating state file for next stage (2)...\n");
	create_state_file(luo_fd, 2);

	ksft_print_msg("[STAGE 1] Setting up Sessions A, B, C for first kexec...\n");
	for (i = 0; i < NUM_SESSIONS; i++) {
		setup_session(luo_fd, &sessions[i], i);
		ksft_print_msg("  - Session '%s' created.\n", sessions[i].name);
	}

	ksft_print_msg("[STAGE 1] Triggering global PREPARE...\n");
	if (luo_set_global_event(luo_fd, LIVEUPDATE_PREPARE) < 0)
		fail_exit("luo_set_global_event(PREPARE)");

	ksft_print_msg("[STAGE 1] Executing kexec...\n");
	if (system(KEXEC_SCRIPT) != 0)
		fail_exit("kexec script failed");

	/* Should not be reached */
	sleep(10);
	exit(EXIT_FAILURE);
}

/* Run after first kexec, before second kexec */
static void run_stage_2(int luo_fd, int state_session_fd)
{
	struct session_info sessions[NUM_SESSIONS] = {0};
	int session_fd_A;

	ksft_print_msg("[STAGE 2] Partially reclaiming and preparing for second kexec...\n");

	reinit_all_sessions(sessions, NUM_SESSIONS);

	session_fd_A = verify_session_and_get_fd(luo_fd, &sessions[0]);
	verify_session_and_get_fd(luo_fd, &sessions[1]);

	ksft_print_msg("  - Finishing state session to allow modification...\n");
	if (luo_set_session_event(state_session_fd, LIVEUPDATE_FINISH) < 0)
		fail_exit("luo_set_session_event(FINISH) for state_session");

	ksft_print_msg("  - Updating state file for next stage (3)...\n");
	update_state_file(state_session_fd, 3);

	ksft_print_msg("  - Session A verified. Sending per-session FINISH.\n");
	if (luo_set_session_event(session_fd_A, LIVEUPDATE_FINISH) < 0)
		fail_exit("luo_set_session_event(FINISH) for Session A");
	close(session_fd_A);

	ksft_print_msg("  - Session B verified. Its FD will be auto-closed for next kexec.\n");
	ksft_print_msg("  - NOT retrieving Session C to test global finish cleanup.\n");

	ksft_print_msg("[STAGE 2] Triggering global FINISH...\n");
	if (luo_set_global_event(luo_fd, LIVEUPDATE_FINISH) < 0)
		fail_exit("luo_set_global_event(FINISH)");

	ksft_print_msg("[STAGE 2] Triggering global PREPARE for next kexec...\n");
	if (luo_set_global_event(luo_fd, LIVEUPDATE_PREPARE) < 0)
		fail_exit("luo_set_global_event(PREPARE)");

	ksft_print_msg("[STAGE 2] Executing second kexec...\n");
	if (system(KEXEC_SCRIPT) != 0)
		fail_exit("kexec script failed");

	sleep(10);
	exit(EXIT_FAILURE);
}

/* Run after second kexec */
static void run_stage_3(int luo_fd)
{
	struct session_info sessions[NUM_SESSIONS] = {0};
	int ret;

	ksft_print_msg("[STAGE 3] Final verification...\n");

	reinit_all_sessions(sessions, NUM_SESSIONS);

	ksft_print_msg("[STAGE 3] Verifying surviving sessions...\n");
	/* Session B */
	verify_session_and_get_fd(luo_fd, &sessions[1]);

	ksft_print_msg("[STAGE 3] Verifying Session A was cleaned up...\n");
	ret = luo_retrieve_session(luo_fd, sessions[0].name);
	if (ret != -ENOENT)
		fail_exit("Expected ENOENT for Session A, but got %d", ret);
	ksft_print_msg("  Success. Session A not found as expected.\n");

	ksft_print_msg("[STAGE 3] Verifying Session C was cleaned up...\n");
	ret = luo_retrieve_session(luo_fd, sessions[2].name);
	if (ret != -ENOENT)
		fail_exit("Expected ENOENT for Session C, but got %d", ret);
	ksft_print_msg("  Success. Session C not found as expected.\n");

	ksft_print_msg("[STAGE 3] Triggering final global FINISH...\n");
	if (luo_set_global_event(luo_fd, LIVEUPDATE_FINISH) < 0)
		fail_exit("luo_set_global_event(FINISH)");

	ksft_print_msg("\n--- MULTI-KEXEC TEST PASSED ---\n");
}

int main(int argc, char *argv[])
{
	enum liveupdate_state state;
	int luo_fd, stage = 0;

	luo_fd = luo_open_device();
	if (luo_fd < 0) {
		ksft_exit_skip("Failed to open %s. Is the luo module loaded?\n",
			       LUO_DEVICE);
	}

	if (luo_get_global_state(luo_fd, &state) < 0)
		fail_exit("luo_get_global_state");

	if (state == LIVEUPDATE_STATE_NORMAL) {
		ksft_print_msg("LUO state is NORMAL. Starting Stage 1.\n");
		run_stage_1(luo_fd);
	} else if (state == LIVEUPDATE_STATE_UPDATED) {
		int state_session_fd;

		ksft_print_msg("LUO state is UPDATED. Restoring state to determine stage...\n");
		state_session_fd = restore_and_read_state(luo_fd, &stage);
		if (state_session_fd < 0)
			fail_exit("Could not restore test state");

		if (stage == 2) {
			ksft_print_msg("State file indicates we are entering Stage 2.\n");
			run_stage_2(luo_fd, state_session_fd);
		} else if (stage == 3) {
			ksft_print_msg("State file indicates we are entering Stage 3.\n");
			run_stage_3(luo_fd);
		} else {
			fail_exit("Invalid stage found in state file: %d",
				  stage);
		}
	}

	close(luo_fd);
	ksft_exit_pass();
}
