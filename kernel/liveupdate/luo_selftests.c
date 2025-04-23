// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: LUO Selftests
 *
 * We provide ioctl-based selftest interface for the LUO. It provides a
 * mechanism to test core LUO functionality, particularly the registration,
 * unregistration, and data handling aspects of LUO subsystems, without
 * requiring a full live update event sequence.
 *
 * The tests are intended primarily for developers working on the LUO framework
 * or for validation purposes during system integration. This functionality is
 * conditionally compiled based on the `CONFIG_LIVEUPDATE_SELFTESTS` Kconfig
 * option and should typically be disabled in production kernels.
 *
 * Interface:
 * The selftests are accessed via the `/dev/liveupdate` character device using
 * the `LIVEUPDATE_IOCTL_SELFTESTS` ioctl command. The argument to the ioctl
 * is a pointer to a `struct liveupdate_selftest` structure (defined in
 * `uapi/linux/liveupdate.h`), which contains:
 * - `cmd`: The specific selftest command to execute (e.g.,
 * `LUO_CMD_SUBSYSTEM_REGISTER`).
 * - `arg`: A pointer to a command-specific argument structure. For subsystem
 * tests, this points to a `struct luo_arg_subsystem` (defined in
 * `luo_selftests.h`).
 *
 * Commands:
 * - `LUO_CMD_SUBSYSTEM_REGISTER`:
 * Registers a new dummy LUO subsystem. It allocates kernel memory for test
 * data, copies initial data from the user-provided `data_page`, sets up
 * simple logging callbacks, and calls the core
 * `liveupdate_register_subsystem()`
 * function. Requires `arg` pointing to `struct luo_arg_subsystem`.
 * - `LUO_CMD_SUBSYSTEM_UNREGISTER`:
 * Unregisters a previously registered dummy subsystem identified by `name`.
 * It calls the core `liveupdate_unregister_subsystem()` function and then
 * frees the associated kernel memory and internal tracking structures.
 * Requires `arg` pointing to `struct luo_arg_subsystem` (only `name` used).
 * - `LUO_CMD_SUBSYSTEM_GETDATA`:
 * Copies the content of the kernel data page associated with the specified
 * dummy subsystem (`name`) back to the user-provided `data_page`. This allows
 * userspace to verify the state of the data after potential test operations.
 * Requires `arg` pointing to `struct luo_arg_subsystem`.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <uapi/linux/liveupdate.h>
#include "luo_internal.h"
#include "luo_selftests.h"

static struct luo_subsystems {
	struct liveupdate_subsystem handle;
	char name[LUO_NAME_LENGTH];
	void *data;
	bool in_use;
	bool preserved;
} luo_subsystems[LUO_MAX_SUBSYSTEMS];

/* Only allow one selftest ioctl operation at a time */
static DEFINE_MUTEX(luo_ioctl_mutex);

static int luo_subsystem_prepare(struct liveupdate_subsystem *h, u64 *data)
{
	struct luo_subsystems *s = container_of(h, struct luo_subsystems,
						handle);
	unsigned long phys_addr = __pa(s->data);
	int ret;

	ret = kho_preserve_pages(phys_to_page(phys_addr), 1);
	if (ret)
		return ret;

	s->preserved = true;
	*data = phys_addr;
	pr_info("Subsystem '%s' prepare data[%lx]\n",
		s->name, phys_addr);

	if (strstr(s->name, NAME_PREPARE_FAIL))
		return -EAGAIN;

	return 0;
}

static int luo_subsystem_freeze(struct liveupdate_subsystem *h, u64 *data)
{
	struct luo_subsystems *s = container_of(h, struct luo_subsystems,
						handle);

	pr_info("Subsystem '%s' freeze data[%llx]\n", s->name, *data);

	return 0;
}

static void luo_subsystem_cancel(struct liveupdate_subsystem *h, u64 data)
{
	struct luo_subsystems *s = container_of(h, struct luo_subsystems,
						handle);

	pr_info("Subsystem '%s' canel data[%llx]\n", s->name, data);
	s->preserved = false;
	WARN_ON(kho_unpreserve_pages(phys_to_page(data), 1));
}

static void luo_subsystem_finish(struct liveupdate_subsystem *h, u64 data)
{
	struct luo_subsystems *s = container_of(h, struct luo_subsystems,
						handle);

	pr_info("Subsystem '%s' finish data[%llx]\n", s->name, data);
}

static const struct liveupdate_subsystem_ops luo_selftest_subsys_ops = {
	.prepare = luo_subsystem_prepare,
	.freeze = luo_subsystem_freeze,
	.cancel = luo_subsystem_cancel,
	.finish = luo_subsystem_finish,
	.owner = THIS_MODULE,
};

static int luo_subsystem_idx(char *name)
{
	int i;

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		if (luo_subsystems[i].in_use &&
		    !strcmp(luo_subsystems[i].name, name))
			break;
	}

	if (i == LUO_MAX_SUBSYSTEMS) {
		pr_warn("Subsystem with name '%s' is not registred\n", name);

		return -EINVAL;
	}

	return i;
}

