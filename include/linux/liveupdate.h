/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef _LINUX_LIVEUPDATE_H
#define _LINUX_LIVEUPDATE_H

#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/kho/abi/luo.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/rwsem.h>
#include <linux/types.h>
#include <uapi/linux/liveupdate.h>

struct liveupdate_file_handler;
struct liveupdate_flb;
struct liveupdate_session;
struct file;

/**
 * struct liveupdate_arg_spec - Specification of a file handler user argument.
 * @size:     Full size of the argument structure in the current kernel.
 * @min_size: Minimum valid user-supplied size (offsetofend of the last
 *            required field).
 */
struct liveupdate_arg_spec {
	size_t size;
	size_t min_size;
};

/**
 * LIVEUPDATE_ARG - Validated specification of a file handler argument struct.
 * @_struct:   The argument structure type.
 * @_min_last: The last required member in @_struct.
 *
 * All file handler argument structures passed via
 * &liveupdate_session_preserve_fd.arg or &liveupdate_session_retrieve_fd.arg
 * must begin with a 32-bit @size field so they can be extended compatibly.
 * Use this macro to initialize &liveupdate_file_ops.parg and
 * &liveupdate_file_ops.rarg.
 */
#define LIVEUPDATE_ARG(_struct, _min_last)				\
	{								\
		.size = sizeof(_struct) +				\
			BUILD_BUG_ON_ZERO(offsetof(_struct, size) != 0) + \
			BUILD_BUG_ON_ZERO(sizeof_field(_struct, size) != sizeof(u32)), \
		.min_size = offsetofend(_struct, _min_last),		\
	}

/**
 * struct liveupdate_file_op_args - Arguments for file operation callbacks.
 * @handler:          The file handler being called.
 * @retrieve_status:  The retrieve status for the 'can_finish / finish'
 *                    operation. A value of 0 means the retrieve has not been
 *                    attempted (or only failed with a retryable error), a
 *                    positive value means the retrieve was successful, and a
 *                    negative value means the retrieve permanently failed, and
 *                    the value is the error code of the call.
 * @retryable:        For .retrieve(): [OUT] Set to true by the callback on
 *                    error when the preserved state has not been consumed and
 *                    retrieval may be retried.
 * @file:             The file object. For retrieve: [OUT] The callback sets
 *                    this to the new file. For other ops: [IN] The caller sets
 *                    this to the file being operated on.
 * @serialized_data:  The opaque u64 handle, preserve/prepare/freeze may update
 *                    this field.
 * @private_data:     Private data for the file used to hold runtime state that
 *                    is not preserved. Set by the handler's .preserve()
 *                    callback, and must be freed in the handler's
 *                    .unpreserve() callback.
 * @arg:              Kernel copy of the user-provided argument for .preserve()
 *                    and .retrieve(), or %NULL if none was provided. Allocated
 *                    to the handler's expected argument size with any trailing
 *                    bytes beyond the user-supplied size zero-filled. Valid
 *                    only for the duration of the callback.
 *
 * This structure bundles all parameters for the file operation callbacks.
 * The 'data' and 'file' fields are used for both input and output.
 */
struct liveupdate_file_op_args {
	struct liveupdate_file_handler *handler;
	int retrieve_status;
	bool retryable;
	struct file *file;
	u64 serialized_data;
	void *private_data;
	const void *arg;
};

/**
 * struct liveupdate_file_ops - Callbacks for live-updatable files.
 * @can_preserve: Required. Lightweight check to see if this handler is
 *                compatible with the given file.
 * @preserve:     Required. Performs state-saving for the file.
 * @unpreserve:   Required. Cleans up any resources allocated by @preserve.
 * @freeze:       Optional. Prepares final state before kexec (via
 *                LIVEUPDATE_SESSION_FREEZE or during reboot).
 * @unfreeze:     Optional. Undo freeze operations.
 * @retrieve:     Required. Restores the file in the new kernel.
 * @can_finish:   Optional. Check if this FD can finish, i.e. all restoration
 *                pre-requirements for this FD are satisfied. Called prior to
 *                finish, in order to do successful finish calls for all
 *                resources in the session.
 * @finish:       Required. Final cleanup in the new kernel.
 * @get_id:       Optional. Returns a unique identifier for the file.
 * @parg:         Optional. Specification of the user argument structure
 *                accepted by @preserve, initialized via LIVEUPDATE_ARG(). If
 *                unset, non-zero user arguments are rejected with -EOPNOTSUPP.
 * @rarg:         Optional. Specification of the user argument structure
 *                accepted by @retrieve, initialized via LIVEUPDATE_ARG(). If
 *                unset, non-zero user arguments are rejected with -EOPNOTSUPP.
 * @owner:        Module reference
 *
 * All operations (except can_preserve) receive a pointer to a
 * 'struct liveupdate_file_op_args' containing the necessary context.
 */
