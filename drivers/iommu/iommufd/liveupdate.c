// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/mm.h>

#include "iommufd_private.h"

int iommufd_hwpt_lu_set_preserved(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_lu_set_preserved *cmd = ucmd->cmd;
	struct iommufd_hwpt_paging *hwpt_target, *hwpt;
	struct iommufd_ctx *ictx = ucmd->ictx;
	struct iommufd_object *obj;
	unsigned long index;
	int rc = 0;

	/* TODO: return error if already prepared */

	hwpt_target = iommufd_get_hwpt_paging(ucmd, cmd->hwpt_id);
	if (IS_ERR(hwpt_target))
		return PTR_ERR(hwpt_target);

	xa_lock(&ictx->objects);
	xa_for_each(&ictx->objects, index, obj) {
		if (obj->type != IOMMUFD_OBJ_HWPT_PAGING)
			continue;

		hwpt = container_of(obj, struct iommufd_hwpt_paging, common.obj);

		if (hwpt == hwpt_target)
			continue;
		if (!hwpt->lu_preserved)
			continue;
		if (hwpt->lu_token == cmd->hwpt_token) {
			xa_unlock(&ictx->objects);
			rc = -EADDRINUSE;
			goto out;
		}
	}
	xa_unlock(&ictx->objects);

	hwpt_target->lu_preserved = cmd->preserved;
	hwpt_target->lu_token = cmd->hwpt_token;

out:
	iommufd_put_object(ictx, &hwpt_target->common.obj);
	return rc;
}

static int iommufd_liveupdate_prepare(struct liveupdate_file_handler *handler,
				      struct file *file, u64 *data)
{
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(file);
	struct iommufd_lu *iommufd_lu;
	struct folio *folio_lu;
	size_t serial_size;
	int rc;

	if (IS_ERR(ictx))
		return PTR_ERR(ictx);

	serial_size = sizeof(*iommufd_lu);

	folio_lu = folio_alloc(GFP_KERNEL, get_order(serial_size));
	if (!folio_lu) {
		rc = -ENOMEM;
		goto err_ctx_put;
	}

	iommufd_lu = folio_address(folio_lu);

	rc = kho_preserve_folio(folio_lu);
	if (rc)
		goto err_folio_put;

	*data = virt_to_phys(iommufd_lu);

	iommufd_ctx_put(ictx);
	return 0;

err_folio_put:
	folio_put(folio_lu);

err_ctx_put:
	iommufd_ctx_put(ictx);
	return rc;
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
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(file);
	struct folio *folio_lu;

	if (WARN_ON(IS_ERR(ictx)))
		return;

	folio_lu = pfn_folio(PHYS_PFN(data));
	WARN_ON(kho_unpreserve_folio(folio_lu));
	folio_put(folio_lu);

	iommufd_ctx_put(ictx);
}

static int iommufd_liveupdate_retrieve(struct liveupdate_file_handler *handler,
				       u64 data, struct file **file_p)
{
	struct iommufd_lu *iommufd_lu;
	struct iommufd_ctx *ictx;
	struct folio *folio_lu;
	struct file *file;
	int rc;

	folio_lu = kho_restore_folio(data);
	if (IS_ERR_OR_NULL(folio_lu))
		return -EFAULT;

	iommufd_lu = folio_address(folio_lu);
	iommufd_lu->folio_lu = folio_lu;

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

	*file_p = file;

	return 0;

err_ctx_put:
	iommufd_ctx_put(ictx);
err_fput:
	fput(file);
err_folio_put:
	folio_put(folio_lu);
	return rc;
}

int iommufd_hwpt_lu_restore(struct iommufd_ucmd *ucmd)
{
	return -ENOTTY;
}

static void iommufd_liveupdate_finish(struct liveupdate_file_handler *handler,
				      struct file *file, u64 data, bool reclaimed)
{
	struct iommufd_lu *iommufd_lu;
	struct iommufd_ctx *ictx;
	struct folio *folio_lu;

	if (!reclaimed || !file) {
		pr_warn("%s: fd not reclaimed\n", __func__);

		folio_lu = kho_restore_folio(data);
		if (WARN_ON_ONCE(IS_ERR_OR_NULL(folio_lu)))
			return;

		iommufd_lu = folio_address(folio_lu);
	} else {
		ictx = iommufd_ctx_from_file(file);
		iommufd_lu = ictx->lu;
		ictx->lu = NULL;
		iommufd_ctx_put(ictx);
	}

	folio_put(iommufd_lu->folio_lu);
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
