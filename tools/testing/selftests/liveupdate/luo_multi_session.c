// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#include "luo_test_utils.h"
#include "../kselftest.h"

#define KEXEC_SCRIPT "./do_kexec.sh"

#define NUM_SESSIONS 5
#define FILES_PER_SESSION 5

/* Helper to manage one session and its files */
static void setup_session(int luo_fd, struct session_info *s, int session_idx)
{
	int i;

	snprintf(s->name, sizeof(s->name), "session-%c", 'A' + session_idx);

	s->fd = luo_create_session(luo_fd, s->name);
	if (s->fd < 0)
		fail_exit("Failed to create session '%s'", s->name);

	/* Create and preserve all files for this session */
	for (i = 0; i < FILES_PER_SESSION; i++) {
		s->file_tokens[i] = (session_idx * 100) + i;
		snprintf(s->file_data[i], sizeof(s->file_data[i]),
			 "Data for %.*s-File%d",
			 LIVEUPDATE_SESSION_NAME_LENGTH,
			 s->name, i);

		if (create_and_preserve_memfd(s->fd, s->file_tokens[i],
					      s->file_data[i]) < 0) {
			fail_exit("Failed to preserve token %d in session '%s'",
				  s->file_tokens[i], s->name);
		}
	}
}

/* Helper to re-initialize the expected session data post-reboot */
static void reinit_sessions(struct session_info *sessions)
{
	int i, j;

	for (i = 0; i < NUM_SESSIONS; i++) {
		snprintf(sessions[i].name, sizeof(sessions[i].name),
			 "session-%c", 'A' + i);
		for (j = 0; j < FILES_PER_SESSION; j++) {
			sessions[i].file_tokens[j] = (i * 100) + j;
			snprintf(sessions[i].file_data[j],
				 sizeof(sessions[i].file_data[j]),
				 "Data for %.*s-File%d",
				 LIVEUPDATE_SESSION_NAME_LENGTH,
				 sessions[i].name, j);
		}
	}
}

static void run_pre_kexec(int luo_fd)
{
	struct session_info sessions[NUM_SESSIONS] = {0};
	int i;

	ksft_print_msg("[PRE-KEXEC] Starting workload...\n");

	ksft_print_msg("[PRE-KEXEC] Setting up %d sessions with %d files each...\n",
		       NUM_SESSIONS, FILES_PER_SESSION);
	for (i = 0; i < NUM_SESSIONS; i++)
		setup_session(luo_fd, &sessions[i], i);
	ksft_print_msg("[PRE-KEXEC] Setup complete.\n");

	ksft_print_msg("[PRE-KEXEC] Performing individual session state transitions...\n");
	ksft_print_msg("  - Preparing Session A...\n");
	if (luo_set_session_event(sessions[0].fd, LIVEUPDATE_PREPARE) < 0)
		fail_exit("Failed to prepare Session A");

	ksft_print_msg("  - Preparing and then Canceling Session B...\n");
	if (luo_set_session_event(sessions[1].fd, LIVEUPDATE_PREPARE) < 0)
		fail_exit("Failed to prepare Session B");
	if (luo_set_session_event(sessions[1].fd, LIVEUPDATE_CANCEL) < 0)
		fail_exit("Failed to cancel Session B");

	ksft_print_msg("  - Preparing Session C...\n");
	if (luo_set_session_event(sessions[2].fd, LIVEUPDATE_PREPARE) < 0)
		fail_exit("Failed to prepare Session C");

	ksft_print_msg("  - Sessions D & E remain in NORMAL state.\n");

	ksft_print_msg("[PRE-KEXEC] Triggering global PREPARE event...\n");
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
	struct session_info sessions[NUM_SESSIONS] = {0};

	ksft_print_msg("[POST-KEXEC] Starting workload...\n");

	reinit_sessions(sessions);

	ksft_print_msg("[POST-KEXEC] Verifying preserved sessions (A, B, C, D)...\n");
	verify_session_and_get_fd(luo_fd, &sessions[0]);
	verify_session_and_get_fd(luo_fd, &sessions[1]);
	verify_session_and_get_fd(luo_fd, &sessions[2]);
	verify_session_and_get_fd(luo_fd, &sessions[3]);

	ksft_print_msg("[POST-KEXEC] NOT retrieving session E to test cleanup.\n");

	ksft_print_msg("[POST-KEXEC] Driving global state to FINISH...\n");
	if (luo_set_global_event(luo_fd, LIVEUPDATE_FINISH) < 0)
		fail_exit("Failed to set global FINISH event");

	ksft_print_msg("\n--- TEST PASSED ---\n");
	ksft_print_msg("Check dmesg for cleanup log of session E.\n");
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
