// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

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

static const struct file_operations fops = {
	.owner		= THIS_MODULE,
};

static struct miscdevice liveupdate_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "liveupdate",
	.fops  = &fops,
};

static int __init liveupdate_init(void)
{
	if (!liveupdate_enabled())
		return 0;

	return misc_register(&liveupdate_miscdev);
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
