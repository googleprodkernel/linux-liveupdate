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

static void *iommufd_lu_init_section_ptrs(struct iommufd_lu *iommufd_lu)
{
	iommufd_lu->hwpts = (void *)(iommufd_lu + 1);
	iommufd_lu->attaches = (void *)(iommufd_lu->hwpts +
				       iommufd_lu->nr_hwpts);
	return iommufd_lu->attaches + iommufd_lu->nr_attaches;
}

static bool iommufd_lu_is_counter(struct iommufd_lu *iommufd_lu)
{
	return iommufd_lu->hwpts == NULL;
}

static int iommufd_save_devices(struct iommufd_ctx *ictx,
				struct iommufd_lu *iommufd_lu)
{
	struct iommufd_attach_lu *attach_lu;
	unsigned long index_device, pasid;
	struct iommufd_hwpt_paging *hwpt;
	struct iommufd_hwpt_lu *hwpt_lu;
	struct iommufd_attach *attach;
	struct iommufd_device *idev;
	struct iommufd_object *obj;
	unsigned int nr_hwpts = 0;
	struct xarray seen_hwpts;
	void *xa_value, *xa_old;
	struct pci_dev *pdev;
	int rc = 0;

	xa_init(&seen_hwpts);

	xa_lock(&ictx->objects);
	xa_for_each(&ictx->objects, index_device, obj) {
		if (obj->type != IOMMUFD_OBJ_DEVICE)
			continue;

		idev = container_of(obj, struct iommufd_device, obj);
		if (!dev_is_preserved(idev->dev))
			continue;

		pdev = to_pci_dev(idev->dev);

		xa_for_each(&idev->igroup->pasid_attach, pasid, attach) {
			if (!xa_load(&attach->device_array, idev->obj.id))
				continue;

			hwpt = find_hwpt_paging(attach->hwpt);
			if (!hwpt->lu_preserved) {
				rc = -EINVAL;
				goto out;
			}

			/* TODO: The HWPT should be made immutable */

			if (!hwpt->common.domain) {
				rc = -EINVAL;
				goto out;
			}

			xa_value = xa_load(&seen_hwpts, hwpt->common.obj.id);
			if (!xa_value) {
				xa_old = xa_store(&seen_hwpts, hwpt->common.obj.id,
						  xa_mk_value(nr_hwpts++), GFP_ATOMIC);
				if (xa_is_err(xa_old)) {
					rc = xa_err(xa_old);
					goto out;
				}

				if (!iommufd_lu_is_counter(iommufd_lu)) {
					hwpt_lu = iommufd_lu->hwpts++;

					hwpt_lu->token = hwpt->lu_token;
					hwpt_lu->reclaimed = false;
					hwpt_lu->iommu_context =
						iommu_domain_preserve(hwpt->common.domain);
					if (hwpt_lu->iommu_context < 0) {
						rc = hwpt_lu->iommu_context;
						goto out;
					}
				}
			} else if (!iommufd_lu_is_counter(iommufd_lu)) {
				hwpt_lu = &iommufd_lu->hwpts[xa_to_value(xa_value)];
			}

			if (iommufd_lu_is_counter(iommufd_lu)) {
				iommufd_lu->nr_attaches++;
			} else {
				attach_lu = iommufd_lu->attaches++;

				attach_lu->hwpt_idx = xa_to_value(xa_value);
				attach_lu->pasid = pasid;
				attach_lu->pci_domain = pci_domain_nr(pdev->bus);
				attach_lu->dev_id = pci_dev_id(pdev);
			}
		}
	}

	if (iommufd_lu_is_counter(iommufd_lu))
		iommufd_lu->nr_hwpts = nr_hwpts;
	else if (WARN_ON(iommufd_lu->nr_hwpts != nr_hwpts))
		rc = -EFAULT;

out:
	xa_unlock(&ictx->objects);
	xa_destroy(&seen_hwpts);
	return rc;
}

static int iommufd_liveupdate_prepare(struct liveupdate_file_handler *handler,
				      struct file *file, u64 *data)
{
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(file);
	struct iommufd_lu iommufd_lu_counter = {0};
	struct iommufd_lu *iommufd_lu;
	struct folio *folio_lu;
	size_t serial_size;
	void *serial_end;
	int rc;

	if (IS_ERR(ictx))
		return PTR_ERR(ictx);

	rc = iommufd_save_devices(ictx, &iommufd_lu_counter);
	if (rc)
		goto err_ctx_put;

	serial_end = iommufd_lu_init_section_ptrs(&iommufd_lu_counter);
	serial_size = serial_end - (void *)&iommufd_lu_counter;

	folio_lu = folio_alloc(GFP_KERNEL, get_order(serial_size));
	if (!folio_lu) {
		rc = -ENOMEM;
		goto err_ctx_put;
	}

	iommufd_lu = folio_address(folio_lu);
	*iommufd_lu = iommufd_lu_counter;
	iommufd_lu_init_section_ptrs(iommufd_lu);
	rc = iommufd_save_devices(ictx, iommufd_lu);
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
	struct iommufd_ctx *ictx = iommufd_ctx_from_file(file);
	struct folio *folio_lu;

