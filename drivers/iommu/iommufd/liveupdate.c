// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/mm.h>
#include <linux/pci.h>

#include "iommufd_private.h"

/* TODO: Function to check if device is marked for preservation */
#define dev_is_preserved(dev) dev_is_pci(dev)

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
			rc = -EADDRINUSE;
			goto out;
		}
	}

	hwpt_target->lu_preserved = cmd->preserved;
	hwpt_target->lu_token = cmd->hwpt_token;

out:
	xa_unlock(&ictx->objects);
	iommufd_put_object(ictx, &hwpt_target->common.obj);
	return rc;
}

static int iommufd_save_hwpts(struct iommufd_ctx *ictx,
			      struct iommufd_lu *iommufd_lu)
{
	struct iommufd_hwpt_paging *hwpt, **hwpts = NULL;
	struct iommufd_hwpt_lu *hwpt_lu;
	struct iommufd_object *obj;
	unsigned int nr_hwpts = 0;
	unsigned long index;
	unsigned int i;
	int rc = 0;

	if (iommufd_lu) {
		hwpts = kcalloc(iommufd_lu->nr_hwpts, sizeof(*hwpts),
				GFP_KERNEL);
		if (!hwpts)
			return -ENOMEM;
	}

	xa_lock(&ictx->objects);
	xa_for_each(&ictx->objects, index, obj) {
		if (obj->type != IOMMUFD_OBJ_HWPT_PAGING)
			continue;

		hwpt = container_of(obj, struct iommufd_hwpt_paging, common.obj);
		if (!hwpt->lu_preserved)
			continue;

		/* TODO: The HWPT should be made immutable, and cannot be
		 * destroyed */

		if (!hwpt->common.domain) {
			rc = -EINVAL;
			xa_unlock(&ictx->objects);
			goto out;
		}

		if (iommufd_lu) {
			hwpts[nr_hwpts] = hwpt;
			hwpt_lu = &iommufd_lu->hwpts[nr_hwpts];

			hwpt_lu->token = hwpt->lu_token;
			hwpt_lu->reclaimed = false;
		}

		nr_hwpts++;
	}
	xa_unlock(&ictx->objects);

	if (WARN_ON(iommufd_lu && iommufd_lu->nr_hwpts != nr_hwpts)) {
		rc = -EFAULT;
		goto out;
	}

	if (iommufd_lu) {
		/* iommu_domain_preserve may sleep and must be called
		 * outside of xa_lock */
		for (i = 0; i < nr_hwpts; i++) {
			hwpt = hwpts[nr_hwpts];
			hwpt_lu = &iommufd_lu->hwpts[nr_hwpts];

			hwpt_lu->iommu_hwpt_token =
				iommu_domain_preserve(hwpt->common.domain);
			if (hwpt_lu->iommu_hwpt_token < 0) {
				rc = hwpt_lu->iommu_hwpt_token;
				goto out;
			}
		}
	}

	rc = nr_hwpts;

out:
	kfree(hwpts);
	return rc;
}

static int iommufd_liveupdate_prepare(struct liveupdate_file_handler *handler,
				      struct file *file, u64 *data)
{
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(args->file);
	struct iommufd_lu *iommufd_lu;
	struct folio *folio_lu;
	size_t serial_size;
	int rc;

	if (IS_ERR(ictx))
		return PTR_ERR(ictx);

	rc = iommufd_save_hwpts(ictx, NULL);
	if (rc < 0)
		goto err_ctx_put;

	serial_size = struct_size(iommufd_lu, hwpts, rc);

	folio_lu = folio_alloc(GFP_KERNEL, get_order(serial_size));
	if (!folio_lu) {
		rc = -ENOMEM;
		goto err_ctx_put;
	}

	iommufd_lu = folio_address(folio_lu);
	rc = iommufd_save_hwpts(ictx, iommufd_lu);
	if (rc)
		goto err_folio_put;

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
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(args->file);
	struct iommufd_hwpt_paging *hwpt;
	struct iommufd_object *obj;
	struct folio *folio_lu;
	unsigned long index;

	if (WARN_ON(IS_ERR(ictx)))
		return;

	xa_lock(&ictx->objects);
	xa_for_each(&ictx->objects, index, obj) {
		if (obj->type != IOMMUFD_OBJ_HWPT_PAGING)
			continue;

		hwpt = container_of(obj, struct iommufd_hwpt_paging, common.obj);
		if (!hwpt->lu_preserved)
			continue;

		/* TODO: The HWPT should be made mutable again */

		if (!hwpt->common.domain)
			continue;

		WARN_ON(iommu_domain_unpreserve(hwpt->common.domain));
	}
	xa_unlock(&ictx->objects);

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
	struct iommu_hwpt_lu_restore *cmd = ucmd->cmd;
	struct iommufd_hwpt_paging *hwpt = NULL;
	struct iommufd_ctx *ictx = ucmd->ictx;
	struct iommufd_hwpt_lu *hwpt_lu;
	struct iommufd_lu *iommufd_lu;
	struct iommu_domain *domain;
	unsigned int i;
	int rc;

	iommufd_lu = ictx->lu;
	if (!iommufd_lu)
		return -ENOTTY;

	for (i = 0; i < iommufd_lu->nr_hwpts; i++) {
		hwpt_lu = &iommufd_lu->hwpts[i];

		if (hwpt_lu->reclaimed)
			continue;

		if (hwpt_lu->token == cmd->hwpt_token)
			goto hwpt_found;
	}

	return -ENOENT;

hwpt_found:
	hwpt = _iommufd_hwpt_paging_alloc(ictx);
	if (IS_ERR(hwpt))
		return PTR_ERR(hwpt);

	/* a successful iommu_domain_restore mars the point of no return */
	domain = iommu_domain_restore(hwpt_lu->iommu_hwpt_token);
	if (IS_ERR(domain)) {
		rc = PTR_ERR(domain);
		goto err_destroy;
	}

	iommufd_hwpt_init_from_domain(&hwpt->common, domain);
	iommufd_object_finalize(ictx, &hwpt->common.obj);

	hwpt_lu->reclaimed = true;
	cmd->pt_id = hwpt->common.obj.id;
	return 0;

err_destroy:
	iommufd_object_abort_and_destroy(ictx, &hwpt->common.obj);
	return rc;
}

static void iommufd_liveupdate_finish(struct liveupdate_file_handler *handler,
				      struct file *file, u64 data, bool reclaimed)
{
	struct iommufd_hwpt_lu *hwpt_lu;
	struct iommufd_lu *iommufd_lu;
	struct iommufd_ctx *ictx;
	unsigned int i;
	int rc;

	if (!reclaimed || !file) {
		pr_warn("%s: fd not reclaimed\n", __func__);
		return /* -EBUSY */;
	}

	ictx = iommufd_ctx_from_file(file);
	iommufd_lu = ictx->lu;

	for (i = 0; i < iommufd_lu->nr_hwpts; i++) {
		hwpt_lu = &iommufd_lu->hwpts[i];

		if (!hwpt_lu->reclaimed) {
			rc = -EBUSY;
			goto err;
		}
	}

	/* TODO: Iterate all objects for any restored HWPTs that is in use */

	ictx->lu = NULL;
	folio_put(iommufd_lu->folio_lu);

err:
	iommufd_ctx_put(ictx);
	return /* rc */;
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