static void luo_put_and_free_subsystem(char *name)
{
	int i = luo_subsystem_idx(name);

	if (i < 0)
		return;

	if (luo_subsystems[i].preserved)
		kho_unpreserve_pages(virt_to_page(luo_subsystems[i].data), 1);
	free_page((unsigned long)luo_subsystems[i].data);
	luo_subsystems[i].in_use = false;
	luo_subsystems[i].preserved = false;
}

static int luo_get_and_alloc_subsystem(char *name, void __user *data,
				       struct liveupdate_subsystem **hp)
{
	unsigned long page_addr, i;

	page_addr = get_zeroed_page(GFP_KERNEL);
	if (!page_addr) {
		pr_warn("Failed to allocate memory for subsystem data\n");
		return -ENOMEM;
	}

	if (copy_from_user((void *)page_addr, data, PAGE_SIZE)) {
		free_page(page_addr);
		return -EFAULT;
	}

	for (i = 0; i < LUO_MAX_SUBSYSTEMS; i++) {
		if (!luo_subsystems[i].in_use)
			break;
	}

	if (i == LUO_MAX_SUBSYSTEMS) {
		pr_warn("Maximum number of subsystems registered\n");
		free_page(page_addr);
		return -ENOMEM;
	}

	luo_subsystems[i].in_use = true;
	luo_subsystems[i].handle.ops = &luo_selftest_subsys_ops;
	luo_subsystems[i].handle.name = luo_subsystems[i].name;
	strscpy(luo_subsystems[i].name, name, LUO_NAME_LENGTH);
	luo_subsystems[i].data = (void *)page_addr;

	*hp = &luo_subsystems[i].handle;

	return 0;
}

static int luo_cmd_subsystem_unregister(void __user *argp)
{
	struct luo_arg_subsystem arg;
	int ret, i;

	if (copy_from_user(&arg, argp, sizeof(arg)))
		return -EFAULT;

	i = luo_subsystem_idx(arg.name);
	if (i < 0)
		return i;

	ret = liveupdate_unregister_subsystem(&luo_subsystems[i].handle);
	if (ret)
		return ret;

	luo_put_and_free_subsystem(arg.name);

	return 0;
}

static int luo_cmd_subsystem_register(void __user *argp)
{
	struct liveupdate_subsystem *h;
	struct luo_arg_subsystem arg;
	int ret;

	if (copy_from_user(&arg, argp, sizeof(arg)))
		return -EFAULT;

	ret = luo_get_and_alloc_subsystem(arg.name,
					  (void __user *)arg.data_page, &h);
	if (ret)
		return ret;

	ret = liveupdate_register_subsystem(h);
	if (ret)
		luo_put_and_free_subsystem(arg.name);

	return ret;
}

static int luo_cmd_subsystem_getdata(void __user *argp)
{
	struct luo_arg_subsystem arg;
	int i;

	if (copy_from_user(&arg, argp, sizeof(arg)))
		return -EFAULT;

	i = luo_subsystem_idx(arg.name);
	if (i < 0)
		return i;

	if (copy_to_user(arg.data_page, luo_subsystems[i].data,
			 PAGE_SIZE)) {
		return -EFAULT;
	}

	return 0;
}

static int luo_ioctl_selftests(void __user *argp)
{
	struct liveupdate_selftest luo_st;
	void __user *cmd_argp;
	int ret = 0;

	if (copy_from_user(&luo_st, argp, sizeof(luo_st)))
		return -EFAULT;

	cmd_argp = (void __user *)luo_st.arg;

	mutex_lock(&luo_ioctl_mutex);
	switch (luo_st.cmd) {
	case LUO_CMD_SUBSYSTEM_REGISTER:
		ret =  luo_cmd_subsystem_register(cmd_argp);
		break;

	case LUO_CMD_SUBSYSTEM_UNREGISTER:
		ret =  luo_cmd_subsystem_unregister(cmd_argp);
		break;

	case LUO_CMD_SUBSYSTEM_GETDATA:
		ret = luo_cmd_subsystem_getdata(cmd_argp);
		break;

	default:
		pr_warn("ioctl: unknown self-test command nr: 0x%llx\n",
			luo_st.cmd);
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&luo_ioctl_mutex);

	return ret;
}

static long luo_selftest_ioctl(struct file *filep, unsigned int cmd,
			       unsigned long arg)
{
	int ret = 0;

	if (_IOC_TYPE(cmd) != LIVEUPDATE_IOCTL_TYPE)
		return -ENOTTY;

	switch (cmd) {
	case LIVEUPDATE_IOCTL_FREEZE:
		ret = luo_freeze();
		break;

	case LIVEUPDATE_IOCTL_SELFTESTS:
		ret = luo_ioctl_selftests((void __user *)arg);
		break;

	default:
		pr_warn("ioctl: unknown command nr: 0x%x\n", _IOC_NR(cmd));
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static const struct file_operations luo_selftest_fops = {
	.open = nonseekable_open,
	.unlocked_ioctl = luo_selftest_ioctl,
};

static int __init luo_seltesttest_init(void)
{
	if (!liveupdate_debugfs_root) {
		pr_err("liveupdate root is not set\n");
		return 0;
	}
	debugfs_create_file_unsafe("luo_selftest", 0600,
				   liveupdate_debugfs_root, NULL,
				   &luo_selftest_fops);
	return 0;
}

late_initcall(luo_seltesttest_init);
