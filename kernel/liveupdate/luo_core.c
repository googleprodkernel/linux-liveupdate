// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: Live Update Orchestrator (LUO)
 *
 * Live Update is a specialized, kexec-based reboot process that allows a
 * running kernel to be updated from one version to another while preserving
 * the state of selected resources and keeping designated hardware devices
 * operational. For these devices, DMA activity may continue throughout the
 * kernel transition.
 *
 * While the primary use case driving this work is supporting live updates of
 * the Linux kernel when it is used as a hypervisor in cloud environments, the
 * LUO framework itself is designed to be workload-agnostic. Much like Kernel
 * Live Patching, which applies security fixes regardless of the workload,
 * Live Update facilitates a full kernel version upgrade for any type of system.
 *
 * For example, a non-hypervisor system running an in-memory cache like
 * memcached with many gigabytes of data can use LUO. The userspace service
 * can place its cache into a memfd, have its state preserved by LUO, and
 * restore it immediately after the kernel kexec.
 *
 * Whether the system is running virtual machines, containers, a
 * high-performance database, or networking services, LUO's primary goal is to
 * enable a full kernel update by preserving critical userspace state and
 * keeping essential devices operational.
 *
 * The core of LUO is a state machine that tracks the progress of a live update,
 * along with a callback API that allows other kernel subsystems to participate
 * in the process. Example subsystems that can hook into LUO include: kvm,
 * iommu, interrupts, vfio, participating filesystems, and memory management.
 *
 * LUO uses Kexec Handover to transfer memory state from the current kernel to
 * the next kernel. For more details see
 * Documentation/core-api/kho/concepts.rst.
 *
 * The LUO state machine ensures that operations are performed in the correct
 * sequence and provides a mechanism to track and recover from potential
 * failures.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/kexec_handover.h>
#include <linux/kobject.h>
#include <linux/libfdt.h>
#include <linux/liveupdate.h>
#include <linux/mm.h>
#include <linux/rwsem.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include "luo_internal.h"

DECLARE_RWSEM(luo_state_rwsem);

static enum liveupdate_state luo_state = LIVEUPDATE_STATE_UNDEFINED;

static const char *const luo_state_str[] = {
	[LIVEUPDATE_STATE_UNDEFINED]	= "undefined",
	[LIVEUPDATE_STATE_NORMAL]	= "normal",
	[LIVEUPDATE_STATE_PREPARED]	= "prepared",
	[LIVEUPDATE_STATE_FROZEN]	= "frozen",
	[LIVEUPDATE_STATE_UPDATED]	= "updated",
};

static bool luo_enabled;

static void *luo_fdt_out;
static void *luo_fdt_in;

/*
 * The LUO FDT size depends on the number of participating subsystems,
 *
 * The current fixed size (4K) is large enough to handle reasonable number of
 * preserved entities. If this size ever becomes insufficient, it can either be
 * increased, or a dynamic size calculation mechanism could be implemented in
 * the future.
 */
#define LUO_FDT_SIZE		PAGE_SIZE
#define LUO_KHO_ENTRY_NAME	"LUO"
#define LUO_COMPATIBLE		"luo-v1"

static int __init early_liveupdate_param(char *buf)
{
	return kstrtobool(buf, &luo_enabled);
}
early_param("liveupdate", early_liveupdate_param);

/* Return true if the current state is equal to the provided state */
static inline bool is_current_luo_state(enum liveupdate_state expected_state)
{
	return liveupdate_get_state() == expected_state;
}

static void __luo_set_state(enum liveupdate_state state)
{
	WRITE_ONCE(luo_state, state);
}

static inline void luo_set_state(enum liveupdate_state state)
{
	pr_info("Switched from [%s] to [%s] state\n",
		luo_current_state_str(), luo_state_str[state]);
	__luo_set_state(state);
}

