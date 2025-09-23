/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

/*
 * Userspace interface for /dev/liveupdate
 * Live Update Orchestrator
 *
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _UAPI_LIVEUPDATE_H
#define _UAPI_LIVEUPDATE_H

#include <linux/ioctl.h>
#include <linux/types.h>

/**
 * DOC: General ioctl format
 *
 * The ioctl interface follows a general format to allow for extensibility. Each
 * ioctl is passed in a structure pointer as the argument providing the size of
 * the structure in the first u32. The kernel checks that any structure space
 * beyond what it understands is 0. This allows userspace to use the backward
 * compatible portion while consistently using the newer, larger, structures.
 *
 * ioctls use a standard meaning for common errnos:
 *
 *  - ENOTTY: The IOCTL number itself is not supported at all
 *  - E2BIG: The IOCTL number is supported, but the provided structure has
 *    non-zero in a part the kernel does not understand.
 *  - EOPNOTSUPP: The IOCTL number is supported, and the structure is
 *    understood, however a known field has a value the kernel does not
 *    understand or support.
 *  - EINVAL: Everything about the IOCTL was understood, but a field is not
 *    correct.
 *  - ENOENT: A provided token does not exist.
 *  - ENOMEM: Out of memory.
 *  - EOVERFLOW: Mathematics overflowed.
 *
 * As well as additional errnos, within specific ioctls.
 */

/**
 * enum liveupdate_state - Defines the possible states of the live update
 * orchestrator.
 * @LIVEUPDATE_STATE_UNDEFINED:      State has not yet been initialized.
 * @LIVEUPDATE_STATE_NORMAL:         Default state, no live update in progress.
 * @LIVEUPDATE_STATE_PREPARED:       Live update is prepared for reboot; the
 *                                   LIVEUPDATE_PREPARE callbacks have completed
 *                                   successfully.
 *                                   Devices might operate in a limited state
 *                                   for example the participating devices might
 *                                   not be allowed to unbind, and also the
 *                                   setting up of new DMA mappings might be
 *                                   disabled in this state.
 * @LIVEUPDATE_STATE_FROZEN:         The final reboot event
 *                                   (%LIVEUPDATE_FREEZE) has been sent, and the
 *                                   system is performing its final state saving
 *                                   within the "blackout window". User
 *                                   workloads must be suspended. The actual
 *                                   reboot (kexec) into the next kernel is
 *                                   imminent.
 * @LIVEUPDATE_STATE_UPDATED:        The system has rebooted into the next
 *                                   kernel via live update the system is now
 *                                   running the next kernel, awaiting the
 *                                   finish event.
 *
 * These states track the progress and outcome of a live update operation.
 */
enum liveupdate_state  {
	LIVEUPDATE_STATE_UNDEFINED = 0,
	LIVEUPDATE_STATE_NORMAL = 1,
	LIVEUPDATE_STATE_PREPARED = 2,
	LIVEUPDATE_STATE_FROZEN = 3,
	LIVEUPDATE_STATE_UPDATED = 4,
};

/**
 * enum liveupdate_event - Events that trigger live update callbacks.
 * @LIVEUPDATE_PREPARE: PREPARE should happen *before* the blackout window.
 *                      Subsystems should prepare for an upcoming reboot by
 *                      serializing their states. However, it must be considered
 *                      that user applications, e.g. virtual machines are still
 *                      running during this phase.
 * @LIVEUPDATE_FREEZE:  FREEZE sent from the reboot() syscall, when the current
 *                      kernel is on its way out. This is the final opportunity
 *                      for subsystems to save any state that must persist
 *                      across the reboot. Callbacks for this event should be as
 *                      fast as possible since they are on the critical path of
 *                      rebooting into the next kernel.
 * @LIVEUPDATE_FINISH:  FINISH is sent in the newly booted kernel after a
 *                      successful live update and normally *after* the blackout
 *                      window. Subsystems should perform any final cleanup
 *                      during this phase. This phase also provides an
 *                      opportunity to clean up devices that were preserved but
 *                      never explicitly reclaimed during the live update
 *                      process. State restoration should have already occurred
 *                      before this event. Callbacks for this event must not
 *                      fail. The completion of this call transitions the
 *                      machine from ``updated`` to ``normal`` state.
 * @LIVEUPDATE_CANCEL:  CANCEL the live update and go back to normal state. This
 *                      event is user initiated, or is done automatically when
 *                      LIVEUPDATE_PREPARE or LIVEUPDATE_FREEZE stage fails.
 *                      Subsystems should revert any actions taken during the
 *                      corresponding prepare event. Callbacks for this event
 *                      must not fail.
 *
 * These events represent the different stages and actions within the live
 * update process that subsystems (like device drivers and bus drivers)
 * need to be aware of to correctly serialize and restore their state.
 *
 */
