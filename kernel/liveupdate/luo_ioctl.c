// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: LUO ioctl Interface
 *
 * The IOCTL user-space control interface for the LUO subsystem.
 * It registers a character device, typically found at ``/dev/liveupdate``,
 * which allows a userspace agent to manage the LUO state machine and its
 * associated resources, such as preservable file descriptors.
 *
 * To ensure that the state machine is controlled by a single entity, access
 * to this device is exclusive: only one process is permitted to have
 * ``/dev/liveupdate`` open at any given time. Subsequent open attempts will
 * fail with -EBUSY until the first process closes its file descriptor.
 * This singleton model simplifies state management by preventing conflicting
 * commands from multiple userspace agents.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
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

struct luo_device_state {
	struct miscdevice miscdev;
	atomic_t in_use;
};

static int luo_ioctl_get_state(struct luo_ucmd *ucmd)
{
	struct liveupdate_ioctl_get_state *argp = ucmd->cmd;

	argp->state = liveupdate_get_state();

	return luo_ucmd_respond(ucmd, sizeof(*argp));
}

static int luo_ioctl_set_event(struct luo_ucmd *ucmd)
{
	struct liveupdate_ioctl_set_event *argp = ucmd->cmd;
	int ret;

	switch (argp->event) {
	case LIVEUPDATE_PREPARE:
		ret = luo_prepare();
		break;
	case LIVEUPDATE_FINISH:
		ret = luo_finish();
		break;
	case LIVEUPDATE_CANCEL:
		ret = luo_cancel();
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static int luo_ioctl_create_session(struct luo_ucmd *ucmd)
{
	struct liveupdate_ioctl_create_session *argp = ucmd->cmd;
	struct file *file;
	int ret;

	argp->fd = get_unused_fd_flags(O_CLOEXEC);
	if (argp->fd < 0)
		return argp->fd;

	ret = luo_session_create(argp->name, &file);
	if (ret)
		return ret;

	ret = luo_ucmd_respond(ucmd, sizeof(*argp));
	if (ret) {
		fput(file);
		put_unused_fd(argp->fd);
		return ret;
	}

	fd_install(argp->fd, file);

	return 0;
}

static int luo_ioctl_retrieve_session(struct luo_ucmd *ucmd)
{
	struct liveupdate_ioctl_retrieve_session *argp = ucmd->cmd;
	struct file *file;
	int ret;

	argp->fd = get_unused_fd_flags(O_CLOEXEC);
	if (argp->fd < 0)
		return argp->fd;

	ret = luo_session_retrieve(argp->name, &file);
	if (ret < 0) {
		put_unused_fd(argp->fd);

		return ret;
	}

	ret = luo_ucmd_respond(ucmd, sizeof(*argp));
	if (ret) {
		fput(file);
		put_unused_fd(argp->fd);
		return ret;
	}

	fd_install(argp->fd, file);

	return 0;
}

static int luo_open(struct inode *inodep, struct file *filep)
{
	struct luo_device_state *ldev = container_of(filep->private_data,
						     struct luo_device_state,
						     miscdev);

	if (atomic_cmpxchg(&ldev->in_use, 0, 1))
		return -EBUSY;

	return 0;
}

static int luo_release(struct inode *inodep, struct file *filep)
{
	struct luo_device_state *ldev = container_of(filep->private_data,
						     struct luo_device_state,
						     miscdev);
	atomic_set(&ldev->in_use, 0);

	return 0;
}

union ucmd_buffer {
	struct liveupdate_ioctl_create_session create;
	struct liveupdate_ioctl_get_state state;
	struct liveupdate_ioctl_retrieve_session retrieve;
	struct liveupdate_ioctl_set_event event;
};

struct luo_ioctl_op {
	unsigned int size;
	unsigned int min_size;
	unsigned int ioctl_num;
	int (*execute)(struct luo_ucmd *ucmd);
};

#define IOCTL_OP(_ioctl, _fn, _struct, _last)                                  \
	[_IOC_NR(_ioctl) - LIVEUPDATE_CMD_BASE] = {                            \
		.size = sizeof(_struct) +                                      \
			BUILD_BUG_ON_ZERO(sizeof(union ucmd_buffer) <          \
					  sizeof(_struct)),                    \
		.min_size = offsetofend(_struct, _last),                       \
		.ioctl_num = _ioctl,                                           \
		.execute = _fn,                                                \
	}

static const struct luo_ioctl_op luo_ioctl_ops[] = {
	IOCTL_OP(LIVEUPDATE_IOCTL_CREATE_SESSION, luo_ioctl_create_session,
		 struct liveupdate_ioctl_create_session, name),
	IOCTL_OP(LIVEUPDATE_IOCTL_GET_STATE, luo_ioctl_get_state,
		 struct liveupdate_ioctl_get_state, state),
	IOCTL_OP(LIVEUPDATE_IOCTL_RETRIEVE_SESSION, luo_ioctl_retrieve_session,
		 struct liveupdate_ioctl_retrieve_session, name),
	IOCTL_OP(LIVEUPDATE_IOCTL_SET_EVENT, luo_ioctl_set_event,
		 struct liveupdate_ioctl_set_event, event),
};

static long luo_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	const struct luo_ioctl_op *op;
	struct luo_ucmd ucmd = {};
	union ucmd_buffer buf;
	unsigned int nr;
	int ret;

	nr = _IOC_NR(cmd);
	if (nr < LIVEUPDATE_CMD_BASE ||
	    (nr - LIVEUPDATE_CMD_BASE) >= ARRAY_SIZE(luo_ioctl_ops)) {
		return -EINVAL;
	}

	ucmd.ubuffer = (void __user *)arg;
	ret = get_user(ucmd.user_size, (u32 __user *)ucmd.ubuffer);
	if (ret)
		return ret;

	op = &luo_ioctl_ops[nr - LIVEUPDATE_CMD_BASE];
	if (op->ioctl_num != cmd)
		return -ENOIOCTLCMD;
	if (ucmd.user_size < op->min_size)
		return -EINVAL;

	ucmd.cmd = &buf;
	ret = copy_struct_from_user(ucmd.cmd, op->size, ucmd.ubuffer,
				    ucmd.user_size);
	if (ret)
		return ret;

	return op->execute(&ucmd);
}

static const struct file_operations luo_fops = {
	.owner		= THIS_MODULE,
	.open		= luo_open,
	.release	= luo_release,
	.unlocked_ioctl	= luo_ioctl,
};

static struct luo_device_state luo_dev = {
	.miscdev = {
		.minor = MISC_DYNAMIC_MINOR,
		.name  = "liveupdate",
		.fops  = &luo_fops,
	},
	.in_use = ATOMIC_INIT(0),
};

static int __init liveupdate_init(void)
{
	if (!liveupdate_enabled())
		return 0;

	return misc_register(&luo_dev.miscdev);
}
module_init(liveupdate_init);

static void __exit liveupdate_exit(void)
{
	misc_deregister(&luo_dev.miscdev);
}
module_exit(liveupdate_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pasha Tatashin");
MODULE_DESCRIPTION("Live Update Orchestrator");
MODULE_VERSION("0.1");
