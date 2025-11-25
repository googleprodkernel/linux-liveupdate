// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Vipin Sharma <vipinsh@google.com>
 * David Matlack <dmatlack@google.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kho/abi/vfio_pci.h>
#include <linux/liveupdate.h>
#include <linux/errno.h>

#include "vfio_pci_priv.h"

static bool vfio_pci_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					     struct file *file)
{
	return false;
}

static int vfio_pci_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}

static void vfio_pci_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
}

static int vfio_pci_liveupdate_retrieve(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}

static void vfio_pci_liveupdate_finish(struct liveupdate_file_op_args *args)
{
}

static const struct liveupdate_file_ops vfio_pci_liveupdate_file_ops = {
	.can_preserve = vfio_pci_liveupdate_can_preserve,
	.preserve = vfio_pci_liveupdate_preserve,
	.unpreserve = vfio_pci_liveupdate_unpreserve,
	.retrieve = vfio_pci_liveupdate_retrieve,
	.finish = vfio_pci_liveupdate_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler vfio_pci_liveupdate_fh = {
	.ops = &vfio_pci_liveupdate_file_ops,
	.compatible = VFIO_PCI_LUO_FH_COMPATIBLE,
};

int __init vfio_pci_liveupdate_init(void)
{
	if (!liveupdate_enabled())
		return 0;

	return liveupdate_register_file_handler(&vfio_pci_liveupdate_fh);
}

void vfio_pci_liveupdate_cleanup(void)
{
	if (!liveupdate_enabled())
		return;

	liveupdate_unregister_file_handler(&vfio_pci_liveupdate_fh);
}
