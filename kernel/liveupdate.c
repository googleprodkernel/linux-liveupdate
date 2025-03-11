// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: Live Update Orchestrator (LUO)
 *
 * Live Update is a specialized reboot process where selected devices are
 * kept operational across a kernel transition. For these devices, DMA and
 * interrupt activity may continue uninterrupted during the kernel reboot.
 *
 * The primary use case is in cloud environments, allowing hypervisor updates
 * without disrupting running virtual machines. During a live update, VMs can be
 * suspended (with their state preserved in memory), while the hypervisor kernel
 * reboots. Devices attached to these VMs (e.g., NICs, block devices) are kept
 * operational by the LUO during the hypervisor reboot, allowing the VMs to be
 * quickly resumed on the new kernel.
 *
 * Various kernel subsystems register with the Live Update Orchestrator to
 * participate in the live update process. These subsystems are notified at
 * different stages of the live update sequence, allowing them to serialize
 * device state before the reboot and restore it afterwards. Examples include
 * the device layer, interrupt controllers, KVM, IOMMU, and specific device
 * drivers.
 *
 * The core of LUO is a state machine that tracks the progress of a live update,
 * along with a callback API that allows other kernel subsystems to participate
 * in the process. Example subsystems that can hook into LUO include: kvm,
 * iommu, interrupts, Documentation/driver-api/liveupdate.rst, participating
 * filesystems, and mm.
 *
 * LUO uses KHO to transfer memory state from Old Kernel to the New Kernel.
 *
 * LUO can be controlled through sysfs interface. It provides the following
 * files under: ``/sys/kernel/liveupdate/{state, prepare, cancel}``
 *
 * The ``state`` file can contain the following values:
 *
 * ``normal``
 *   The system is operating normally, and no live update is in progress.
 *   This is the initial state.
 * ``prepared``
 *   The system has begun preparing for a live update. This state is reached
 *   after subsystems have successfully responded to the ``LIVEUPDATE_PREPARE``
 *   callback. It indicates that initial preparation is done, but it does not
 *   necessarily mean all state has been serialized; subsystems can save more
 *   state during the subsequent ``LIVEUPDATE_REBOOT`` callback.
 * ``updated``
 *   The new kernel has successfully taken over, and any suspended operations
 *   are resumed. However, the system has not yet fully transitioned back to
 *   a normal operational state; this happens after the ``LIVEUPDATE_FINISH``
 *   callback is invoked.
 *
 * The state machine ensures that operations are performed in the correct
 * sequence and provides a mechanism to track and recover from potential
 * failures, and select devices and subsystems that should participate in
 * live update sequence.
 *
 */

 #undef pr_fmt
 #define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#undef pr_fmt
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/rwsem.h>
#include <linux/err.h>
#include <linux/liveupdate.h>
#include <linux/cpufreq.h>
#include <linux/kexec_handover.h>

#define LUO_KHO_NODE_NAME		"liveupdate_orchestrator"
#define LUO_KHO_VERSION_PROP_NAME	"version"
#define LUO_VERSION_MAJOR		1
#define LUO_VERSION_MINOR		0

/* 'version' property */
struct luo_kho_version_prop {
	u32 major;
	u32 minor;
};

static const struct luo_kho_version_prop luo_version = {
	.major = LUO_VERSION_MAJOR,
	.minor = LUO_VERSION_MINOR,
};

static struct kho_node luo_node = KHO_NODE_INIT;
static enum liveupdate_state luo_state;
static DECLARE_RWSEM(luo_state_rwsem);
static BLOCKING_NOTIFIER_HEAD(luo_notify_list);

static const char *const luo_event_str[] = {
	"PREPARE",
	"REBOOT",
	"FINISH",
	"CANCEL",
};

static const char *const luo_state_str[] = {
	"normal",
	"prepared",
	"updated",
};

static bool luo_enabled;
static bool luo_sysfs_initialized;

static int __init early_liveupdate_param(char *buf)
{
	return kstrtobool(buf, &luo_enabled);
}

early_param("liveupdate", early_liveupdate_param);

/* Return true if the current state is equal to the provided state */
#define IS_STATE(state) (READ_ONCE(luo_state) == (state))

