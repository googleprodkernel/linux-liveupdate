// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/liveupdate.h>

#include "iommufd_private.h"

static int iommufd_liveupdate_prepare(struct liveupdate_file_handler *handler,
				      struct file *file, u64 *data)
{
	return -EOPNOTSUPP;
}

static int iommufd_liveupdate_freeze(struct liveupdate_file_handler *handler,
				     struct file *file, u64 *data)
{
	/* No-Op; everything should be made read-only */
	return 0;
}

static void iommufd_liveupdate_cancel(struct liveupdate_file_handler *handler,
				      struct file *file, u64 data)
{
}

static int iommufd_liveupdate_retrieve(struct liveupdate_file_handler *handler,
				       u64 data, struct file **file_p)
{
	return -EOPNOTSUPP;
}

static void iommufd_liveupdate_finish(struct liveupdate_file_handler *handler,
				      struct file *file, u64 data, bool reclaimed)
{
}

static bool iommufd_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					    struct file *file)
{
	return false;
}

static struct liveupdate_file_ops iommufd_lu_file_ops = {
	.prepare = iommufd_liveupdate_prepare,
	.freeze = iommufd_liveupdate_freeze,
	.cancel = iommufd_liveupdate_cancel,
	.finish = iommufd_liveupdate_finish,
	.retrieve = iommufd_liveupdate_retrieve,
	.can_preserve = iommufd_liveupdate_can_preserve,
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
