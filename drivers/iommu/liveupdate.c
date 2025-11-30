// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2025, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#define pr_fmt(fmt)    "iommu: liveupdate: " fmt

#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/iommu-lu.h>
#include <linux/errno.h>

static void iommu_liveupdate_flb_free(struct iommu_lu_flb_obj *obj)
{
}

static int iommu_liveupdate_flb_preserve(struct liveupdate_flb_op_args *argp)
{
	return -EOPNOTSUPP;
}

static void iommu_liveupdate_flb_unpreserve(struct liveupdate_flb_op_args *argp)
{
	iommu_liveupdate_flb_free(argp->obj);
}

static void iommu_liveupdate_flb_finish(struct liveupdate_flb_op_args *argp)
{
}

static int iommu_liveupdate_flb_retrieve(struct liveupdate_flb_op_args *argp)
{
	return -EOPNOTSUPP;
}

static struct liveupdate_flb_ops iommu_flb_ops = {
	.preserve = iommu_liveupdate_flb_preserve,
	.unpreserve = iommu_liveupdate_flb_unpreserve,
	.finish = iommu_liveupdate_flb_finish,
	.retrieve = iommu_liveupdate_flb_retrieve,
};

static struct liveupdate_flb iommu_flb = {
	.compatible = IOMMU_LUO_FLB_COMPATIBLE,
	.ops = &iommu_flb_ops,
};

int iommu_liveupdate_register_flb(struct liveupdate_file_handler *handler)
{
	return liveupdate_register_flb(handler, &iommu_flb);
}
EXPORT_SYMBOL(iommu_liveupdate_register_flb);

int iommu_liveupdate_unregister_flb(struct liveupdate_file_handler *handler)
{
	return liveupdate_unregister_flb(handler, &iommu_flb);
}
EXPORT_SYMBOL(iommu_liveupdate_unregister_flb);