enum liveupdate_event {
	LIVEUPDATE_PREPARE = 0,
	LIVEUPDATE_FREEZE = 1,
	LIVEUPDATE_FINISH = 2,
	LIVEUPDATE_CANCEL = 3,
};

/* The maximum length of session name including null termination */
#define LIVEUPDATE_SESSION_NAME_LENGTH 56

/* The ioctl type, documented in ioctl-number.rst */
#define LIVEUPDATE_IOCTL_TYPE		0xBA

/* The /dev/liveupdate ioctl commands */
enum {
	LIVEUPDATE_CMD_BASE = 0x00,
	LIVEUPDATE_CMD_GET_STATE = LIVEUPDATE_CMD_BASE,
	LIVEUPDATE_CMD_SET_EVENT = 0x01,
	LIVEUPDATE_CMD_CREATE_SESSION = 0x02,
	LIVEUPDATE_CMD_RETRIEVE_SESSION = 0x03,
};

/* ioctl commands for session file descriptors */
enum {
	LIVEUPDATE_CMD_SESSION_BASE = 0x40,
	LIVEUPDATE_CMD_SESSION_PRESERVE_FD = LIVEUPDATE_CMD_SESSION_BASE,
	LIVEUPDATE_CMD_SESSION_UNPRESERVE_FD = 0x41,
	LIVEUPDATE_CMD_SESSION_RESTORE_FD = 0x42,
	LIVEUPDATE_CMD_SESSION_GET_STATE = 0x43,
	LIVEUPDATE_CMD_SESSION_SET_EVENT = 0x44,
};

/**
 * struct liveupdate_ioctl_get_state - ioctl(LIVEUPDATE_IOCTL_GET_STATE)
 * @size:  Input; sizeof(struct liveupdate_ioctl_get_state)
 * @state: Output; The current live update state.
 *
 * Query the current state of the live update orchestrator.
 *
 * The kernel fills the @state with the current
 * state of the live update subsystem. Possible states are:
 *
 * - %LIVEUPDATE_STATE_NORMAL:   Default state; no live update operation is
 *                               currently in progress.
 * - %LIVEUPDATE_STATE_PREPARED: The preparation phase (triggered by
 *                               %LIVEUPDATE_PREPARE) has completed
 *                               successfully. The system is ready for the
 *                               reboot transition. Note that some
 *                               device operations (e.g., unbinding, new DMA
 *                               mappings) might be restricted in this state.
 * - %LIVEUPDATE_STATE_UPDATED:  The system has successfully rebooted into the
 *                               new kernel via live update. It is now running
 *                               the new kernel code and is awaiting the
 *                               completion signal from user space via
 *                               %LIVEUPDATE_FINISH after restoration tasks are
 *                               done.
 *
 * See the definition of &enum liveupdate_state for more details on each state.
 *
 * Return: 0 on success, negative error code on failure.
 */
struct liveupdate_ioctl_get_state {
	__u32	size;
	__u32	state;
};

#define LIVEUPDATE_IOCTL_GET_STATE					\
	_IO(LIVEUPDATE_IOCTL_TYPE, LIVEUPDATE_CMD_GET_STATE)

