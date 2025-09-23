// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: LUO Sessions
 *
 * LUO Sessions provide the core mechanism for grouping and managing file
 * descriptors that need to be preserved across a kexec-based live update.
 * Each session acts as a named container for a set of file objects, allowing
 * a userspace agent (e.g., a Live Update Orchestration Daemon) to manage the
 * lifecycle of resources critical to a workload.
 *
 * Core Concepts:
 *
 * - Named Containers: Sessions are identified by a unique, user-provided name,
 *   which is used for both creation and retrieval.
 *
 * - Userspace Interface: Session management is driven from userspace via
 *   ioctls on /dev/liveupdate (e.g., CREATE_SESSION, RETRIEVE_SESSION).
 *
 * - Serialization: Session metadata is preserved using the KHO framework.
 *   During the 'prepare' phase, an array of `struct luo_session_ser` is
 *   allocated and preserved. An FDT node is also created, containing the
 *   count of sessions and the physical address of this array.
 *
 * Session Lifecycle and State Management:
 *
 * 1.  Creation: A userspace agent calls `luo_session_create()` to create a new,
 *     empty session, receiving a file descriptor handle. This can be done in
 *     the NORMAL or UPDATED states.
 *
 * 2.  Name Collision: In the UPDATED state, `luo_session_create()` checks for
 *     name conflicts against sessions preserved from the previous kernel to
 *     prevent ambiguity.
 *
 * 3.  Preparation (`prepare` callback): When the global LUO PREPARE event is
 *     triggered, the list of all created sessions is serialized. The main
 *     `ser` array is allocated, and each active `struct luo_session` is given
 *     a direct pointer to its corresponding entry in this array.
 *
 * 4.  Release After Prepare: When a session FD is closed *after* the PREPARE
 *     event, the `.release` handler uses the session's direct pointer to
 *     `memset(0)` its entry in the `ser` array. This effectively marks the
 *     session as defunct without needing to resize the already-preserved
 *     memory.
 *
 * 5.  Boot (`boot` callback): In the new kernel, the FDT is read to locate
 *     the preserved `ser` array. The metadata (count, physical address) is
 *     stored in the `luo_session` global.
 *
 * 6.  Lazy Deserialization: The actual `luo_session` list is populated on
 *     first use (e.g., by `retrieve`, `finish`, or `create`). During this
 *     process, any zeroed-out entries from step 4 are skipped.
 *
 * 7.  Retrieval: The userspace agent calls `luo_session_retrieve()` in the new
 *     kernel to get a new FD handle for a preserved session by its name.
 *
 * 8.  Finalization (`finish` callback): When the global LUO FINISH event is
 *     sent, any preserved sessions that were successfully retrieved are moved
 *     to the `luo_session_global` list, making them available for a subsequent
 *     live update. Any sessions that were not retrieved are considered stale
 *     and are cleaned up.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/anon_inodes.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/libfdt.h>
#include <linux/liveupdate.h>
#include <uapi/linux/liveupdate.h>
#include "luo_internal.h"

#define LUO_SESSION_NODE_NAME	"luo-session"
#define LUO_SESSION_COMPATIBLE	"luo-session-v1"

/**
 * struct luo_session_ser - Represents the serialized metadata for a LUO session.
 * @name:    The unique name of the session, copied from the `luo_session`
 *           structure.
 * @files:   The physical address of a contiguous memory block that holds
 *           the serialized state of files.
 * @pgcnt:   The number of pages occupied by the `files` memory block.
 * @count:   The total number of files that were part of this session during
 *           serialization. Used for iteration and validation during
 *           restoration.
 *
 * This structure is used to package session-specific metadata for transfer
 * between kernels via Kexec Handover. An array of these structures (one per
 * session) is created and passed to the new kernel, allowing it to reconstruct
 * the session context.
 *
 * If this structure is modified, LUO_SESSION_COMPATIBLE must be updated.
 */
struct luo_session_ser {
	char name[LIVEUPDATE_SESSION_NAME_LENGTH];
	u64 files;
	u64 pgcnt;
	u64 count;
} __packed;

/**
 * struct luo_session_global - Global container for managing LUO sessions.
 * @count: The number of sessions currently tracked in the @list.
 * @list:  The head of the linked list of `struct luo_session` instances.
 * @rwsem: A read-write semaphore providing synchronized access to the session
 *         list and other fields in this structure.
 * @ser:   A pointer to the contiguous block of memory holding the serialized
 *         session data (an array of `struct luo_session_ser`). For `_out`, this
 *         is allocated and populated during `prepare`. For `_in`, this points
 *         to the data restored from the previous kernel.
 * @pgcnt: The size, in pages, of the memory block pointed to by @ser.
 * @fdt:   A pointer to the FDT blob that contains the metadata for this group
 *         of sessions. This FDT is what is ultimately passed to the parent LUO
 *         subsystem for preservation.
 */
