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

struct liveupdate_subsystem;

/**
 * struct liveupdate_subsystem_ops - LUO events callback functions
 * @prepare:      Optional. Called during LUO prepare phase. Should perform
 *                preparatory actions and can store a u64 handle/state
 *                via the 'data' pointer for use in later callbacks.
 *                Return 0 on success, negative error code on failure.
 * @freeze:       Optional. Called during LUO freeze event (before actual jump
 *                to new kernel). Should perform final state saving actions and
 *                can update the u64 handle/state via the 'data' pointer. Retur:
 *                0 on success, negative error code on failure.
 * @cancel:       Optional. Called if the live update process is canceled after
 *                prepare (or freeze) was called. Receives the u64 data
 *                set by prepare/freeze. Used for cleanup.
 * @boot:         Optional. Call durng boot post live update. This callback is
 *                done when subsystem register during live update.
 * @finish:       Optional. Called after the live update is finished in the new
 *                kernel.
 *                Receives the u64 data set by prepare/freeze. Used for cleanup.
 * @owner:        Module reference
 */
struct liveupdate_subsystem_ops {
	int (*prepare)(struct liveupdate_subsystem *handle, u64 *data);
	int (*freeze)(struct liveupdate_subsystem *handle, u64 *data);
	void (*cancel)(struct liveupdate_subsystem *handle, u64 data);
	void (*boot)(struct liveupdate_subsystem *handle, u64 data);
	void (*finish)(struct liveupdate_subsystem *handle, u64 data);
	struct module *owner;
};

/**
 * struct liveupdate_subsystem - Represents a subsystem participating in LUO
 * @ops:          Callback functions
 * @name:         Unique name identifying the subsystem.
 * @list:         List head used internally by LUO. Should not be modified by
 *                caller after registration.
 * @private_data: For LUO internal use, cached value of data field.
 */
struct liveupdate_subsystem {
	const struct liveupdate_subsystem_ops *ops;
	const char *name;
	struct list_head list;
	u64 private_data;
};

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

int liveupdate_register_subsystem(struct liveupdate_subsystem *h);
int liveupdate_unregister_subsystem(struct liveupdate_subsystem *h);
int liveupdate_get_subsystem_data(struct liveupdate_subsystem *h, u64 *data);

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

static inline int liveupdate_register_subsystem(struct liveupdate_subsystem *h)
{
	return 0;
}

static inline int liveupdate_unregister_subsystem(struct liveupdate_subsystem *h)
{
	return 0;
}

static inline int liveupdate_get_subsystem_data(struct liveupdate_subsystem *h,
						u64 *data)
{
	return -ENODATA;
}

#endif /* CONFIG_LIVEUPDATE */
#endif /* _LINUX_LIVEUPDATE_H */
