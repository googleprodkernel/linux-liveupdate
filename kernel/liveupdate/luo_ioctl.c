// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: LUO ioctl Interface
 *
 * The IOCTL user-space control interface for the LUO subsystem.
 * It registers a misc character device, typically found at ``/dev/liveupdate``,
 * which allows privileged userspace applications (requiring %CAP_SYS_ADMIN) to
 * manage and monitor the LUO state machine and associated resources like
 * preservable file descriptors.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/liveupdate.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <uapi/linux/liveupdate.h>
#include "luo_internal.h"

static int luo_ioctl_fd_restore(struct liveupdate_fd *luo_fd)
{
	struct file *file;
	int ret;
	int fd;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		pr_err("Failed to allocate new fd: %d\n", fd);
		return fd;
	}

	ret = luo_retrieve_file(luo_fd->token, &file);
	if (ret < 0) {
		put_unused_fd(fd);

		return ret;
	}

	fd_install(fd, file);
	luo_fd->fd = fd;

	return 0;
}

static int luo_open(struct inode *inodep, struct file *filep)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	if (filep->f_flags & O_EXCL)
		return -EINVAL;

	filep->private_data = luo_create_session();
	if (!filep->private_data)
		return -ENOMEM;

	return 0;
}

static int luo_release(struct inode *inodep, struct file *filep)
{
	luo_destroy_session(filep->private_data);

	return 0;
}

static long luo_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct liveupdate_fd luo_fd;
	enum liveupdate_state state;
	int ret = 0;
	u64 token;

	if (_IOC_TYPE(cmd) != LIVEUPDATE_IOCTL_TYPE)
		return -ENOTTY;

	switch (cmd) {
	case LIVEUPDATE_IOCTL_GET_STATE:
		state = liveupdate_get_state();
		if (copy_to_user(argp, &state, sizeof(state)))
			ret = -EFAULT;
		break;

	case LIVEUPDATE_IOCTL_PREPARE:
		ret = luo_prepare();
		break;

	case LIVEUPDATE_IOCTL_FINISH:
		ret = luo_finish();
		break;

	case LIVEUPDATE_IOCTL_CANCEL:
		ret = luo_cancel();
		break;

	case LIVEUPDATE_IOCTL_FD_PRESERVE:
		if (copy_from_user(&luo_fd, argp, sizeof(luo_fd))) {
			ret = -EFAULT;
			break;
		}

		ret = luo_register_file(filep->private_data, luo_fd.token,
					luo_fd.fd);
		if (!ret && copy_to_user(argp, &luo_fd, sizeof(luo_fd))) {
			WARN_ON_ONCE(luo_unregister_file(filep->private_data,
							 luo_fd.token));
			ret = -EFAULT;
		}
		break;

	case LIVEUPDATE_IOCTL_FD_UNPRESERVE:
		if (copy_from_user(&token, argp, sizeof(u64))) {
			ret = -EFAULT;
			break;
		}

		ret = luo_unregister_file(filep->private_data, token);
		break;

	case LIVEUPDATE_IOCTL_FD_RESTORE:
		if (copy_from_user(&luo_fd, argp, sizeof(luo_fd))) {
			ret = -EFAULT;
			break;
		}

		ret = luo_ioctl_fd_restore(&luo_fd);
		if (!ret && copy_to_user(argp, &luo_fd, sizeof(luo_fd)))
			ret = -EFAULT;
		break;

	default:
		pr_warn("ioctl: unknown command nr: 0x%x\n", _IOC_NR(cmd));
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static const struct file_operations fops = {
	.owner          = THIS_MODULE,
	.open           = luo_open,
	.release	= luo_release,
	.unlocked_ioctl = luo_ioctl,
};

static struct miscdevice liveupdate_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "liveupdate",
	.fops  = &fops,
};

static int __init liveupdate_init(void)
{
	int err;

	if (!liveupdate_enabled())
		return 0;

	err = misc_register(&liveupdate_miscdev);
	if (err < 0) {
		pr_err("Failed to register misc device '%s': %d\n",
		       liveupdate_miscdev.name, err);
	}

	return err;
}
module_init(liveupdate_init);

static void __exit liveupdate_exit(void)
{
	misc_deregister(&liveupdate_miscdev);
}
module_exit(liveupdate_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pasha Tatashin");
MODULE_DESCRIPTION("Live Update Orchestrator");
MODULE_VERSION("0.1");