struct liveupdate_file_ops {
	bool (*can_preserve)(struct liveupdate_file_handler *handler,
			     struct file *file);
	int (*preserve)(struct liveupdate_file_op_args *args);
	void (*unpreserve)(struct liveupdate_file_op_args *args);
	int (*freeze)(struct liveupdate_file_op_args *args);
	void (*unfreeze)(struct liveupdate_file_op_args *args);
	int (*retrieve)(struct liveupdate_file_op_args *args);
	bool (*can_finish)(struct liveupdate_file_op_args *args);
	void (*finish)(struct liveupdate_file_op_args *args);
	unsigned long (*get_id)(struct file *file);
	struct liveupdate_arg_spec parg;
	struct liveupdate_arg_spec rarg;
	struct module *owner;
};

/**
 * struct liveupdate_file_handler - Represents a handler for a live-updatable file type.
 * @ops:                Callback functions
 * @compatible:         The compatibility string (e.g., "memfd-v1", "vfiofd-v1")
 *                      that uniquely identifies the file type this handler
 *                      supports. This is matched against the compatible string
 *                      associated with individual &struct file instances.
 *
 * Modules that want to support live update for specific file types should
 * register an instance of this structure. LUO uses this registration to
 * determine if a given file can be preserved and to find the appropriate
 * operations to manage its state across the update.
 */
struct liveupdate_file_handler {
	const struct liveupdate_file_ops *ops;
	const char compatible[LIVEUPDATE_HNDL_COMPAT_LENGTH];

	/* private: */

	/*
	 * Used for linking this handler instance into a global list of
	 * registered file handlers.
	 */
	struct list_head __private list;
	/* A list of FLB dependencies. */
	struct list_head __private flb_list;
};

/**
 * struct liveupdate_flb_op_args - Arguments for FLB operation callbacks.
 * @flb:       The global FLB instance for which this call is performed.
 * @data:      For .preserve():    [OUT] The callback sets this field.
 *             For .unpreserve():  [IN]  The handle from .preserve().
 *             For .retrieve():    [IN]  The handle from .preserve().
 * @obj:       For .preserve():    [OUT] Sets this to the live object.
 *             For .retrieve():    [OUT] Sets this to the live object.
 *             For .finish():      [IN]  The live object from .retrieve().
 *
 * This structure bundles all parameters for the FLB operation callbacks.
 */
struct liveupdate_flb_op_args {
	struct liveupdate_flb *flb;
	u64 data;
	void *obj;
};

/**
 * struct liveupdate_flb_ops - Callbacks for global File-Lifecycle-Bound data.
 * @preserve:        Called when the first file using this FLB is preserved.
 *                   The callback must save its state and return a single,
 *                   self-contained u64 handle by setting the 'argp->data'
 *                   field and 'argp->obj'.
 * @unpreserve:      Called when the last file using this FLB is unpreserved
 *                   (aborted before reboot). Receives the handle via
 *                   'argp->data' and live object via 'argp->obj'.
 * @retrieve:        Called on-demand in the new kernel, the first time a
 *                   component requests access to the shared object. It receives
 *                   the preserved handle via 'argp->data' and must reconstruct
 *                   the live object, returning it by setting the 'argp->obj'
 *                   field.
 * @finish:          Called in the new kernel when the last file using this FLB
 *                   is finished. Receives the live object via 'argp->obj' for
 *                   cleanup.
 * @owner:           Module reference
 *
 * Operations that manage global shared data with file bound lifecycle,
 * triggered by the first file that uses it and concluded by the last file that
 * uses it, across all sessions.
 */
struct liveupdate_flb_ops {
	int (*preserve)(struct liveupdate_flb_op_args *argp);
	void (*unpreserve)(struct liveupdate_flb_op_args *argp);
	int (*retrieve)(struct liveupdate_flb_op_args *argp);
	void (*finish)(struct liveupdate_flb_op_args *argp);
	struct module *owner;
};