	if (WARN_ON(IS_ERR(ictx)))
		return;

	/* TODO: call iommu_unpreserve_domain for all preserved domains */

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

	iommufd_lu_init_section_ptrs(iommufd_lu);

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

static struct iommufd_device *
iommufd_lu_find_device(struct iommufd_ctx *ictx, struct iommufd_attach_lu *attach_lu)
{
	struct iommufd_device *idev;
	struct iommufd_object *obj;
	struct pci_dev *pdev;
	unsigned long index;

	xa_lock(&ictx->objects);
	xa_for_each(&ictx->objects, index, obj) {
		if (obj->type != IOMMUFD_OBJ_DEVICE)
			continue;

		idev = container_of(obj, struct iommufd_device, obj);
		if (!dev_is_pci(idev->dev))
			continue;

		pdev = to_pci_dev(idev->dev);
		if (attach_lu->pci_domain != pci_domain_nr(pdev->bus))
			continue;
		if (attach_lu->dev_id != pci_dev_id(pdev))
			continue;

		xa_unlock(&ictx->objects);
		return idev;
	}

	xa_unlock(&ictx->objects);
	return ERR_PTR(-ENODEV);
}

int iommufd_hwpt_lu_restore(struct iommufd_ucmd *ucmd)
{
	unsigned int hwpt_idx, attach_idx, attach_idx_free;
	struct iommu_hwpt_lu_restore *cmd = ucmd->cmd;
	struct iommufd_hwpt_paging *hwpt = NULL;
	struct iommufd_device *idev, **idev_arr;
	struct iommufd_ctx *ictx = ucmd->ictx;
	struct iommufd_attach_lu *attach_lu;
	struct iommufd_hwpt_lu *hwpt_lu;
	struct iommufd_lu *iommufd_lu;
	int rc;

	iommufd_lu = ictx->lu;
	if (!iommufd_lu)
		return -ENOTTY;

	for (hwpt_idx = 0; hwpt_idx < iommufd_lu->nr_hwpts; hwpt_idx++) {
		hwpt_lu = &iommufd_lu->hwpts[hwpt_idx];

		if (hwpt_lu->reclaimed)
			continue;

		if (hwpt_lu->token == cmd->hwpt_token)
			goto hwpt_found;
	}

	return -ENOENT;

hwpt_found:
	idev_arr = kcalloc(iommufd_lu->nr_attaches, sizeof(*idev_arr), GFP_KERNEL);
	if (!idev_arr)
		return -ENOMEM;

	for (attach_idx = 0; attach_idx < iommufd_lu->nr_attaches; attach_idx++) {
		attach_lu = &iommufd_lu->attaches[attach_idx];
		if (attach_lu->hwpt_idx != hwpt_idx)
			continue;

		idev = iommufd_lu_find_device(ictx, attach_lu);
		if (IS_ERR(idev)) {
			rc = PTR_ERR(idev);
			goto err_free;
		}

		idev_arr[attach_idx] = idev;
	}

	hwpt_lu->reclaimed = true;

	for (attach_idx = 0; attach_idx < iommufd_lu->nr_attaches; attach_idx++) {
		attach_lu = &iommufd_lu->attaches[attach_idx];
		if (attach_lu->hwpt_idx != hwpt_idx)
			continue;

		idev = idev_arr[attach_idx];

		if (!hwpt) {
			hwpt = iommufd_hwpt_paging_alloc(ictx, NULL, idev,
							 attach_lu->pasid,
							 cmd->hwpt_alloc_flags,
							 false, NULL);
			if (IS_ERR(hwpt)) {
				rc = PTR_ERR(hwpt);
				goto err;
			}

			iommufd_object_finalize(idev->ictx, &hwpt->common.obj);
			cmd->pt_id = hwpt->common.obj.id;
		}

		rc = iommufd_device_attach(idev, attach_lu->pasid, &cmd->pt_id);
		if (rc)
			goto err;
	}

	kfree(idev_arr);
	return 0;

err:
	/* TODO: What should be done here? attach should never fail because
	 * the domain is already attached as a preserved state, so detaching it
	 * is technically wrong */
	attach_idx_free = attach_idx;
	for (attach_idx = 0; attach_idx < attach_idx_free; attach_idx++) {
		attach_lu = &iommufd_lu->attaches[attach_idx];
		if (attach_lu->hwpt_idx != hwpt_idx)
			continue;

		idev = idev_arr[attach_idx];
		iommufd_device_detach(idev, attach_lu->pasid);
	}

	if (hwpt)
		iommufd_object_remove(idev->ictx, &hwpt->common.obj,
				      hwpt->common.obj.id, 0);

	hwpt_lu->reclaimed = false;
err_free:
	kfree(idev_arr);
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

	/* TODO: Iterate all objects for any read-only HWPTs that is referenced */

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