/* Called during the prepare phase, to create LUO fdt tree */
static int luo_fdt_setup(void)
{
	void *fdt_out;
	int ret;

	fdt_out = luo_contig_alloc_preserve(LUO_FDT_SIZE);
	if (IS_ERR(fdt_out)) {
		pr_err("failed to allocate/preserve FDT memory\n");
		return PTR_ERR(fdt_out);
	}

	ret = fdt_create_empty_tree(fdt_out, LUO_FDT_SIZE);
	if (ret)
		goto exit_free;

	ret = fdt_setprop_string(fdt_out, 0, "compatible", LUO_COMPATIBLE);
	if (ret)
		goto exit_free;

	ret = luo_subsystems_fdt_setup(fdt_out);
	if (ret)
		goto exit_free;

	ret = kho_add_subtree(LUO_KHO_ENTRY_NAME, fdt_out);
	if (ret)
		goto exit_free;
	luo_fdt_out = fdt_out;

	return 0;

exit_free:
	luo_contig_free_unpreserve(fdt_out, LUO_FDT_SIZE);
	pr_err("failed to prepare LUO FDT: %d\n", ret);

	return ret;
}

static void luo_fdt_destroy(void)
{
	kho_remove_subtree(luo_fdt_out);
	luo_contig_free_unpreserve(luo_fdt_out, LUO_FDT_SIZE);
	luo_fdt_out = NULL;
}

static int luo_do_prepare_calls(void)
{
	int ret;

	ret = luo_do_subsystems_prepare_calls();

	return ret;
}

static int luo_do_freeze_calls(void)
{
	int ret;

	ret = luo_do_subsystems_freeze_calls();

	return ret;
}

static void luo_do_finish_calls(void)
{
	luo_do_subsystems_finish_calls();
}

static void luo_do_cancel_calls(void)
{
	luo_do_subsystems_cancel_calls();
}

static int __luo_prepare(void)
{
	int ret;

	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn("[prepare] event canceled by user\n");
		return -EAGAIN;
	}

	if (!is_current_luo_state(LIVEUPDATE_STATE_NORMAL)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_PREPARED],
			luo_current_state_str());
		ret = -EINVAL;
		goto exit_unlock;
	}

	ret = luo_fdt_setup();
	if (ret)
		goto exit_unlock;

	ret = luo_do_prepare_calls();
	if (ret) {
		luo_fdt_destroy();
		goto exit_unlock;
	}

	luo_set_state(LIVEUPDATE_STATE_PREPARED);

exit_unlock:
	up_write(&luo_state_rwsem);

	return ret;
}

static int __luo_cancel(void)
{
	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn("[cancel] event canceled by user\n");
		return -EAGAIN;
	}

	if (!is_current_luo_state(LIVEUPDATE_STATE_PREPARED) &&
	    !is_current_luo_state(LIVEUPDATE_STATE_FROZEN)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_NORMAL],
			luo_current_state_str());
		up_write(&luo_state_rwsem);

		return -EINVAL;
	}

	luo_do_cancel_calls();
	luo_fdt_destroy();
	luo_set_state(LIVEUPDATE_STATE_NORMAL);

	up_write(&luo_state_rwsem);

	return 0;
}

/* Get the current state as a string */
const char *luo_current_state_str(void)
{
	return luo_state_str[liveupdate_get_state()];
}

enum liveupdate_state liveupdate_get_state(void)
{
	return READ_ONCE(luo_state);
}

/**
 * luo_prepare - Initiate the live update preparation phase.
 *
 * This function is called to begin the live update process. It attempts to
 * transition the luo to the ``LIVEUPDATE_STATE_PREPARED`` state.
 *
 * If the calls complete successfully, the orchestrator state is set
 * to ``LIVEUPDATE_STATE_PREPARED``. If any  call fails a
 * ``LIVEUPDATE_CANCEL`` is sent to roll back any actions.
 *
 * @return 0 on success, ``-EAGAIN`` if the state change was cancelled by the
 * user while waiting for the lock, ``-EINVAL`` if the orchestrator is not in
 * the normal state, or a negative error code returned by the calls.
 */
