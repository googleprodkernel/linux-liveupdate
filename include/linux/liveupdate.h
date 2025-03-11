/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef _LINUX_LIVEUPDATE_H
#define _LINUX_LIVEUPDATE_H

#include <linux/compiler.h>
#include <linux/notifier.h>

/**
 * enum liveupdate_event - Events that trigger live update callbacks.
 * @LIVEUPDATE_PREPARE: Sent when the live update process is initiated via
 *                      a sysfs by writing '1' into
 *                      ``/sys/kernel/liveupdate/prepare``. This happens
 *                      *before* the blackout window. Subsystems should prepare
 *                      for an upcoming reboot by serializing their states.
 *                      However, it must be considered that user applications,
 *                      e.g. virtual machines are still running during this
 *                      phase.
 * @LIVEUPDATE_REBOOT:  Sent from the reboot() syscall, when the old kernel is
 *                      on its way out. This is the final opportunity for
 *                      subsystems to save any state that must persist across
 *                      the reboot. Callbacks for this event are part of the
 *                      blackout window and must be fast.
 * @LIVEUPDATE_FINISH:  Sent in the newly booted kernel after a successful live
 *                      update and *after* the blackout window. This event is
 *                      initiated by writing '1' into
 *                      ``/sys/kernel/liveupdate/prepare``. Subsystems should
 *                      perform any final cleanup during this phase. This phase
 *                      also provides an opportunity to clean up devices that
 *                      were preserved but never explicitly reclaimed during the
 *                      live update process. State restoration should have
 *                      already occurred before this event. Callbacks for this
 *                      event must not fail. The completion of this call
 *                      transitions the machine from ``updated`` to ``normal``
 *                      state.
 * @LIVEUPDATE_CANCEL:  Sent if the LIVEUPDATE_PREPARE or LIVEUPDATE_REBOOT
 *                      stage fails. Subsystems should revert any actions taken
 *                      during the corresponding prepare phase. Callbacks for
 *                      this event must not fail.
 *
 * These events represent the different stages and actions within the live
 * update process that subsystems (like device drivers and bus drivers)
 * need to be aware of to correctly serialize and restore their state.
 *
 */
enum liveupdate_event {
	LIVEUPDATE_PREPARE,
	LIVEUPDATE_REBOOT,
	LIVEUPDATE_FINISH,
	LIVEUPDATE_CANCEL,
};

/**
 * enum liveupdate_state - Defines the possible states of the live update
 * orchestrator.
 * @LIVEUPDATE_STATE_NORMAL:         Default state, no live update in progress.
 * @LIVEUPDATE_STATE_PREPARED:       Live update is prepared for reboot; the
 *                                   LIVEUPDATE_PREPARE callbacks have completed
 *                                   successfully.
 *                                   Devices might operate in a limited state
 *                                   for example the participating devices might
 *                                   not be allowed to unbind, and also the
 *                                   setting up of new DMA mappings might be
 *                                   disabled in this state.
 * @LIVEUPDATE_STATE_UPDATED:        The system has rebooted into a new kernel
 *                                   via live update the system is now running
 *                                   the new kernel, awaiting the finish stage.
 *
 * These states track the progress and outcome of a live update operation.
 */
enum liveupdate_state  {
	LIVEUPDATE_STATE_NORMAL,
	LIVEUPDATE_STATE_PREPARED,
	LIVEUPDATE_STATE_UPDATED,
};

/**
 * enum liveupdate_cb_priority - Priority levels for live update notifiers.
 * @LIVEUPDATE_CB_PRIO_BEFORE_DEVICES: Callbacks with this priority will be
 *                                     executed before the device layer
 *                                     callbacks.
 * @LIVEUPDATE_CB_PRIO_WITH_DEVICES:   Callbacks with this priority will be
 *                                     executed at the same time as the device
 *                                     layer callbacks.
 * @LIVEUPDATE_CB_PRIO_AFTER_DEVICES:  Callbacks with this priority will be
 *                                     executed after the device layer
 *                                     callbacks.
 *
 * This enum defines the priority levels for notifier callbacks registered with
 * the live update orchestrator. It allows subsystems to control the order in
 * which their callbacks are executed relative to other subsystems during the
 * live update process.
 */
enum liveupdate_cb_priority {
	LIVEUPDATE_CB_PRIO_BEFORE_DEVICES,
	LIVEUPDATE_CB_PRIO_WITH_DEVICES,
	LIVEUPDATE_CB_PRIO_AFTER_DEVICES,
};

#ifdef CONFIG_LIVEUPDATE

/* Called during reboot to notify subsystems to complete serialization */
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