/**
 * struct liveupdate_ioctl_set_event - ioctl(LIVEUPDATE_IOCTL_SET_EVENT)
 * @size:  Input; sizeof(struct liveupdate_ioctl_set_event)
 * @event: Input; The live update event.
 *
 * Notify live update orchestrator about global event, that causes a state
 * transition.
 *
 * Event, can be one of the following:
 *
 * - %LIVEUPDATE_PREPARE: Initiates the live update preparation phase. This
 *                        typically triggers the saving process for items marked
 *                        via the PRESERVE ioctls. This typically occurs
 *                        *before* the "blackout window", while user
 *                        applications (e.g., VMs) may still be running. Kernel
 *                        subsystems receiving the %LIVEUPDATE_PREPARE event
 *                        should serialize necessary state. This command does
 *                        not transfer data.
 * - %LIVEUPDATE_FINISH:  Signal restoration completion and triggercleanup.
 *
 *                        Signals that user space has completed all necessary
 *                        restoration actions in the new kernel (after a live
 *                        update reboot). Calling this ioctl triggers the
 *                        cleanup phase: any resources that were successfully
 *                        preserved but were *not* subsequently restored
 *                        (reclaimed) via the RESTORE ioctls will have their
 *                        preserved state discarded and associated kernel
 *                        resources released. Involved devices may be reset. All
 *                        desired restorations *must* be completed *before*
 *                        this. Kernel callbacks for the %LIVEUPDATE_FINISH
 *                        event must not fail. Successfully completing this
 *                        phase transitions the system state from
 *                        %LIVEUPDATE_STATE_UPDATED back to
 *                        %LIVEUPDATE_STATE_NORMAL. This command does
 *                        not transfer data.
 * - %LIVEUPDATE_CANCEL:  Cancel the live update preparation phase.
 *
 *                        Notifies the live update subsystem to abort the
 *                        preparation sequence potentially initiated by
 *                        %LIVEUPDATE_PREPARE event.
 *
 *                        When triggered, subsystems receiving the
 *                        %LIVEUPDATE_CANCEL event should revert any state
 *                        changes or actions taken specifically for the aborted
 *                        prepare phase (e.g., discard partially serialized
 *                        state). The kernel releases resources allocated
 *                        specifically for this *aborted preparation attempt*.
 *
 *                        This operation cancels the current *attempt* to
 *                        prepare for a live update but does **not** remove
 *                        previously validated items from the internal list
 *                        of potentially preservable resources.
 *
 *                        This command does not transfer data. Kernel callbacks
 *                        for the %LIVEUPDATE_CANCEL event must not fail.
 *
 * See the definition of &enum liveupdate_event for more details on each state.
 *
 * Return: 0 on success, negative error code on failure.
 */
struct liveupdate_ioctl_set_event {
	__u32	size;
	__u32	event;
};

#define LIVEUPDATE_IOCTL_SET_EVENT					\
	_IO(LIVEUPDATE_IOCTL_TYPE, LIVEUPDATE_CMD_SET_EVENT)

/**
 * struct liveupdate_ioctl_create_session - ioctl(LIVEUPDATE_IOCTL_CREATE_SESSION)
 * @size:	Input; sizeof(struct liveupdate_ioctl_create_session)
 * @fd:		Output; The new file descriptor for the created session.
 * @name:	Input; A null-terminated string for the session name, max
 *		length %LIVEUPDATE_SESSION_NAME_LENGTH including termination
 *		char.
 *
 * Creates a new live update session for managing preserved resources.
 * This ioctl can only be called on the main /dev/liveupdate device.
 *
 * Return: 0 on success, negative error code on failure.
 */
struct liveupdate_ioctl_create_session {
	__u32		size;
	__s32		fd;
	__u8		name[LIVEUPDATE_SESSION_NAME_LENGTH];
};

#define LIVEUPDATE_IOCTL_CREATE_SESSION					\
	_IO(LIVEUPDATE_IOCTL_TYPE, LIVEUPDATE_CMD_CREATE_SESSION)

