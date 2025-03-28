/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _LINUX_LUO_INTERNAL_H
#define _LINUX_LUO_INTERNAL_H

#include <linux/liveupdate.h>

/*
 * Handles a deserialization failure: devices and memory is in unpredictable
 * state.
 *
 * Continuing the boot process after a failure is dangerous because it could
 * lead to leaks of private data.
 */
#define luo_restore_fail(__fmt, ...) panic(__fmt, ##__VA_ARGS__)

struct luo_ucmd {
	void __user *ubuffer;
	u32 user_size;
	void *cmd;
};

static inline int luo_ucmd_respond(struct luo_ucmd *ucmd,
				   size_t kernel_cmd_size)
{
	/*
	 * Copy the minimum of what the user provided and what we actually
	 * have.
	 */
	if (copy_to_user(ucmd->ubuffer, ucmd->cmd,
			 min_t(size_t, ucmd->user_size, kernel_cmd_size))) {
		return -EFAULT;
	}
	return 0;
}

int luo_cancel(void);
int luo_prepare(void);
int luo_freeze(void);
int luo_finish(void);

void luo_state_read_enter(void);
void luo_state_read_exit(void);
extern struct rw_semaphore luo_state_rwsem;

const char *luo_current_state_str(void);

void *luo_contig_alloc_preserve(size_t size);
void luo_contig_free_unpreserve(void *mem, size_t size);
void luo_contig_free_restore(void *mem, size_t size);

/**
 * struct luo_session - Represents an active or incoming Live Update session.
 * @name:      A unique name for this session, used for identification and
 *             retrieval.
 * @files_xa:  An xarray used to store the files associated with this session.
 * @ser:       Pointer to the serialized data for this session.
 * @count:     A counter tracking the number of files currently stored in the
 *             @files_xa for this session.
 * @list:      A list_head member used to link this session into a global list
 *             of either outgoing (to be preserved) or incoming (restored from
 *             previous kernel) sessions.
 * @retrieved: A boolean flag indicating whether this session has been retrieved
 *             by a consumer in the new kernel. Valid only during the
 *             LIVEUPDATE_STATE_UPDATED state.
 * @mutex:     Session lock, protects files_xa, and count.
 * @state:     State of this session: prepared/frozen/updated/normal.
 * @files:     The physical address of a contiguous memory block that holds
 *             the serialized state of files.
 */
struct luo_session {
	char name[LIVEUPDATE_SESSION_NAME_LENGTH];
	struct xarray files_xa;
	struct luo_session_ser *ser;
	long count;
	struct list_head list;
	bool retrieved;
	struct mutex mutex;
	enum liveupdate_state state;
	u64 files;
};

int luo_session_create(const char *name, struct file **filep);
int luo_session_retrieve(const char *name, struct file **filep);

void luo_subsystems_startup(void *fdt);
int luo_subsystems_fdt_setup(void *fdt);
int luo_do_subsystems_prepare_calls(void);
int luo_do_subsystems_freeze_calls(void);
void luo_do_subsystems_finish_calls(void);
void luo_do_subsystems_cancel_calls(void);

int luo_retrieve_file(struct luo_session *session, u64 token, struct file **filep);
int luo_preserve_file(struct luo_session *session, u64 token, int fd);
int luo_unpreserve_file(struct luo_session *session, u64 token);
void luo_file_unpreserve_all_files(struct luo_session *session);
void luo_file_unpreserve_unreclaimed_files(struct luo_session *session);

int luo_file_prepare(struct luo_session *session);
int luo_file_freeze(struct luo_session *session);
void luo_file_finish(struct luo_session *session);
void luo_file_cancel(struct luo_session *session);
void luo_file_deserialize(struct luo_session *session);

#endif /* _LINUX_LUO_INTERNAL_H */
