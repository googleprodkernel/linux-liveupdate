// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file luoctl.c
 * @brief Simple utility to interact with LUO
 *
 * This utility allows viewing and controlling LUO state.
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Author: Pratyush Yadav <ptyadav@amazon.de>
 */

#include <libluo.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#define fatal(fmt, ...)					\
	do {						\
		fprintf(stderr, "Error: " fmt, ##__VA_ARGS__);	\
		exit(1);				\
	} while (0)

struct command {
	char *name;
	int (*handler)(void);
};

static void usage(const char *prog_name)
{
	printf("Usage: %s [command]\n\n", prog_name);
	printf("Commands:\n");
	printf("  state         - Show current LUO state\n");
	printf("  prepare       - Prepare for live update\n");
	printf("  cancel        - Cancel live update preparation\n");
	printf("  finish        - Signal completion of restoration\n");
}

static enum liveupdate_state get_state(void)
{
	enum liveupdate_state state;
	int ret;

	ret = luo_get_state(&state);
	if (ret)
		fatal("failed to get LUO state: %s\n", strerror(-ret));

	return state;
}

static int show_state(void)
{
	enum liveupdate_state state;

	state = get_state();
	printf("%s\n", luo_state_to_string(state));
	return 0;
}

static int do_prepare(void)
{
	enum liveupdate_state state;
	int ret;

	state = get_state();
	if (state != LIVEUPDATE_STATE_NORMAL)
		fatal("can only switch to prepared state from normal state. Current state: %s\n",
		      luo_state_to_string(state));

	ret = luo_prepare();
	if (ret)
		fatal("failed to prepare for live update: %s\n", strerror(-ret));

	return 0;
}

static int do_cancel(void)
{
	enum liveupdate_state state;
	int ret;

	state = get_state();
	if (state != LIVEUPDATE_STATE_PREPARED)
		fatal("can only cancel from normal state. Current state: %s\n",
		      luo_state_to_string(state));

	ret = luo_cancel();
	if (ret)
		fatal("failed to cancel live update: %s\n", strerror(-ret));

	return 0;
}

static int do_finish(void)
{
	enum liveupdate_state state;
	int ret;

	state = get_state();
	if (state != LIVEUPDATE_STATE_UPDATED)
		fatal("can only finish from updated state. Current state: %s\n",
		      luo_state_to_string(state));

	ret = luo_finish();
	if (ret)
		fatal("failed to finish live update: %s\n", strerror(-ret));

	return 0;
}

static struct command commands[] = {
	{"state", show_state},
	{"prepare", do_prepare},
	{"cancel", do_cancel},
	{"finish", do_finish},
	{NULL, NULL},
};

int main(int argc, char *argv[])
{
	struct option long_options[] = {
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};
	struct command *command;
	int ret = -EINVAL, opt;
	char *cmd;

	if (!luo_is_available()) {
		fprintf(stderr, "LUO is not available on this system\n");
		return 1;
	}

	while ((opt = getopt_long(argc, argv, "ht:e:", long_options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return 1;
		}
	}

	if (argc - optind != 1) {
		usage(argv[0]);
		return 1;
	}

	cmd = argv[optind];

	ret = luo_init();
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize LibLUO: %s\n", strerror(-ret));
		return 1;
	}

	command = &commands[0];
	while (command->name) {
		if (!strcmp(cmd, command->name)) {
			ret = command->handler();
			break;
		}
		command++;
	}

	if (!command->name) {
		fprintf(stderr, "Unknown command %s. Try '%s --help' for more information\n",
			cmd, argv[0]);
		ret = -EINVAL;
	}

	luo_cleanup();
	return (ret < 0) ? 1 : 0;
}