/**
 * struct liveupdate_ioctl_retrieve_session - ioctl(LIVEUPDATE_IOCTL_RETRIEVE_SESSION)
 * @size:    Input; sizeof(struct liveupdate_ioctl_retrieve_session)
 * @fd:      Output; The new file descriptor for the retrieved session.
 * @name:    Input; A null-terminated string identifying the session to retrieve.
 *           The name must exactly match the name used when the session was
 *           created in the previous kernel.
 *
 * Retrieves a handle (a new file descriptor) for a preserved session by its
 * name. This is the primary mechanism for a userspace agent to regain control
 * of its preserved resources after a live update.
 *
 * The userspace application provides the null-terminated `name` of a session
 * it created before the live update. If a preserved session with a matching
 * name is found, the kernel instantiates it and returns a new file descriptor
 * in the `fd` field. This new session FD can then be used for all file-specific
 * operations, such as restoring individual file descriptors with
 * LIVEUPDATE_SESSION_RESTORE_FD.
 *
 * It is the responsibility of the userspace application to know the names of
 * the sessions it needs to retrieve. If no session with the given name is
 * found, the ioctl will fail with -ENOENT.
 *
 * This ioctl can only be called on the main /dev/liveupdate device when the
 * system is in the LIVEUPDATE_STATE_UPDATED state.
 */
struct liveupdate_ioctl_retrieve_session {
	__u32		size;
	__s32		fd;
	__u8		name[64];
};

#define LIVEUPDATE_IOCTL_RETRIEVE_SESSION \
	_IO(LIVEUPDATE_IOCTL_TYPE, LIVEUPDATE_CMD_RETRIEVE_SESSION)

/* Session specific IOCTLs */

/**
 * struct liveupdate_session_preserve_fd - ioctl(LIVEUPDATE_SESSION_PRESERVE_FD)
 * @size:  Input; sizeof(struct liveupdate_session_preserve_fd)
 * @fd:    Input; The user-space file descriptor to be preserved.
 * @token: Input; An opaque, unique token for preserved resource.
 *
 * Holds parameters for preserving Validate and initiate preservation for a file
 * descriptor.
 *
 * User sets the @fd field identifying the file descriptor to preserve
 * (e.g., memfd, kvm, iommufd, VFIO). The kernel validates if this FD type
 * and its dependencies are supported for preservation. If validation passes,
 * the kernel marks the FD internally and *initiates the process* of preparing
 * its state for saving. The actual snapshotting of the state typically occurs
 * during the subsequent %LIVEUPDATE_IOCTL_PREPARE execution phase, though
 * some finalization might occur during freeze.
 * On successful validation and initiation, the kernel uses the @token
 * field with an opaque identifier representing the resource being preserved.
 * This token confirms the FD is targeted for preservation and is required for
 * the subsequent %LIVEUPDATE_SESSION_RESTORE_FD call after the live update.
 *
 * Return: 0 on success (validation passed, preservation initiated), negative
 * error code on failure (e.g., unsupported FD type, dependency issue,
 * validation failed).
 */
struct liveupdate_session_preserve_fd {
	__u32		size;
	__s32		fd;
	__aligned_u64	token;
};

#define LIVEUPDATE_SESSION_PRESERVE_FD					\
	_IO(LIVEUPDATE_IOCTL_TYPE, LIVEUPDATE_CMD_SESSION_PRESERVE_FD)

/**
 * struct liveupdate_session_unpreserve_FD - ioctl(LIVEUPDATE_SESSION_UNPRESERVE_FD)
 * @size:     Input; sizeof(struct liveupdate_session_unpreserve_fd)
 * @reserved: Must be zero.
 * @token:    Input; A token for resource to be unpreserved.
 *
 * Remove a file descriptor from the preservation list.
 *
 * Allows user space to explicitly remove a file descriptor from the set of
 * items marked as potentially preservable. User space provides a @token that
 * was previously used by a successful %LIVEUPDATE_SESSION_PRESERVE_FD call
 * (potentially from a prior, possibly canceled, live update attempt). The
 * kernel reads the token value from the provided user-space address.
 *
 * On success, the kernel removes the corresponding entry (identified by the
 * token value read from the user pointer) from its internal preservation list.
 * The provided @token (representing the now-removed entry) becomes invalid
 * after this call.
 *
 * Return: 0 on success, negative error code on failure (e.g., -EBUSY or -EINVAL
 * if bad address provided, invalid token value read, token not found).
 */
struct liveupdate_session_unpreserve_fd {
	__u32		size;
	__u32		reserved;
	__aligned_u64	token;
};