/*
 * struct luo_flb_private_state - Private FLB state structures.
 * @count:     The number of preserved files currently depending on this FLB.
 *             This is used to trigger the preserve/unpreserve/finish ops on the
 *             first/last file.
 * @data:      The opaque u64 handle returned by .preserve() or passed to
 *             .retrieve().
 * @obj:       The live kernel object returned by .preserve() or .retrieve().
 * @lock:      A mutex that protects all fields within this structure, providing
 *             the synchronization service for the FLB's ops.
 * @finished:  True once the FLB's finish() callback has run.
 * @retrieve_status: Status code indicating whether retrieve() has been
 *                   attempted. 0 means not attempted, 1 means successful,
 *                   and negative value means it failed with that error code.
 */
struct luo_flb_private_state {
	refcount_t count;
	u64 data;
	void *obj;
	struct mutex lock;
	bool finished;
	int retrieve_status;
};

/*
 * struct luo_flb_private - Keep separate incoming and outgoing states.
 * @list:        A global list of registered FLBs.
 * @outgoing:    The runtime state for the pre-reboot
 *               (preserve/unpreserve) lifecycle.
 * @incoming:    The runtime state for the post-reboot (retrieve/finish)
 *               lifecycle.
 * @users:       With how many File-Handlers this FLB is registered.
 * @initialized: true when private fields have been initialized.
 */
struct luo_flb_private {
	struct list_head list;
	struct luo_flb_private_state outgoing;
	struct luo_flb_private_state incoming;
	int users;
	bool initialized;
};

/**
 * struct liveupdate_flb - A global definition for a shared data object.
 * @ops:         Callback functions
 * @compatible:  The compatibility string (e.g., "iommu-core-v1"
 *               that uniquely identifies the FLB type this handler
 *               supports. This is matched against the compatible string
 *               associated with individual &struct liveupdate_flb
 *               instances.
 *
 * This struct is the "template" that a driver registers to define a shared,
 * file-lifecycle-bound object. The actual runtime state (the live object,
 * refcount, etc.) is managed privately by the LUO core.
 */
struct liveupdate_flb {
	const struct liveupdate_flb_ops *ops;
	const char compatible[LIVEUPDATE_FLB_COMPAT_LENGTH];

	/* private: */
	struct luo_flb_private __private private;
};

#ifdef CONFIG_LIVEUPDATE

/* Return true if live update orchestrator is enabled */
bool liveupdate_enabled(void);

/* Called during kexec to tell LUO that entered into reboot */
int liveupdate_reboot(void);

int liveupdate_register_file_handler(struct liveupdate_file_handler *fh);
void liveupdate_unregister_file_handler(struct liveupdate_file_handler *fh);

int liveupdate_register_flb(struct liveupdate_file_handler *fh,
			    struct liveupdate_flb *flb);
void liveupdate_unregister_flb(struct liveupdate_file_handler *fh,
			       struct liveupdate_flb *flb);

int liveupdate_flb_get_incoming(struct liveupdate_flb *flb, void **objp);
void liveupdate_flb_put_incoming(struct liveupdate_flb *flb);

int liveupdate_flb_get_outgoing(struct liveupdate_flb *flb, void **objp);
void liveupdate_flb_put_outgoing(struct liveupdate_flb *flb);

#else /* CONFIG_LIVEUPDATE */

static inline bool liveupdate_enabled(void)
{
	return false;
}

static inline int liveupdate_reboot(void)
{
	return 0;
}

static inline int liveupdate_register_file_handler(struct liveupdate_file_handler *fh)
{
	return -EOPNOTSUPP;
}

static inline void liveupdate_unregister_file_handler(struct liveupdate_file_handler *fh)
{
}

static inline int liveupdate_register_flb(struct liveupdate_file_handler *fh,
					  struct liveupdate_flb *flb)
{
	return -EOPNOTSUPP;
}

static inline void liveupdate_unregister_flb(struct liveupdate_file_handler *fh,
					     struct liveupdate_flb *flb)
{
}

static inline int liveupdate_flb_get_incoming(struct liveupdate_flb *flb,
					      void **objp)
{
	return -EOPNOTSUPP;
}

static inline void liveupdate_flb_put_incoming(struct liveupdate_flb *flb)
{
}

static inline int liveupdate_flb_get_outgoing(struct liveupdate_flb *flb,
					      void **objp)
{
	return -EOPNOTSUPP;
}

static inline void liveupdate_flb_put_outgoing(struct liveupdate_flb *flb)
{
}

#endif /* CONFIG_LIVEUPDATE */
#endif /* _LINUX_LIVEUPDATE_H */
