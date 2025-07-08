// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/liveupdate.h>

#include "iommufd_private.h"

static int iommufd_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}

static int iommufd_liveupdate_freeze(struct liveupdate_file_op_args *args)
{
	/* No-Op; everything should be made read-only */
	return 0;
}

static void iommufd_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
}

static int iommufd_liveupdate_retrieve(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}

static bool iommufd_liveupdate_can_finish(struct liveupdate_file_op_args *args)
{
	return false;
}

static void iommufd_liveupdate_finish(struct liveupdate_file_op_args *args)
{
}

static bool iommufd_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					    struct file *file)
{
	return false;
}

static struct liveupdate_file_ops iommufd_lu_file_ops = {
	.can_preserve = iommufd_liveupdate_can_preserve,
	.preserve = iommufd_liveupdate_preserve,
	.unpreserve = iommufd_liveupdate_unpreserve,
	.freeze = iommufd_liveupdate_freeze,
	.retrieve = iommufd_liveupdate_retrieve,
	.can_finish = iommufd_liveupdate_can_finish,
	.finish = iommufd_liveupdate_finish,
};

static struct liveupdate_file_handler iommufd_lu_handler = {
	.compatible = "iommufd-v1",
	.ops = &iommufd_lu_file_ops,
};

int iommufd_liveupdate_register_lufs(void)
{
	return liveupdate_register_file_handler(&iommufd_lu_handler);
}

int iommufd_liveupdate_unregister_lufs(void)
{
	return liveupdate_unregister_file_handler(&iommufd_lu_handler);
}
