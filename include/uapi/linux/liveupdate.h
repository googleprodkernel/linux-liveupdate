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

#endif /* _UAPI_LIVEUPDATE_H */
