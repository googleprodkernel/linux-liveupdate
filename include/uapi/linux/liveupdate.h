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
 * struct liveupdate_fd - Holds parameters for preserving and restoring file
 * descriptors across live update.
 * @fd:    Input for %LIVEUPDATE_IOCTL_FD_PRESERVE: The user-space file
 *         descriptor to be preserved.
 *         Output for %LIVEUPDATE_IOCTL_FD_RESTORE: The new file descriptor
 *         representing the fully restored kernel resource.
 * @flags: Unused, reserved for future expansion, must be set to 0.
 * @token: Input for %LIVEUPDATE_IOCTL_FD_PRESERVE: An opaque, unique token
 *         preserved for preserved resource.
 *         Input for %LIVEUPDATE_IOCTL_FD_RESTORE: The token previously
 *         provided to the preserve ioctl for the resource to be restored.
 *
 * This structure is used as the argument for the %LIVEUPDATE_IOCTL_FD_PRESERVE
 * and %LIVEUPDATE_IOCTL_FD_RESTORE ioctls. These ioctls allow specific types
 * of file descriptors (for example memfd, kvm, iommufd, and VFIO) to have their
 * underlying kernel state preserved across a live update cycle.
 *
 * To preserve an FD, user space passes this struct to
 * %LIVEUPDATE_IOCTL_FD_PRESERVE with the @fd field set. On success, the
 * kernel uses the @token field to uniquly associate the preserved FD.
 *
 * After the live update transition, user space passes the struct populated with
 * the *same* @token to %LIVEUPDATE_IOCTL_FD_RESTORE. The kernel uses the @token
 * to find the preserved state and, on success, populates the @fd field with a
 * new file descriptor referring to the restored resource.
 */
struct liveupdate_fd {
	int		fd;
	__u32		flags;
	__aligned_u64	token;
};

/* The ioctl type, documented in ioctl-number.rst */
#define LIVEUPDATE_IOCTL_TYPE		0xBA

/**
 * LIVEUPDATE_IOCTL_FD_PRESERVE - Validate and initiate preservation for a file
 * descriptor.
 *
 * Argument: Pointer to &struct liveupdate_fd.
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
 * the subsequent %LIVEUPDATE_IOCTL_FD_RESTORE call after the live update.
 *
 * Return: 0 on success (validation passed, preservation initiated), negative
 * error code on failure (e.g., unsupported FD type, dependency issue,
 * validation failed).
 */
#define LIVEUPDATE_IOCTL_FD_PRESERVE					\
	_IOW(LIVEUPDATE_IOCTL_TYPE, 0x00, struct liveupdate_fd)

/**
 * LIVEUPDATE_IOCTL_FD_UNPRESERVE - Remove a file descriptor from the
 * preservation list.
 *
 * Argument: Pointer to __u64 token.
 *
 * Allows user space to explicitly remove a file descriptor from the set of
 * items marked as potentially preservable. User space provides a pointer to the
 * __u64 @token that was previously returned by a successful
 * %LIVEUPDATE_IOCTL_FD_PRESERVE call (potentially from a prior, possibly
 * cancelled, live update attempt). The kernel reads the token value from the
 * provided user-space address.
 *
 * On success, the kernel removes the corresponding entry (identified by the
 * token value read from the user pointer) from its internal preservation list.
 * The provided @token (representing the now-removed entry) becomes invalid
 * after this call.
 *
 * Return: 0 on success, negative error code on failure (e.g., -EBUSY or -EINVAL
 * if not in %LIVEUPDATE_STATE_NORMAL, bad address provided, invalid token value
 * read, token not found).
 */
#define LIVEUPDATE_IOCTL_FD_UNPRESERVE					\
	_IOW(LIVEUPDATE_IOCTL_TYPE, 0x01, __u64)

/**
 * LIVEUPDATE_IOCTL_FD_RESTORE - Restore a previously preserved file descriptor.
 *
 * Argument: Pointer to &struct liveupdate_fd.
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
#define LIVEUPDATE_IOCTL_FD_RESTORE					\
	_IOWR(LIVEUPDATE_IOCTL_TYPE, 0x02, struct liveupdate_fd)

/**
 * LIVEUPDATE_IOCTL_GET_STATE - Query the current state of the live update
 * orchestrator.
 *
 * Argument: Pointer to &enum liveupdate_state.
 *
 * The kernel fills the enum value pointed to by the argument with the current
 * state of the live update subsystem. Possible states are:
 *
 * - %LIVEUPDATE_STATE_NORMAL:   Default state; no live update operation is
 *                               currently in progress.
 * - %LIVEUPDATE_STATE_PREPARED: The preparation phase (triggered by
 *                               %LIVEUPDATE_IOCTL_PREPARE) has completed
 *                               successfully. The system is ready for the
 *                               reboot transition. Note that some
 *                               device operations (e.g., unbinding, new DMA
 *                               mappings) might be restricted in this state.
 * - %LIVEUPDATE_STATE_UPDATED:  The system has successfully rebooted into the
 *                               new kernel via live update. It is now running
 *                               the new kernel code and is awaiting the
 *                               completion signal from user space via
 *                               %LIVEUPDATE_IOCTL_FINISH after
 *                               restoration tasks are done.
 *
 * See the definition of &enum liveupdate_state for more details on each state.
 *
 * Return: 0 on success, negative error code on failure.
 */
