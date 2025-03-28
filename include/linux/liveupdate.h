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
struct liveupdate_file_handler;
struct file;

/**
 * struct liveupdate_file_ops - Callbacks for live-updatable files.
 * @prepare:       Optional. Saves state for a specific file instance @file,
 *                 before update, potentially returning value via @data.
 *                 Returns 0 on success, negative errno on failure.
 * @freeze:        Optional. Performs final actions just before kernel
 *                 transition, potentially reading/updating the handle via
 *                 @data.
 *                 Returns 0 on success, negative errno on failure.
 * @cancel:        Optional. Cleans up state/resources if update is aborted
 *                 after prepare/freeze succeeded, using the @data handle (by
 *                 value) from the successful prepare. Returns void.
 * @finish:        Optional. Performs final cleanup in the new kernel using the
 *                 preserved @data handle (by value). Returns void.
 * @retrieve:      Retrieve the preserved file. Must be called before finish.
 * @can_preserve:  callback to determine if @file can be preserved by this
 *                 handler.
 *                 Return bool (true if preservable, false otherwise).
 * @owner:         Module reference
 */
struct liveupdate_file_ops {
	int (*prepare)(struct liveupdate_file_handler *handler,
		       struct file *file, u64 *data);
	int (*freeze)(struct liveupdate_file_handler *handler,
		      struct file *file, u64 *data);
	void (*cancel)(struct liveupdate_file_handler *handler,
		       struct file *file, u64 data);
	void (*finish)(struct liveupdate_file_handler *handler,
		       struct file *file, u64 data, bool reclaimed);
	int (*retrieve)(struct liveupdate_file_handler *handler,
			u64 data, struct file **file);
	bool (*can_preserve)(struct liveupdate_file_handler *handler,
			     struct file *file);
	struct module *owner;
};

/**
 * struct liveupdate_file_handler - Represents a handler for a live-updatable
 * file type.
 * @ops:           Callback functions
 * @compatible:    The compatibility string (e.g., "memfd-v1", "vfiofd-v1")
 *                 that uniquely identifies the file type this handler supports.
 *                 This is matched against the compatible string associated with
 *                 individual &struct liveupdate_file instances.
 * @list:          used for linking this handler instance into a global list of
 *                 registered file handlers.
 *
 * Modules that want to support live update for specific file types should
 * register an instance of this structure. LUO uses this registration to
 * determine if a given file can be preserved and to find the appropriate
 * operations to manage its state across the update.
 */
struct liveupdate_file_handler {
	const struct liveupdate_file_ops *ops;
	const char *compatible;
	struct list_head list;
};

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

int liveupdate_register_file_handler(struct liveupdate_file_handler *h);
int liveupdate_unregister_file_handler(struct liveupdate_file_handler *h);

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

static inline int liveupdate_register_file_handler(struct liveupdate_file_handler *h)
{
	return 0;
}

static inline int liveupdate_unregister_file_handler(struct liveupdate_file_handler *h)
{
	return 0;
}

#endif /* CONFIG_LIVEUPDATE */
#endif /* _LINUX_LIVEUPDATE_H */