#define LIVEUPDATE_SESSION_UNPRESERVE_FD				\
	_IO(LIVEUPDATE_IOCTL_TYPE, LIVEUPDATE_CMD_SESSION_UNPRESERVE_FD)

/**
 * struct liveupdate_session_restore_fd - ioctl(LIVEUPDATE_SESSION_RESTORE_FD)
 * @size:  Input; sizeof(struct liveupdate_session_restore_fd)
 * @fd:    Output; The new file descriptor representing the fully restored
 *         kernel resource.
 * @token: Input; An opaque, token that was used to preserve the resource.
 *
 * Restore a previously preserved file descriptor.
 *
 * User sets the @token field to the value obtained from a successful
 * %LIVEUPDATE_IOCTL_FD_PRESERVE call before the live update. On success,
 * the kernel restores the state (saved during the PREPARE/FREEZE phases)
 * associated with the token and populates the @fd field with a new file
 * descriptor referencing the restored resource in the current (new) kernel.
 * This operation must be performed *before* signaling completion via
 * %LIVEUPDATE_IOCTL_FINISH.
 *
 * Return: 0 on success, negative error code on failure (e.g., invalid token).
 */
struct liveupdate_session_restore_fd {
	__u32		size;
	__s32		fd;
	__aligned_u64	token;
};

#define LIVEUPDATE_SESSION_RESTORE_FD					\
	_IO(LIVEUPDATE_IOCTL_TYPE, LIVEUPDATE_CMD_SESSION_RESTORE_FD)

/**
 * struct liveupdate_session_get_state - ioctl(LIVEUPDATE_SESSION_GET_STATE)
 * @size:     Input; sizeof(struct liveupdate_session_get_state)
 * @incoming: Input; If 1, query the state of a restored file from the incoming
 *            (previous kernel's) set. If 0, query a file being prepared for
 *            preservation in the current set.
 * @reserved: Must be zero.
 * @state:    Output; The live update state of this FD.
 *
 * Query the current live update state of a specific preserved file descriptor.
 *
 * - %LIVEUPDATE_STATE_NORMAL:   Default state
 * - %LIVEUPDATE_STATE_PREPARED: Prepare callback has been performed on this FD.
 * - %LIVEUPDATE_STATE_FROZEN:   Freeze callback ahs been performed on this FD.
 * - %LIVEUPDATE_STATE_UPDATED:  The system has successfully rebooted into the
 *                               new kernel.
 *
 * See the definition of &enum liveupdate_state for more details on each state.
 *
 * Return: 0 on success, negative error code on failure.
 */
struct liveupdate_session_get_state {
	__u32		size;
	__u8		incoming;
	__u8		reserved[3];
	__u32		state;
};

#define LIVEUPDATE_SESSION_GET_STATE					\
	_IO(LIVEUPDATE_IOCTL_TYPE, LIVEUPDATE_CMD_SESSION_GET_STATE)

/**
 * struct liveupdate_session_set_event - ioctl(LIVEUPDATE_SESSION_SET_EVENT)
 * @size:  Input; sizeof(struct liveupdate_session_set_event)
 * @event: Input; The live update event.
 *
 * Notify a specific preserved file descriptor of an event, that causes a state
 * transition for that file descriptor.
 *
 * Event, can be one of the following:
 *
 * - %LIVEUPDATE_PREPARE: Initiates the FD live update preparation phase.
 * - %LIVEUPDATE_FREEZE:  Initiates the FD live update freeze phase.
 * - %LIVEUPDATE_CANCEL:  Cancel the FD preparation or freeze phase.
 * - %LIVEUPDATE_FINISH:  FD Restoration completion and trigger cleanup.
 *
 * See the definition of &enum liveupdate_event for more details on each state.
 *
 * Return: 0 on success, negative error code on failure.
 */
struct liveupdate_session_set_event {
	__u32		size;
	__u32		event;
};

#define LIVEUPDATE_SESSION_SET_EVENT					\
	_IO(LIVEUPDATE_IOCTL_TYPE, LIVEUPDATE_CMD_SESSION_SET_EVENT)

#endif /* _UAPI_LIVEUPDATE_H */
