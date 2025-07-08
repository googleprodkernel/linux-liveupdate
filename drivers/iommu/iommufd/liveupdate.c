// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/iommufd.h>
#include <linux/liveupdate.h>
#include <linux/mm.h>

#include "iommufd_private.h"

static int iommufd_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(args->file);
	struct iommufd_lu *iommufd_lu;
	size_t serial_size;
	void *mem;
	int rc;

	if (IS_ERR(ictx))
		return PTR_ERR(ictx);

	serial_size = sizeof(*iommufd_lu);

	mem = kho_alloc_preserve(serial_size);
	if (!mem) {
		rc = -ENOMEM;
		goto err_ctx_put;
	}

	iommufd_lu = mem;

	args->serialized_data = virt_to_phys(iommufd_lu);
	iommufd_ctx_put(ictx);
	return 0;

err_ctx_put:
	iommufd_ctx_put(ictx);
	return rc;
}

static int iommufd_liveupdate_freeze(struct liveupdate_file_op_args *args)
{
	/* No-Op; everything should be made read-only */
	return 0;
}

static void iommufd_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(args->file);

	if (WARN_ON(IS_ERR(ictx)))
		return;

	kho_unpreserve_free(phys_to_virt(args->serialized_data));
	iommufd_ctx_put(ictx);
}

static int iommufd_liveupdate_retrieve(struct liveupdate_file_op_args *args)
{
	struct iommufd_lu *iommufd_lu;
	struct iommufd_ctx *ictx;
	struct folio *folio_lu;
	struct file *file;
	int rc;

	folio_lu = kho_restore_folio(args->serialized_data);
	if (IS_ERR_OR_NULL(folio_lu))
		return -EFAULT;

	iommufd_lu = folio_address(folio_lu);

	file = anon_inode_create_getfile("iommufd", &iommufd_fops,
					 NULL, O_RDWR, NULL);
	if (IS_ERR(file)) {
		rc = PTR_ERR(file);
		goto err_folio_put;
	}

	rc = iommufd_fops.open(file->f_inode, file);
	if (rc)
		goto err_fput;

	ictx = iommufd_ctx_from_file(file);
	if (WARN_ON(IS_ERR(ictx))) {
		rc = PTR_ERR(ictx);
		goto err_fput;
	}

	if (WARN_ON(ictx->lu)) {
		rc = -EEXIST;
		goto err_ctx_put;
	}
	ictx->lu = iommufd_lu;

	iommufd_ctx_put(ictx);

	args->file = file;

	return 0;

err_ctx_put:
	iommufd_ctx_put(ictx);
err_fput:
	fput(file);
err_folio_put:
	folio_put(folio_lu);
	return rc;
}

static bool iommufd_liveupdate_can_finish(struct liveupdate_file_op_args *args)
{
	if (!args->retrieved || !args->file) {
		pr_warn("%s: fd not reclaimed\n", __func__);
		return false;
	}

	return true;
}

static void iommufd_liveupdate_finish(struct liveupdate_file_op_args *args)
{
	struct iommufd_lu *iommufd_lu;
	struct iommufd_ctx *ictx;

	ictx = iommufd_ctx_from_file(args->file);
	iommufd_lu = ictx->lu;
	ictx->lu = NULL;
	iommufd_ctx_put(ictx);

	folio_put(virt_to_folio(iommufd_lu));
}

static bool iommufd_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					    struct file *file)
{
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(file);

	if (IS_ERR(ictx))
		return false;

	iommufd_ctx_put(ictx);
	return true;
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
	.compatible = IOMMUFD_LUO_COMPATIBLE,
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
