/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef _LINUX_LIVEUPDATE_H
#define _LINUX_LIVEUPDATE_H

#include <linux/bug.h>
#include <linux/types.h>
#include <linux/list.h>
#include <uapi/linux/liveupdate.h>

#ifdef CONFIG_LIVEUPDATE

/* Return true if live update orchestrator is enabled */
bool liveupdate_enabled(void);

/* Called during reboot to tell participants to complete serialization */
int liveupdate_reboot(void);

/*
 * Return true if machine is in updated state (i.e. live update boot in
 * progress)
 */
bool liveupdate_state_updated(void);

/*
 * Return true if machine is in normal state (i.e. no live update in progress).
 */
bool liveupdate_state_normal(void);

enum liveupdate_state liveupdate_get_state(void);

#else /* CONFIG_LIVEUPDATE */

static inline int liveupdate_reboot(void)
{
	return 0;
}

static inline bool liveupdate_enabled(void)
{
	return false;
}

static inline bool liveupdate_state_updated(void)
{
	return false;
}

static inline bool liveupdate_state_normal(void)
{
	return true;
}

static inline enum liveupdate_state liveupdate_get_state(void)
{
	return LIVEUPDATE_STATE_NORMAL;
}

#endif /* CONFIG_LIVEUPDATE */
#endif /* _LINUX_LIVEUPDATE_H */