/* Get the current state as a string */
#define LUO_STATE_STR luo_state_str[READ_ONCE(luo_state)]

static void __luo_set_state(enum liveupdate_state state)
{
	WRITE_ONCE(luo_state, state);
	if (luo_sysfs_initialized)
		sysfs_notify(kernel_kobj, NULL, "state");
}

static inline void luo_set_state(enum liveupdate_state state)
{
	pr_info("Switched from [%s] to [%s] state\n",
		LUO_STATE_STR, luo_state_str[state]);
	__luo_set_state(state);
}

/* Show the current live update state */
static ssize_t state_show(struct kobject *kobj,
			  struct kobj_attribute *attr,
			  char *buf)
{
	return sysfs_emit(buf, "%s\n", LUO_STATE_STR);
}

/**
 * luo_notify - Call registered notifiers for a live update event.
 * @event: The live update event to notify subsystems about.
 *
 * This function is notifying registered subsystems about the specified event.
 *
 * For ``LIVEUPDATE_PREPARE`` event, it uses
 * ``blocking_notifier_call_chain_robust()`` to ensure that if a notifier
 * callback fails, a corresponding ``LIVEUPDATE_CANCEL`` notification is sent
 * to already-notified subsystems, allowing for a rollback.
 *
 * For ``LIVEUPDATE_REBOOT`` event, it uses ``blocking_notifier_call_chain()``
 * and if it returns a failure, cancels the operation via calling
 * ``lou_notify(LIVEUPDATE_CANCEL)`` to notify every subsystem to transition
 * back to ``normal`` state.
 *
 * For ``LIVEUPDATE_FINISH`` and ``LIVEUPDATE_CANCEL`` events, it uses the
 * standard ``blocking_notifier_call_chain()``. These events are expected not to
 * fail, and a warning is printed if they do.
 *
 * @return 0 on success, or the negative error code returned by the failing
 * notifier callback (for ``LIVEUPDATE_PREPARE`` and ``LIVEUPDATE_REBOOT``), or
 * 0 for ``LIVEUPDATE_FINISH`` and ``LIVEUPDATE_CANCEL`` even if a warning was
 * printed due to a callback failure.
 */
static int luo_notify(enum liveupdate_event event)
{
	int ret;

	if (event == LIVEUPDATE_PREPARE) {
		ret = blocking_notifier_call_chain_robust(&luo_notify_list,
							  LIVEUPDATE_PREPARE,
							  LIVEUPDATE_CANCEL,
							  NULL);
	} else if (event == LIVEUPDATE_REBOOT) {
		ret = blocking_notifier_call_chain(&luo_notify_list,
						   LIVEUPDATE_REBOOT,
						   NULL);
		/*
		 * For LIVEUPDATE_REBOOT do CANCEL for everyone, so even
		 * prepared subsystems return back to the normal state
		 */
		if (notifier_to_errno(ret))
			luo_notify(LIVEUPDATE_CANCEL);
	} else {
		ret = blocking_notifier_call_chain(&luo_notify_list,
						   event,
						   NULL);
		/* Cancel and finish must not fail, warn and return success */
		WARN_ONCE(notifier_to_errno(ret), "Callback failed event: %s [%d]\n",
			  luo_event_str[event], notifier_to_errno(ret));
		ret = 0;
	}

	return notifier_to_errno(ret);
}

/**
 * luo_prepare - Initiate the live update preparation phase.
 *
 * This function is called to begin the live update process. It attempts to
 * transition the luo to the ``LIVEUPDATE_STATE_PREPARED`` state.
 *
 * It first acquires the write lock for the orchestrator state. Then, it checks
 * if the current state is ``LIVEUPDATE_STATE_NORMAL``. If not, it returns an
 * error. If the state is normal, it triggers the ``LIVEUPDATE_PREPARE``
 * notifier chain.
 *
 * If the notifier chain completes successfully, the orchestrator state is set
 * to ``LIVEUPDATE_STATE_PREPARED``. If any notifier callback fails a
 * ``LIVEUPDATE_CANCEL`` notification is sent to roll back any actions.
 *
 * @return 0 on success, ``-EAGAIN`` if the state change was cancelled by the
 * user while waiting for the lock, ``-EINVAL`` if the orchestrator is not in
 * the normal state, or a negative error code returned by the notifier chain.
 */