struct luo_session_global {
	long count;
	struct list_head list;
	struct rw_semaphore rwsem;
	struct luo_session_ser *ser;
	u64 pgcnt;
	void *fdt;
	long ser_count;
};

static struct luo_session_global luo_session_global;

static struct luo_session *luo_session_alloc(const char *name)
{
	struct luo_session *session = kzalloc(sizeof(*session), GFP_KERNEL);

	if (!session)
		return NULL;

	strscpy(session->name, name, sizeof(session->name));
	xa_init(&session->files_xa);
	session->count = 0;
	INIT_LIST_HEAD(&session->list);
	mutex_init(&session->mutex);
	session->state = LIVEUPDATE_STATE_NORMAL;

	return session;
}

static void luo_session_free(struct luo_session *session)
{
	WARN_ON(session->count);
	xa_destroy(&session->files_xa);
	mutex_destroy(&session->mutex);
	kfree(session);
}

static int luo_session_insert(struct luo_session *session)
{
	struct luo_session *it;

	lockdep_assert_held_write(&luo_session_global.rwsem);
	/*
	 * For small number of sessions this loop won't hurt performance
	 * but if we ever start using a lot of sessions, this might
	 * become a bottle neck during deserialization time, as it would
	 * cause O(n*n) complexity.
	 */
	list_for_each_entry(it, &luo_session_global.list, list) {
		if (!strncmp(it->name, session->name, sizeof(it->name)))
			return -EEXIST;
	}
	list_add_tail(&session->list, &luo_session_global.list);
	luo_session_global.count++;

	return 0;
}

static void luo_session_remove(struct luo_session *session)
{
	lockdep_assert_held_write(&luo_session_global.rwsem);
	list_del(&session->list);
	luo_session_global.count--;
}

/* One session switches from the updated state to  normal state */
static void luo_session_finish_one(struct luo_session *session)
{
}

/* Cancel one session from frozen or prepared state, back to normal */
static void luo_session_cancel_one(struct luo_session *session)
{
}

/* One session is changed from normal to prepare state */
static int luo_session_prepare_one(struct luo_session *session)
{
	return 0;
}

static int luo_session_release(struct inode *inodep, struct file *filep)
{
	struct luo_session *session = filep->private_data;

	scoped_guard(rwsem_read, &luo_session_global.rwsem) {
		scoped_guard(mutex, &session->mutex) {
			if (session->ser) {
				memset(session->ser, 0,
				       sizeof(struct luo_session_ser));
			}
		}
	}

	if (session->state == LIVEUPDATE_STATE_UPDATED)
		luo_session_finish_one(session);
	if (session->state == LIVEUPDATE_STATE_PREPARED ||
	    session->state == LIVEUPDATE_STATE_FROZEN) {
		luo_session_cancel_one(session);
	}

	scoped_guard(rwsem_write, &luo_session_global.rwsem)
		luo_session_remove(session);
	luo_session_free(session);

	return 0;
}

static const struct file_operations luo_session_fops = {
	.owner = THIS_MODULE,
	.release = luo_session_release,
};

static void luo_session_deserialize(void)
{
	static int visited;
	int i;

	if (visited)
		return;

	guard(rwsem_write)(&luo_session_global.rwsem);
	if (visited)
		return;
	visited++;
	for (i = 0; i < luo_session_global.ser_count; i++) {
		struct luo_session *session;

		/*
		 * If there is no name, this session was remove from
		 * preservation after prepare. So, skip it.
		 */
		if (!luo_session_global.ser[i].name[0])
			continue;

		session = luo_session_alloc(luo_session_global.ser[i].name);
		if (!session)
			luo_restore_fail("Failed to allocate session on boot\n");

		if (luo_session_insert(session)) {
			luo_restore_fail("Failed to insert session due to name conflict [%s]\n",
					 session->name);
		}

		session->state = LIVEUPDATE_STATE_UPDATED;
		session->count = luo_session_global.ser[i].count;
		session->files = luo_session_global.ser[i].files;
	}
}

/* Create a "struct file" for session, and delete it on case of failure */
static int luo_session_getfile(struct luo_session *session, struct file **filep)
{
	char name_buf[128];
	struct file *file;

	scoped_guard(mutex, &session->mutex) {
		lockdep_assert_held(&session->mutex);
		snprintf(name_buf, sizeof(name_buf), "[luo_session] %s",
			 session->name);
		file = anon_inode_getfile(name_buf, &luo_session_fops, session,
					  O_RDWR);
	}
	if (IS_ERR(file)) {
		scoped_guard(rwsem_write, &luo_session_global.rwsem)
			luo_session_remove(session);
		luo_session_free(session);
		return PTR_ERR(file);
	}

	*filep = file;
	return 0;
}