int luo_prepare(void)
{
	int err = __luo_prepare();

	if (err)
		return err;

	return kho_finalize();
}

/**
 * luo_freeze() - Initiate the final freeze notification phase for live update.
 *
 * Attempts to transition the live update orchestrator state from
 * %LIVEUPDATE_STATE_PREPARED to %LIVEUPDATE_STATE_FROZEN. This function is
 * typically called just before the actual reboot system call (e.g., kexec)
 * is invoked, either directly by the orchestration tool or potentially from
 * within the reboot syscall path itself.
 *
 * @return  0: Success. Negative error otherwise. State is reverted to
 * %LIVEUPDATE_STATE_NORMAL in case of an error during callbacks, and everything
 * is canceled via cancel notifcation.
 */
int luo_freeze(void)
{
	int ret;

	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn("[freeze] event canceled by user\n");
		return -EAGAIN;
	}

	if (!is_current_luo_state(LIVEUPDATE_STATE_PREPARED)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_FROZEN],
			luo_current_state_str());
		up_write(&luo_state_rwsem);

		return -EINVAL;
	}

	ret = luo_do_freeze_calls();
	if (!ret)
		luo_set_state(LIVEUPDATE_STATE_FROZEN);
	else
		luo_set_state(LIVEUPDATE_STATE_NORMAL);

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
 * @return 0 on success, ``-EAGAIN`` if the state change was cancelled by the
 * user while waiting for the lock, or ``-EINVAL`` if the orchestrator is not in
 * the updated state.
 */
int luo_finish(void)
{
	if (down_write_killable(&luo_state_rwsem)) {
		pr_warn("[finish] event canceled by user\n");
		return -EAGAIN;
	}

	if (!is_current_luo_state(LIVEUPDATE_STATE_UPDATED)) {
		pr_warn("Can't switch to [%s] from [%s] state\n",
			luo_state_str[LIVEUPDATE_STATE_NORMAL],
			luo_current_state_str());
		up_write(&luo_state_rwsem);

		return -EINVAL;
	}

	luo_do_finish_calls();
	luo_set_state(LIVEUPDATE_STATE_NORMAL);

	up_write(&luo_state_rwsem);

	return 0;
}

/**
 * luo_cancel - Cancel the ongoing live update from prepared or frozen states.
 *
 * This function is called to abort a live update that is currently in the
 * ``LIVEUPDATE_STATE_PREPARED`` state.
 *
 * If the state is correct, it triggers the ``LIVEUPDATE_CANCEL`` notifier chain
 * to allow subsystems to undo any actions performed during the prepare or
 * freeze events. Finally, the orchestrator state is transitioned back to
 * ``LIVEUPDATE_STATE_NORMAL``.
 *
 * @return 0 on success, or ``-EAGAIN`` if the state change was cancelled by the
 * user while waiting for the lock.
 */
int luo_cancel(void)
{
	int err =  kho_abort();

	if (err)
		return err;

	return __luo_cancel();
}

void luo_state_read_enter(void)
{
	down_read(&luo_state_rwsem);
}

void luo_state_read_exit(void)
{
	up_read(&luo_state_rwsem);
}

static int __init luo_startup(void)
{
	phys_addr_t fdt_phys;
	int ret;

	if (!kho_is_enabled()) {
		if (luo_enabled)
			pr_warn("Disabling liveupdate because KHO is disabled\n");
		luo_enabled = false;
		return 0;
	}

	/* Retrieve LUO subtree, and verify its format. */
	ret = kho_retrieve_subtree(LUO_KHO_ENTRY_NAME, &fdt_phys);
	if (ret) {
		if (ret != -ENOENT) {
			luo_restore_fail("failed to retrieve FDT '%s' from KHO: %d\n",
					 LUO_KHO_ENTRY_NAME, ret);
		}
		__luo_set_state(LIVEUPDATE_STATE_NORMAL);

		return 0;
	}

	luo_fdt_in = __va(fdt_phys);
	ret = fdt_node_check_compatible(luo_fdt_in, 0, LUO_COMPATIBLE);
	if (ret) {
		luo_restore_fail("FDT '%s' is incompatible with '%s' [%d]\n",
				 LUO_KHO_ENTRY_NAME, LUO_COMPATIBLE, ret);
	}

	__luo_set_state(LIVEUPDATE_STATE_UPDATED);
	luo_subsystems_startup(luo_fdt_in);

	return 0;
}
early_initcall(luo_startup);