static int luo_prepare(void)
{
	int ret;

	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn(" %s, change state canceled by user\n", __func__);
		return -EAGAIN;
	}

	if (!IS_STATE(LIVEUPDATE_STATE_NORMAL)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_PREPARED],
			LUO_STATE_STR);
		up_write(&luo_state_rwsem);

		return -EINVAL;
	}

	ret = luo_notify(LIVEUPDATE_PREPARE);
	if (!ret)
		luo_set_state(LIVEUPDATE_STATE_PREPARED);

	up_write(&luo_state_rwsem);

	return ret;
}

/**
 * luo_finish - Finalize the live update process in the new kernel.
 *
 * This function is called  after a successful live update reboot into a new
 * kernel, once the new kernel is ready to transition to the normal operational
 * state. It signals the completion of the live update sequence to subsystems.
 *
 * It first attempts to acquire the write lock for the orchestrator state.
 *
 * Then, it checks if the system is in the ``LIVEUPDATE_STATE_UPDATED`` state.
 * If not, it logs a warning and returns ``-EINVAL``.
 *
 * If the state is correct, it triggers the ``LIVEUPDATE_FINISH`` notifier
 * chain. Note that the return value of the notifier is intentionally ignored as
 * finish callbacks must not fail. Finally, the orchestrator state is
 * transitioned back to ``LIVEUPDATE_STATE_NORMAL``, indicating the end of the
 * live update process.
 *
 * @return 0 on success, ``-EAGAIN`` if the state change was cancelled by the
 * user while waiting for the lock, or ``-EINVAL`` if the orchestrator is not in
 * the updated state.
 */
static int luo_finish(void)
{
	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn(" %s, change state canceled by user\n", __func__);
		return -EAGAIN;
	}

	if (!IS_STATE(LIVEUPDATE_STATE_UPDATED)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_NORMAL],
			LUO_STATE_STR);
		up_write(&luo_state_rwsem);

		return -EINVAL;
	}

	(void)luo_notify(LIVEUPDATE_FINISH);
	luo_set_state(LIVEUPDATE_STATE_NORMAL);

	up_write(&luo_state_rwsem);

	return 0;
}

/**
 * luo_cancel - Cancel the ongoing live update preparation or reboot states.
 *
 * This function is called to abort a live update that is currently in the
 * ``LIVEUPDATE_STATE_PREPARED`` state. It can be triggered either
 * programmatically or via the sysfs interface.
 *
 * If the state is correct, it triggers the ``LIVEUPDATE_CANCEL`` notifier chain
 * to allow subsystems to undo any actions performed during the prepare or
 * reboot phase. Finally, the orchestrator state is transitioned back to
 * ``LIVEUPDATE_STATE_NORMAL``.
 *
 * @return 0 on success, or ``-EAGAIN`` if the state change was cancelled by the
 * user while waiting for the lock.
 */
static int luo_cancel(void)
{
	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn(" %s, change state canceled by user\n", __func__);
		return -EAGAIN;
	}

	if (!IS_STATE(LIVEUPDATE_STATE_PREPARED)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_NORMAL],
			LUO_STATE_STR);
		up_write(&luo_state_rwsem);

		return -EINVAL;
	}

	(void)luo_notify(LIVEUPDATE_CANCEL);
	luo_set_state(LIVEUPDATE_STATE_NORMAL);

	up_write(&luo_state_rwsem);

	return 0;
}

/**
 * prepare_store - store method for starting live update prepare state or go
 * back to normal from a prepared state.
 * @kobj: The kobject associated with luo.
 * @attr: The sysfs attribute
 * @buf: The buffer containing the value written by the user.
 * @count: The number of bytes written.
 *
 * This function is the store method for the 'prepare' file under the
 * 'liveupdate' sysfs directory.
 *
 * Writing "1" to this attribute will trigger the luo_prepare() function,
 * attempting to start the live update preparation phase.
 *
 * Writing "0" to this attribute will trigger the luo_cancel() function,
 * attempting to cancel the orchestrator to the normal state.
 *
 * @return The number of bytes processed on success, or a negative error code
 * if the input is invalid or if the underlying functions fail.
 */