/* Protect live update state with a rwsem, take it as a reader */
int liveupdate_read_state_enter_killable(void);
void liveupdate_read_state_enter(void);
void liveupdate_read_state_exit(void);

/* Return true if live update orchestrator is enabled */
bool liveupdate_enabled(void);

int liveupdate_register_notifier(struct notifier_block *nb);
int liveupdate_unregister_notifier(struct notifier_block *nb);

/**
 * LIVEUPDATE_DECLARE_NOTIFIER - Declare a live update notifier with default
 * structure.
 * @_name: A base name used to generate the names of the notifier block
 * (e.g., ``_name##_liveupdate_notifier_block``) and the callback function
 * (e.g., ``_name##_liveupdate``).
 * @_priority: The priority of the notifier, specified using the
 * ``enum liveupdate_cb_priority`` values
 * (e.g., ``LIVEUPDATE_CB_PRIO_BEFORE_DEVICES``).
 *
 * This macro declares a static struct notifier_block and a corresponding
 * notifier callback function for use with the live update orchestrator.
 * It simplifies the process by automatically handling the dispatching of
 * live update events to separate handler functions for prepare, reboot,
 * finish, and cancel.
 *
 * This macro expects the following functions to be defined:
 *
 * ``_name##_liveupdate_prepare()``:  Called on LIVEUPDATE_PREPARE.
 * ``_name##_liveupdate_reboot()``:   Called on LIVEUPDATE_REBOOT.
 * ``_name##_liveupdate_finish()``:   Called on LIVEUPDATE_FINISH.
 * ``_name##_liveupdate_cancel()``:   Called on LIVEUPDATE_CANCEL.
 *
 * The generated callback function handles the switch statement for the
 * different live update events and calls the appropriate handler function.
 * It also includes warnings if the finish or cancel handlers return an error.
 *
 * For example, declartion can look like this:
 *
 * ``static int foo_liveupdate_prepare(void) { ... }``
 *
 * ``static int foo_liveupdate_reboot(void) { ... }``
 *
 * ``static int foo_liveupdate_finish(void) { ... }``
 *
 * ``static int foo_liveupdate_cancel(void) { ... }``
 *
 * ``LIVEUPDATE_DECLARE_NOTIFIER(foo, LIVEUPDATE_CB_PRIO_WITH_DEVICES);``
 *
 */
#define LIVEUPDATE_DECLARE_NOTIFIER(_name, _priority)			\
static int _name##_liveupdate(struct notifier_block *nb,		\
			      unsigned long action,			\
			      void *data)				\
{									\
	enum liveupdate_event event = (enum liveupdate_event)action;	\
	int err = 0;							\
	int rv;								\
									\
	switch (event) {						\
	case LIVEUPDATE_PREPARE:					\
		err = _name##_liveupdate_prepare();			\
		break;							\
	case LIVEUPDATE_REBOOT:						\
		err = _name##_liveupdate_reboot();			\
		break;							\
	case LIVEUPDATE_FINISH:						\
		rv = _name##_liveupdate_finish();			\
		WARN_ONCE(rv, "finish failed[%d]\n", rv);		\
		break;							\
	case LIVEUPDATE_CANCEL:						\
		rv = _name##_liveupdate_cancel();			\
		WARN_ONCE(rv, "cancel failed[%d]\n", rv);		\
		break;							\
	default:							\
		WARN_ONCE(1, "unexpected event[%d]\n", event);		\
		return NOTIFY_DONE;					\
	}								\
									\
	return notifier_from_errno(err);				\
}									\
									\
static struct notifier_block _name##_liveupdate_notifier_block = {	\
	.notifier_call = _name##_liveupdate,				\
	.priority = _priority,						\
}

/**
 * LIVEUPDATE_REGISTER_NOTIFIER - Register a live update notifier declared with
 * the macro.
 * @_name: The base name used when declaring the notifier with
 * ``LIVEUPDATE_DECLARE_NOTIFIER``.
 *
 * This macro simplifies the registration of a notifier block that was
 * declared using the LIVEUPDATE_DECLARE_NOTIFIER macro.
 */
#define LIVEUPDATE_REGISTER_NOTIFIER(_name)				\
	liveupdate_register_notifier(&_name##_liveupdate_notifier_block)

#else /* CONFIG_LIVEUPDATE */

static inline int liveupdate_reboot(void)
{
	return 0;
}

static inline int liveupdate_register_notifier(struct notifier_block *nb)
{
	return 0;
}

static inline int liveupdate_unregister_notifier(struct notifier_block *nb)
{
	return 0;
}

#endif /* CONFIG_LIVEUPDATE */
#endif /* _LINUX_LIVEUPDATE_H */