#define LIVEUPDATE_IOCTL_GET_STATE					\
	_IOR(LIVEUPDATE_IOCTL_TYPE, 0x03, enum liveupdate_state)

/**
 * LIVEUPDATE_IOCTL_PREPARE - Initiate preparation phase and trigger state
 * saving.
 *
 * Argument: None.
 *
 * Initiates the live update preparation phase. This action corresponds to
 * the internal %LIVEUPDATE_PREPARE. This typically triggers the saving process
 * for items marked via the PRESERVE ioctls. This typically occurs *before*
 * the "blackout window", while user applications (e.g., VMs) may still be
 * running. Kernel subsystems receiving the %LIVEUPDATE_PREPARE event should
 * serialize necessary state. This command does not transfer data.
 *
 * Return: 0 on success, negative error code on failure. Transitions state
 * towards %LIVEUPDATE_STATE_PREPARED on success.
 */
#define LIVEUPDATE_IOCTL_PREPARE					\
	_IO(LIVEUPDATE_IOCTL_TYPE, 0x04)

/**
 * LIVEUPDATE_IOCTL_CANCEL - Cancel the live update preparation phase.
 *
 * Argument: None.
 *
 * Notifies the live update subsystem to abort the preparation sequence
 * potentially initiated by %LIVEUPDATE_IOCTL_PREPARE. This action
 * typically corresponds to the internal %LIVEUPDATE_CANCEL kernel event,
 * which might also be triggered automatically if the PREPARE stage fails
 * internally.
 *
 * When triggered, subsystems receiving the %LIVEUPDATE_CANCEL event should
 * revert any state changes or actions taken specifically for the aborted
 * prepare phase (e.g., discard partially serialized state). The kernel
 * releases resources allocated specifically for this *aborted preparation
 * attempt*.
 *
 * This operation cancels the current *attempt* to prepare for a live update
 * but does **not** remove previously validated items from the internal list
 * of potentially preservable resources. Consequently, preservation tokens
 * previously generated by successful %LIVEUPDATE_IOCTL_FD_PRESERVE or calls
 * generally **remain valid** as identifiers for those potentially preservable
 * resources. However, since the system state returns towards
 * %LIVEUPDATE_STATE_NORMAL, user space must initiate a new live update sequence
 * (starting with %LIVEUPDATE_IOCTL_PREPARE) to proceed with an update
 * using these (or other) tokens.
 *
 * This command does not transfer data. Kernel callbacks for the
 * %LIVEUPDATE_CANCEL event must not fail.
 *
 * Return: 0 on success, negative error code on failure. Transitions state back
 * towards %LIVEUPDATE_STATE_NORMAL on success.
 */
#define LIVEUPDATE_IOCTL_CANCEL						\
	_IO(LIVEUPDATE_IOCTL_TYPE, 0x06)

/**
 * LIVEUPDATE_IOCTL_EVENT_FINISH - Signal restoration completion and trigger
 * cleanup.
 *
 * Argument: None.
 *
 * Signals that user space has completed all necessary restoration actions in
 * the new kernel (after a live update reboot). This action corresponds to the
 * internal %LIVEUPDATE_FINISH kernel event. Calling this ioctl triggers the
 * cleanup phase: any resources that were successfully preserved but were *not*
 * subsequently restored (reclaimed) via the RESTORE ioctls will have their
 * preserved state discarded and associated kernel resources released. Involved
 * devices may be reset. All desired restorations *must* be completed *before*
 * this. Kernel callbacks for the %LIVEUPDATE_FINISH event must not fail.
 * Successfully completing this phase transitions the system state from
 * %LIVEUPDATE_STATE_UPDATED back to %LIVEUPDATE_STATE_NORMAL. This command does
 * not transfer data.
 *
 * Return: 0 on success, negative error code on failure.
 */
#define LIVEUPDATE_IOCTL_FINISH						\
	_IO(LIVEUPDATE_IOCTL_TYPE, 0x07)

#endif /* _UAPI_LIVEUPDATE_H */