int luo_session_create(const char *name, struct file **filep)
{
	struct luo_session *session;
	int ret;

	guard(rwsem_read)(&luo_state_rwsem);

	/* New sessions cannot be added after prepared state */
	if (!liveupdate_state_normal() && !liveupdate_state_updated())
		return -EAGAIN;

	session = luo_session_alloc(name);
	if (!session)
		return -ENOMEM;

	scoped_guard(rwsem_write, &luo_session_global.rwsem)
		ret = luo_session_insert(session);
	if (ret) {
		luo_session_free(session);
		return ret;
	}

	return luo_session_getfile(session, filep);
}

int luo_session_retrieve(const char *name, struct file **filep)
{
	struct luo_session *session = NULL;
	struct luo_session *it;

	guard(rwsem_read)(&luo_state_rwsem);

	/* Can only retrieve in the updated state */
	if (!liveupdate_state_updated())
		return -EAGAIN;

	luo_session_deserialize();
	scoped_guard(rwsem_read, &luo_session_global.rwsem) {
		list_for_each_entry(it, &luo_session_global.list, list) {
			if (!strncmp(it->name, name, sizeof(it->name))) {
				session = it;
				break;
			}
		}
	}

	if (!session)
		return -ENOENT;

	scoped_guard(mutex, &session->mutex) {
		/*
		 * Session already retrieved or a session with the same name was
		 * created during updated state
		 */
		if (session->retrieved || session->state != LIVEUPDATE_STATE_UPDATED)
			return -EADDRINUSE;

		session->retrieved = true;
	}

	return luo_session_getfile(session, filep);
}

static void luo_session_global_preserved_cleanup(void)
{
	lockdep_assert_held_write(&luo_session_global.rwsem);
	if (luo_session_global.ser && !IS_ERR(luo_session_global.ser)) {
		luo_contig_free_unpreserve(luo_session_global.ser,
					   luo_session_global.pgcnt << PAGE_SHIFT);
	}
	if (luo_session_global.fdt && !IS_ERR(luo_session_global.fdt))
		luo_contig_free_unpreserve(luo_session_global.fdt, PAGE_SIZE);

	luo_session_global.fdt = NULL;
	luo_session_global.ser = NULL;
	luo_session_global.ser_count = 0;
	luo_session_global.pgcnt = 0;
}

static int luo_session_fdt_setup(void)
{
	u64 ser_pa;
	int ret;

	lockdep_assert_held_write(&luo_session_global.rwsem);
	luo_session_global.pgcnt = DIV_ROUND_UP(luo_session_global.count *
				sizeof(struct luo_session_ser), PAGE_SIZE);

	if (luo_session_global.pgcnt > 0) {
		size_t ser_size = luo_session_global.pgcnt << PAGE_SHIFT;

		luo_session_global.ser = luo_contig_alloc_preserve(ser_size);
		if (IS_ERR(luo_session_global.ser)) {
			ret = PTR_ERR(luo_session_global.ser);
			goto exit_cleanup;
		}
	}

	luo_session_global.fdt = luo_contig_alloc_preserve(PAGE_SIZE);
	if (IS_ERR(luo_session_global.fdt)) {
		ret = PTR_ERR(luo_session_global.fdt);
		goto exit_cleanup;
	}

	ret = fdt_create(luo_session_global.fdt, PAGE_SIZE);
	if (ret < 0)
		goto exit_cleanup;

	ret = fdt_finish_reservemap(luo_session_global.fdt);
	if (ret < 0)
		goto exit_finish;

	ret = fdt_begin_node(luo_session_global.fdt, LUO_SESSION_NODE_NAME);
	if (ret < 0)
		goto exit_finish;

	ret = fdt_property_string(luo_session_global.fdt, "compatible",
				  LUO_SESSION_COMPATIBLE);
	if (ret < 0)
		goto exit_end_node;

	ret = fdt_property_u64(luo_session_global.fdt, "count",
			       luo_session_global.count);
	if (ret < 0)
		goto exit_end_node;

	ser_pa = luo_session_global.ser ? __pa(luo_session_global.ser) : 0;
	ret = fdt_property_u64(luo_session_global.fdt, "data", ser_pa);
	if (ret < 0)
		goto exit_end_node;

	ret = fdt_property_u64(luo_session_global.fdt, "pgcnt",
			       luo_session_global.pgcnt);
	if (ret < 0)
		goto exit_end_node;

	ret = fdt_end_node(luo_session_global.fdt);
	if (ret < 0)
		goto exit_finish;

	ret = fdt_finish(luo_session_global.fdt);
	if (ret < 0)
		goto exit_cleanup;

	return 0;

exit_end_node:
	fdt_end_node(luo_session_global.fdt);
exit_finish:
	fdt_finish(luo_session_global.fdt);
exit_cleanup:
	luo_session_global_preserved_cleanup();

	return ret;
}