static ssize_t prepare_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf,
			     size_t count)
{
	ssize_t ret;
	long val;

	if (kstrtol(buf, 0, &val) < 0)
		return -EINVAL;

	if (val != 1 && val != 0)
		return -EINVAL;

	if (val)
		ret = luo_prepare();
	else
		ret = luo_cancel();

	if (!ret)
		ret = count;

	return ret;
}

/**
 * finish_store - store method for finalizing a live update.
 * @kobj: The kobject associated with the luo.
 * @attr: The sysfs attribute
 * @buf: The buffer containing the value written by the user.
 * @count: The number of bytes written.
 *
 * This function is the store method for the ``finish`` file under the
 * ``liveupdate`` sysfs directory.
 *
 * Writing "1" to this attribute will trigger the luo_finish() function,
 * attempting to finalize the live update process in the new kernel and
 * transition to the normal state.
 *
 * @return The number of bytes processed on success, or a negative error code
 * if the input is invalid or if luo_finish() fails.
 */
static ssize_t finish_store(struct kobject *kobj,
			    struct kobj_attribute *attr,
			    const char *buf,
			    size_t count)
{
	ssize_t ret;
	long val;

	if (kstrtol(buf, 0, &val) < 0)
		return -EINVAL;

	if (val != 1)
		return -EINVAL;

	ret = luo_finish();
	if (!ret)
		ret = count;

	return ret;
}

static struct kobj_attribute state_attribute = __ATTR_RO(state);
static struct kobj_attribute prepare_attribute = __ATTR_WO(prepare);
static struct kobj_attribute finish_attribute = __ATTR_WO(finish);

static struct attribute *luo_attrs[] = {
	&state_attribute.attr,
	&prepare_attribute.attr,
	&finish_attribute.attr,
	NULL,
};

static struct attribute_group luo_attr_group = {
	.attrs = luo_attrs,
	.name = "liveupdate",
};

/**
 * luo_init - Initialize the Live Update Orchestrator sysfs interface.
 *
 * This function is called during the kernel's late initialization phase
 * (``late_initcall``). It is responsible for creating the sysfs interface
 * that allows user-space to interact with the Live Update Orchestrator.
 *
 * If the "liveupdate" feature is enabled (checked via luo_enabled()), this
 * function creates a sysfs directory named ``liveupdate`` under the kernel's
 * top-level sysfs directory (``/sys/kernel/``).
 *
 * It then creates the following sysfs attribute files within the
 * ``/sys/kernel/liveupdate/`` directory:
 *
 * - ``prepare``: Writing '1' initiates preparation, '0' cancels.
 * - ``finish``:  Writing '1' finalizes the update in the new kernel.
 * - ``state``:   Read-only file displaying the current orchestrator state.
 *
 * @return 0 on success, or a negative error code if sysfs directory or
 * attribute creation fails.
 */
static int __init luo_init(void)
{
	int ret;

	if (!luo_enabled || !kho_is_enabled()) {
		pr_info("disabled by user\n");
		luo_enabled = false;

		return 0;
	}

	ret = sysfs_create_group(kernel_kobj, &luo_attr_group);
	if (ret)
		pr_err("Failed to create group\n");

	luo_sysfs_initialized = true;
	pr_info("Initialized\n");

	return ret;
}
subsys_initcall(luo_init);

/**
 * luo_startup - Initialize the Live Update Orchestrator on live update boot.
 *
 * This function is called during the kernel's early initialization phase
 * (early_initcall). Its primary role is to detect if the system is booting
 * as part of a live update sequence by checking for the presence of a
 * luo node in the kho tree.
 *
 * If a kho node named ``liveupdate_orchestrator`` is found, the function
 * extracts the version information from the previous kernel. It then performs
 * the following checks to ensure a safe continuation of the live update:
 *
 * 1. Verifies the size of the version property.
 * 2. Compares the major version and checks if the minor version of the
 *    previous orchestrator is compatible with the current one. If a mismatch
 *    is detected, the system panics to prevent potential memory corruption.
 * 3. Checks if the ``liveupdate`` kernel command-line parameter has enabled
 *    the feature. If the kho node exists but the feature is disabled, the
 *    system panics.
 *
 * If all checks pass, the orchestrator state is set to
 * ``LIVEUPDATE_STATE_UPDATED``.
 *
 * @return 0 always.
 */