/* Public Functions */

/**
 * liveupdate_reboot() - Kernel reboot notifier for live update final
 * serialization.
 *
 * This function is invoked directly from the reboot() syscall pathway if a
 * reboot is initiated while the live update state is %LIVEUPDATE_STATE_PREPARED
 * (i.e., if the user did not explicitly trigger the frozen state). It handles
 * the implicit transition into the final frozen state.
 *
 * It triggers the %LIVEUPDATE_REBOOT event callbacks for participating
 * subsystems. These callbacks must perform final state saving very quickly as
 * they execute during the blackout period just before kexec.
 *
 * If any %LIVEUPDATE_FREEZE callback fails, this function triggers the
 * %LIVEUPDATE_CANCEL event for all participants to revert their state, aborts
 * the live update, and returns an error.
 */
int liveupdate_reboot(void)
{
	if (!is_current_luo_state(LIVEUPDATE_STATE_PREPARED))
		return 0;

	return luo_freeze();
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
	return is_current_luo_state(LIVEUPDATE_STATE_UPDATED);
}

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
	return is_current_luo_state(LIVEUPDATE_STATE_NORMAL);
}

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

/**
 * luo_contig_alloc_preserve - Allocate, zero, and preserve contiguous memory.
 * @size: The number of bytes to allocate.
 *
 * Allocates a physically contiguous block of zeroed pages that is large
 * enough to hold @size bytes. The allocated memory is then registered with
 * KHO for preservation across a kexec.
 *
 * Note: The actual allocated size will be rounded up to the nearest
 * power-of-two page boundary.
 *
 * @return A virtual pointer to the allocated and preserved memory on success,
 * or an ERR_PTR() encoded error on failure.
 */
void *luo_contig_alloc_preserve(size_t size)
{
	int order, ret;
	void *mem;

	if (!size)
		return ERR_PTR(-EINVAL);

	order = get_order(size);
	if (order > MAX_PAGE_ORDER)
		return ERR_PTR(-E2BIG);

	mem = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!mem)
		return ERR_PTR(-ENOMEM);

	ret = kho_preserve_pages(virt_to_page(mem), 1 << order);
	if (ret) {
		free_pages((unsigned long)mem, order);
		return ERR_PTR(ret);
	}

	return mem;
}

/**
 * luo_contig_free_unpreserve - Unpreserve and free contiguous memory.
 * @mem:  Pointer to the memory allocated by luo_contig_alloc_preserve().
 * @size: The original size requested during allocation. This is used to
 *        recalculate the correct order for freeing the pages.
 *
 * Unregisters the memory from KHO preservation and frees the underlying
 * pages back to the system. This function should be called to clean up
 * memory allocated with luo_contig_alloc_preserve().
 */
void luo_contig_free_unpreserve(void *mem, size_t size)
{
	unsigned int order;

	if (!mem || !size)
		return;

	order = get_order(size);
	if (WARN_ON_ONCE(order > MAX_PAGE_ORDER))
		return;

	WARN_ON_ONCE(kho_unpreserve_pages(virt_to_page(mem), 1 << order));
	free_pages((unsigned long)mem, order);
}

void luo_contig_free_restore(void *mem, size_t size)
{
	unsigned int order;

	if (!mem || !size)
		return;

	order = get_order(size);
	if (WARN_ON_ONCE(order > MAX_PAGE_ORDER))
		return;

	WARN_ON_ONCE(!kho_restore_pages(__pa(mem), 1 << order));
	free_pages((unsigned long)mem, order);
}