/*
 * Change all sessions to normal state: make every file within each session
 * to be in the normal state.
 */
static void luo_session_cancel(struct liveupdate_subsystem *h, u64 data)
{
	struct luo_session *it;

	guard(rwsem_write)(&luo_session_global.rwsem);
	list_for_each_entry(it, &luo_session_global.list, list)
		luo_session_cancel_one(it);
	luo_session_global_preserved_cleanup();
}

static int luo_session_prepare(struct liveupdate_subsystem *h, u64 *data)
{
	struct luo_session_ser *ser;
	struct luo_session *it;
	int ret;

	scoped_guard(rwsem_write, &luo_session_global.rwsem) {
		ret = luo_session_fdt_setup();
		if (ret)
			return ret;

		ser = luo_session_global.ser;
		list_for_each_entry(it, &luo_session_global.list, list) {
			if (it->state == LIVEUPDATE_STATE_NORMAL) {
				ret = luo_session_prepare_one(it);
				if (ret)
					break;
			}
			strscpy(ser->name, it->name, sizeof(ser->name));
			ser->count = it->count;
			ser->files = it->files;
			it->ser = ser;
			ser++;
		}

		if (!ret)
			*data = __pa(luo_session_global.fdt);
	}

	if (ret)
		luo_session_cancel(h, 0);

	return ret;
}

static int luo_session_freeze(struct liveupdate_subsystem *h, u64 *data)
{
	return 0;
}

/*
 * Finish every file within each session. If session has not been reclaimed
 * remove it, otherwise keep this session, so it can participate in the
 * next live update.
 */
static void luo_session_finish(struct liveupdate_subsystem *h, u64 data)
{
	struct luo_session *session, *tmp;

	luo_session_deserialize();

	list_for_each_entry_safe(session, tmp, &luo_session_global.list, list) {
		/*
		 * Skip sessions that were created in new kernel or have been
		 * finished already.
		 */
		if (session->state != LIVEUPDATE_STATE_UPDATED)
			continue;
		luo_session_finish_one(session);
		if (!session->retrieved) {
			pr_warn("Removing unreclaimed session[%s]\n",
				session->name);
			scoped_guard(rwsem_write, &luo_session_global.rwsem)
				luo_session_remove(session);
			luo_session_free(session);
		}
	}

	scoped_guard(rwsem_write, &luo_session_global.rwsem)
		luo_session_global_preserved_cleanup();
}

static void luo_session_boot(struct liveupdate_subsystem *h, u64 data)
{
	u64 count, data_pa, pgcnt;
	const void *prop;
	int prop_len;
	void *fdt;

	fdt = __va(data);
	if (fdt_node_check_compatible(fdt, 0, LUO_SESSION_COMPATIBLE))
		luo_restore_fail("luo-session FDT incompatible\n");

	prop = fdt_getprop(fdt, 0, "count", &prop_len);
	if (!prop || prop_len != sizeof(u64))
		luo_restore_fail("luo-session FDT missing or invalid 'count'\n");
	count = be64_to_cpup(prop);

	prop = fdt_getprop(fdt, 0, "data", &prop_len);
	if (!prop || prop_len != sizeof(u64))
		luo_restore_fail("luo-session FDT missing or invalid 'data'\n");
	data_pa = be64_to_cpup(prop);

	prop = fdt_getprop(fdt, 0, "pgcnt", &prop_len);
	if (!prop || prop_len != sizeof(u64))
		luo_restore_fail("luo-session FDT missing or invalid 'pgcnt'\n");
	pgcnt = be64_to_cpup(prop);

	if (!count)
		return;

	guard(rwsem_write)(&luo_session_global.rwsem);
	luo_session_global.fdt = fdt;
	luo_session_global.ser = __va(data_pa);
	luo_session_global.ser_count = count;
	luo_session_global.pgcnt = pgcnt;
}

static const struct liveupdate_subsystem_ops luo_session_subsys_ops = {
	.prepare = luo_session_prepare,
	.freeze = luo_session_freeze,
	.cancel = luo_session_cancel,
	.boot = luo_session_boot,
	.finish = luo_session_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_subsystem luo_session_subsys = {
	.ops = &luo_session_subsys_ops,
	.name = LUO_SESSION_COMPATIBLE,
};

static int __init luo_session_startup(void)
{
	int ret;

	if (!liveupdate_enabled())
		return 0;

	init_rwsem(&luo_session_global.rwsem);
	INIT_LIST_HEAD(&luo_session_global.list);

	ret = liveupdate_register_subsystem(&luo_session_subsys);
	if (ret) {
		pr_warn("Failed to register luo_session subsystem [%d]\n", ret);
		return ret;
	}

	return ret;
}
late_initcall(luo_session_startup);