static int __init luo_startup(void)
{
	enum liveupdate_state state = LIVEUPDATE_STATE_NORMAL;
	const struct luo_kho_version_prop *p;
	struct kho_in_node luo_node;
	int len;

	if (kho_get_node(NULL, LUO_KHO_NODE_NAME, &luo_node) < 0)
		goto no_liveupdate;

	p = kho_get_prop(&luo_node, LUO_KHO_VERSION_PROP_NAME, &len);
	if (len != sizeof(struct luo_kho_version_prop)) {
		panic("Unexcpected version property size, excpected[%ld] found[%d]\n",
		      sizeof(struct luo_kho_version_prop), len);
	}

	/*
	 * Panic if feature is disabled or version mismatch, we do not want
	 * memory corruptions due to DMA or interrupt tables activity.
	 */
	if (p->major != LUO_VERSION_MAJOR ||
	    p->minor > LUO_VERSION_MINOR) {
		pr_err("prev orchestrator version (%d.%d)\n",
		       p->major, p->minor);
		pr_err("new orchestrator version (%d.%d)\n",
		       LUO_VERSION_MAJOR, LUO_VERSION_MINOR);
		panic("Orchestrator version mismatch\n");
	}

	if (!luo_enabled)
		panic("Live update node found, but feature is disabled\n");

	state = LIVEUPDATE_STATE_UPDATED;
	pr_info("live update boot\n");

no_liveupdate:
	__luo_set_state(state);

	return 0;
}
early_initcall(luo_startup);

/* Public Functions */

/**
 * liveupdate_reboot - Notify subsystems to perform final serialization for live
 * update.
 *
 * This function is called directly from the reboot() syscall path when a live
 * update is prepared (i.e., the system is rebooting into a new kernel while
 * preserving devices). It is part of the "blackout" window where the old kernel
 * is transitioning to the new one.
 *
 * During this phase, the function iterates through the list of participating in
 * the live update subsystems and invokes their registered ``LIVEUPDATE_REBOOT``
 * callbacks. These callbacks *must* be extremely time-sensitive as they perform
 * the final serialization of device/subsystem state necessary to survive the
 * imminent kernel transition. Any delays here directly impact the duration of
 * the blackout window.
 *
 * If any callback fails, the live update process is aborted, and a
 * ``LIVEUPDATE_CANCEL`` notification is sent to all subsystems, that were
 * already notified and were not notified to bring machine back to the
 * ``LIVEUPDATE_NORMAL`` state..
 *
 * On success, the function adds a node to the KHO tree to indicate to the next
 * kernel that a live update is in progress.
 *
 * @return 0 on success, or a negative error code if a callback fails or if
 * adding the KHO node fails.
 */
int liveupdate_reboot(void)
{
	int ret;

	if (!IS_STATE(LIVEUPDATE_STATE_PREPARED))
		return 0;

	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn(" %s, change state canceled by user\n", __func__);
		return -EAGAIN;
	}

	ret = luo_notify(LIVEUPDATE_REBOOT);
	if (ret < 0) {
		luo_set_state(LIVEUPDATE_STATE_NORMAL);
	} else {
		/* Add live update orchestrator node to KHO tree */
		ret = kho_add_node(NULL, LUO_KHO_NODE_NAME, &luo_node);
		if (!ret) {
			ret = kho_add_prop(&luo_node, LUO_KHO_VERSION_PROP_NAME,
					   &luo_version, sizeof(luo_version));
		}

		if (ret) {
			(void)luo_notify(LIVEUPDATE_CANCEL);
			luo_set_state(LIVEUPDATE_STATE_NORMAL);
		}
	}

	up_write(&luo_state_rwsem);

	if (ret)
		pr_warn("%s failed: %d\n", __func__, ret);

	return ret;
}

