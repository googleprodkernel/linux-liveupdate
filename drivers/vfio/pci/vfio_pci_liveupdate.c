// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Vipin Sharma <vipinsh@google.com>
 * David Matlack <dmatlack@google.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/vfio_pci.h>
#include <linux/liveupdate.h>
#include <linux/errno.h>
#include <linux/vfio.h>

#include "vfio_pci_priv.h"

static bool vfio_pci_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					     struct file *file)
{
	struct vfio_device_file *df = to_vfio_device_file(file);

	if (!df)
		return false;

	/* Live Update support is limited to cdev files. */
	if (df->group)
		return false;

	return df->device->ops == &vfio_pci_ops;
}

static int vfio_pci_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device = vfio_device_from_file(args->file);
	struct vfio_pci_core_device_ser *ser;
	struct vfio_pci_core_device *vdev;
	struct pci_dev *pdev;
	struct folio *folio;
	int err;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;

	if (IS_ENABLED(CONFIG_VFIO_PCI_ZDEV_KVM))
		return -EINVAL;

	if (vfio_pci_is_intel_display(pdev))
		return -EINVAL;

	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, get_order(sizeof(*ser)));
	if (!folio)
		return -ENOMEM;

	ser = folio_address(folio);

	ser->bdf = pci_dev_id(pdev);
	ser->domain = pci_domain_nr(pdev->bus);

	err = kho_preserve_folio(folio);
	if (err)
		goto error;

	pci_liveupdate_outgoing_preserve(pdev);
	args->serialized_data = virt_to_phys(ser);
	return 0;

error:
	folio_put(folio);
	return err;
}

static void vfio_pci_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device_ser *ser = phys_to_virt(args->serialized_data);
	struct vfio_device *device = vfio_device_from_file(args->file);
	struct folio *folio = virt_to_folio(ser);

	pci_liveupdate_outgoing_unpreserve(to_pci_dev(device->dev));
	kho_unpreserve_folio(folio);
	folio_put(folio);
}

static int vfio_pci_liveupdate_freeze(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device = vfio_device_from_file(args->file);
	struct vfio_pci_core_device *vdev;
	struct pci_dev *pdev;
	int ret;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;

	guard(mutex)(&device->dev_set->lock);

	/*
	 * Userspace must disable interrupts on the device prior to freeze so
	 * that the device does not send any interrupts until new interrupt
	 * handlers have been established by the next kernel.
	 */
	if (vdev->irq_type != VFIO_PCI_NUM_IRQS) {
		pci_err(pdev, "Freeze failed! Interrupts are still enabled.\n");
		return -EINVAL;
	}

	pci_dev_lock(pdev);

	ret = pci_load_and_free_saved_state(pdev, &vdev->pci_saved_state);
	if (ret)
		goto out;

	/*
	 * Reset the device and restore it back to its original state before
	 * handing it to the next kernel.
	 *
	 * Eventually both of these should be dropped and the device should be
	 * kept running with its current state across the Live Update.
	 */
	if (vdev->reset_works)
		ret = __pci_reset_function_locked(pdev);

	pci_restore_state(pdev);

out:
	pci_dev_unlock(pdev);
	return ret;
}

static int match_device(struct device *dev, const void *arg)
{
	struct vfio_device *device = container_of(dev, struct vfio_device, device);
	const struct vfio_pci_core_device_ser *ser = arg;
	struct vfio_pci_core_device *vdev;
	struct pci_dev *pdev;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;

	return ser->bdf == pci_dev_id(pdev) && ser->domain == pci_domain_nr(pdev->bus);
}

static int vfio_pci_liveupdate_retrieve(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device_ser *ser;
	struct vfio_pci_core_device *vdev;
	struct vfio_device *device;
	struct folio *folio;
	struct file *file;
	int ret;

	folio = kho_restore_folio(args->serialized_data);
	if (!folio)
		return -ENOENT;

	ser = folio_address(folio);

	device = vfio_find_device(ser, match_device);
	if (!device)
		return -ENODEV;

	/*
	 * During a Live Update userspace retrieves preserved VFIO cdev files by
	 * issuing an ioctl on /dev/liveupdate rather than by opening VFIO
	 * character devices.
	 *
	 * To handle that scenario, this routine simulates opening the VFIO
	 * character device for userspace with an anonymous inode. The returned
	 * file has the same properties as a cdev file (e.g. operations are
	 * blocked until BIND_IOMMUFD is called), aside from the inode
	 * association.
	 */
	file = anon_inode_getfile_fmode("[vfio-device-liveupdate]",
					&vfio_device_fops, NULL,
					O_RDWR, FMODE_PREAD | FMODE_PWRITE);

	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto out;
	}

	ret = __vfio_device_fops_cdev_open(device, file);
	if (ret) {
		fput(file);
		goto out;
	}

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	vdev->liveupdate_state = ser;

	args->file = file;

out:
	/* Drop the reference from vfio_find_device() */
	put_device(&device->device);

	return ret;
}

static bool vfio_pci_liveupdate_can_finish(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device *vdev;
	struct vfio_device *device;

	if (!args->retrieved)
		return false;

	device = vfio_device_from_file(args->file);
	vdev = container_of(device, struct vfio_pci_core_device, vdev);

	/*
	 * Ensure VFIO is done using vdev->liveupdate_state, which means its
	 * safe for vfio_pci_liveupdate_finish() to free it.
	 */
	guard(mutex)(&device->dev_set->lock);
	return !vdev->liveupdate_state;
}

static void vfio_pci_liveupdate_finish(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device = vfio_device_from_file(args->file);
	struct folio *folio;

	pci_liveupdate_incoming_finish(to_pci_dev(device->dev));

	folio = virt_to_folio(phys_to_virt(args->serialized_data));
	folio_put(folio);
}

static const struct liveupdate_file_ops vfio_pci_liveupdate_file_ops = {
	.can_preserve = vfio_pci_liveupdate_can_preserve,
	.preserve = vfio_pci_liveupdate_preserve,
	.unpreserve = vfio_pci_liveupdate_unpreserve,
	.freeze = vfio_pci_liveupdate_freeze,
	.retrieve = vfio_pci_liveupdate_retrieve,
	.can_finish = vfio_pci_liveupdate_can_finish,
	.finish = vfio_pci_liveupdate_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler vfio_pci_liveupdate_fh = {
	.ops = &vfio_pci_liveupdate_file_ops,
	.compatible = VFIO_PCI_LUO_FH_COMPATIBLE,
};

int __init vfio_pci_liveupdate_init(void)
{
	int ret;

	if (!liveupdate_enabled())
		return 0;

	ret = liveupdate_register_file_handler(&vfio_pci_liveupdate_fh);
	if (ret)
		return ret;

	ret = pci_liveupdate_register_fh(&vfio_pci_liveupdate_fh);
	if (ret)
		goto error;

	return 0;

error:
	liveupdate_unregister_file_handler(&vfio_pci_liveupdate_fh);
	return ret;
}

void vfio_pci_liveupdate_cleanup(void)
{
	if (!liveupdate_enabled())
		return;

	WARN_ON_ONCE(pci_liveupdate_unregister_fh(&vfio_pci_liveupdate_fh));
	liveupdate_unregister_file_handler(&vfio_pci_liveupdate_fh);
}