/**
 * liveupdate_state_updated - Check if the system is in the live update
 * 'updated' state.
 *
 * This function checks if the live update orchestrator is in the
 * ``LIVEUPDATE_STATE_UPDATED`` state. This state indicates that the system has
 * successfully rebooted into a new kernel as part of a live update, and the
 * preserved devices are expected to be in the process of being reclaimed.
 *
 * This is typically used by subsystems during early boot of the new kernel
 * to determine if they need to attempt to restore state from a previous
 * live update.
 *
 * @return true if the system is in the ``LIVEUPDATE_STATE_UPDATED`` state,
 * false otherwise.
 */
bool liveupdate_state_updated(void)
{
	return IS_STATE(LIVEUPDATE_STATE_UPDATED);
}
EXPORT_SYMBOL_GPL(liveupdate_state_updated);

/**
 * liveupdate_state_normal - Check if the system is in the live update 'normal'
 * state.
 *
 * This function checks if the live update orchestrator is in the
 * ``LIVEUPDATE_STATE_NORMAL`` state. This state indicates that no live update
 * is in progress. It represents the default operational state of the system.
 *
 * This can be used to gate actions that should only be performed when no
 * live update activity is occurring.
 *
 * @return true if the system is in the ``LIVEUPDATE_STATE_NORMAL`` state,
 * false otherwise.
 */
bool liveupdate_state_normal(void)
{
	return IS_STATE(LIVEUPDATE_STATE_NORMAL);
}
EXPORT_SYMBOL_GPL(liveupdate_state_normal);

/**
 * liveupdate_register_notifier - Register a notifier for live update events.
 *
 * This function registers a notifier block to receive callbacks for various
 * stages of the live update process. Notifiers are called when the live
 * update state changes, allowing subsystems to participate in the
 * serialization and restoration of state.
 *
 * @nb: Pointer to the notifier block to register.
 *
 * @return 0 on success, or a negative error code on failure (e.g., if
 * the notifier block is already registered).
 */
int liveupdate_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&luo_notify_list, nb);
}
EXPORT_SYMBOL_GPL(liveupdate_register_notifier);

/**
 * liveupdate_unregister_notifier - Unregister a live update event notifier.
 *
 * This function unregisters a previously registered notifier block from
 * receiving further callbacks for live update events.
 *
 * @nb: Pointer to the notifier block to unregister.
 *
 * @return 0 on success, or a negative error code if the notifier block
 * was not found.
 */
int liveupdate_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&luo_notify_list, nb);
}
EXPORT_SYMBOL_GPL(liveupdate_unregister_notifier);

/**
 * liveupdate_enabled - Check if the live update feature is enabled.
 *
 * This function returns the state of the live update feature flag, which
 * can be controlled via the ``liveupdate`` kernel command-line parameter.
 *
 * @return true if live update is enabled, false otherwise.
 */
bool liveupdate_enabled(void)
{
	return luo_enabled;
}
EXPORT_SYMBOL_GPL(liveupdate_enabled);

/**
 * liveupdate_read_state_enter_killable - Acquire the live update state read
 * lock (killable).
 *
 * This function attempts to acquire the read lock protecting the live update
 * orchestrator state. It allows multiple readers but excludes writers. The
 * call is interruptible by signals.
 *
 * Subsystems should acquire this lock if they need to read the live update
 * state and potentially perform actions based on it.
 *
 * Callers *must* call liveupdate_read_state_exit() to release the lock.
 *
 * @return 0 on success, or ``-EINTR`` if interrupted by a signal.
 */
int liveupdate_read_state_enter_killable(void)
{
	return down_read_killable(&luo_state_rwsem);
}
EXPORT_SYMBOL_GPL(liveupdate_read_state_enter_killable);

/**
 * liveupdate_read_state_enter - Acquire the live update state read lock.
 *
 * The same as liveupdate_read_state_enter_killable(), but not interruptable.
 */
void liveupdate_read_state_enter(void)
{
	down_read(&luo_state_rwsem);
}
EXPORT_SYMBOL_GPL(liveupdate_read_state_enter);

/**
 * liveupdate_read_state_exit - Release the live update state read lock.
 *
 * This function releases the read lock protecting the live update
 * orchestrator state. It must be called after a successful call to
 * liveupdate_read_state_enter_killable() or liveupdate_read_state_enter().
 */
void liveupdate_read_state_exit(void)
{
	up_read(&luo_state_rwsem);
}
EXPORT_SYMBOL_GPL(liveupdate_read_state_exit);
