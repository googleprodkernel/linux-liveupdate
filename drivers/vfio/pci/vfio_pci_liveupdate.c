// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */

/**
 * DOC: VFIO PCI Live Update
 *
 * vfio-pci can preserve devices across a kexec Live Update, so that they keep
 * running while the kernel is replaced: a preserved device is not reset, its
 * Bus Master Enable stays set, it stays in D0, and it can keep doing DMA. The
 * next kernel takes over the device without reconfiguring it.
 *
 * Userspace preserves a device by preserving a VFIO device file for it in a
 * Live Update session with ``ioctl(LIVEUPDATE_SESSION_PRESERVE_FD)``. Only
 * files opened through the device's character device
 * (``/dev/vfio/devices/vfioX``) and bound to an iommufd with
 * ``VFIO_DEVICE_BIND_IOMMUFD`` can be preserved, and only for devices bound to
 * vfio-pci itself (not to a vfio-pci variant driver). LUO holds a reference on
 * a preserved file, so the device stays open (and is not reset by closing it)
 * for as long as it is preserved.
 *
 * Preserving the file also preserves the device with the PCI core (see
 * Documentation/PCI/liveupdate.rst), which takes care of the device's PCI
 * state (e.g. its BARs and bus numbers) and binds the device to vfio-pci again
 * in the next kernel.
 *
 * Preserved State
 * ===============
 *
 * vfio-pci preserves as little about a device as possible, and re-derives
 * everything else from the device in the next kernel.
 *
 * When userspace first opens a device, vfio-pci resets it and saves its state
 * (pci_store_saved_state()), and when userspace closes the device, vfio-pci
 * restores that state and resets the device, so that the device is handed
 * back in the state it was in before userspace used it. A preserved device is
 * not reset, so that saved state can't be re-derived in the next kernel: it is
 * passed to the next kernel in the format defined by the PCI core (struct
 * pci_saved_state_ser), so that closing the device after the Live Update
 * behaves exactly as it would have without one.
 *
 * Freezing
 * ========
 *
 * Right before kexec, LUO freezes preserved files. Freezing a device file
 * fails with -EBUSY unless userspace has disabled all of the device's
 * interrupts (``VFIO_DEVICE_SET_IRQS``), since interrupts are not preserved,
 * and unless the device is in D0 and not in a low power state entered with
 * ``VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY``.
 *
 * Freezing does not stop the device: unlike closing the device, it does not
 * reset the device or clear its Bus Master Enable, so that the device can
 * keep doing DMA. Userspace can keep accessing the device (e.g. its BARs)
 * while it is frozen, but enabling interrupts fails with -EBUSY. If the Live
 * Update is aborted, the device file is unfrozen.
 *
 * The only thing freezing changes is INTx Disable, which it sets: disabling
 * MSI or MSI-X clears INTx Disable (see pci_intx_for_msi()), and nothing in
 * the next kernel handles INTx until userspace sets up interrupts again, so
 * an INTx asserted on a line shared with other devices would get the line
 * disabled for all of them. INTx Disable stays set until userspace sets up
 * interrupts again (in either kernel), which reprograms it. The emulated INTx
 * Disable bit userspace sees is not changed.
 *
 * Power Management
 * ================
 *
 * A preserved device must stay in D0 to keep running. While a device is
 * preserved (from preserving its file until unpreserving it, and in the next
 * kernel until it finishes participating in the Live Update), vfio-pci refuses
 * ``VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY`` and
 * ``VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY_WITH_WAKEUP`` and writes to the PM
 * Control/Status register that would move the device out of D0 with -EBUSY.
 * It also refuses ``VFIO_DEVICE_RESET``, ``VFIO_DEVICE_PCI_HOT_RESET``, and
 * Function Level Resets written to the PCI Express or Advanced Features
 * capability with -EBUSY (the PCI core refuses to reset preserved devices
 * anyway).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kexec_handover.h>
#include <linux/kho/abi/vfio_pci.h>
#include <linux/liveupdate.h>
#include <linux/pci.h>
#include <linux/vfio.h>
#include <linux/vfio_pci_core.h>

#include "vfio_pci_priv.h"

/* Return the vfio-pci device of @file, if it is a vfio-pci device cdev file. */
static struct vfio_pci_core_device *vfio_pci_liveupdate_vdev(struct file *file)
{
	struct vfio_device *device = vfio_device_cdev_file_device(file);

	if (!device || device->ops != &vfio_pci_ops)
		return NULL;

	return container_of(device, struct vfio_pci_core_device, vdev);
}

static bool vfio_pci_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					     struct file *file)
{
	/*
	 * A cdev device can have several files open, but only the one bound to
	 * an iommufd has the device open, and only one file can be bound at a
	 * time. Since LUO holds a reference on preserved files, preserving
	 * only bound files guarantees that the device stays open while it is
	 * preserved, and is not reset by closing it.
	 */
	return vfio_pci_liveupdate_vdev(file) &&
	       vfio_device_cdev_file_is_bound(file);
}

/* Free the state preserved by vfio_pci_liveupdate_preserve(). */
static void vfio_pci_liveupdate_unpreserve_ser(struct vfio_pci_core_device_ser *ser)
{
	if (ser->saved_state)
		kho_unpreserve_free(phys_to_virt(ser->saved_state));

	kho_unpreserve_free(ser);
}

static int vfio_pci_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device *vdev = vfio_pci_liveupdate_vdev(args->file);
	struct pci_dev *pdev = vdev->pdev;
	struct vfio_pci_core_device_ser *ser;
	size_t saved_state_size;
	void *saved_state;
	int ret;

	/*
	 * Intel IGD devices expose extra regions backed by firmware tables and
	 * host bridge state that are not preserved.
	 */
	if (vfio_pci_is_intel_display(pdev))
		return -EOPNOTSUPP;

	/* Serializes against open and close, which set pci_saved_state. */
	guard(mutex)(&vdev->vdev.dev_set->lock);

	/*
	 * A device preserved by the previous kernel must finish participating
	 * in that Live Update before it can be preserved for the next one.
	 */
	if (READ_ONCE(vdev->vdev.liveupdate_incoming))
		return -EBUSY;

	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser))
		return PTR_ERR(ser);

	ser->size = sizeof(*ser);
	ser->domain = pci_domain_nr(pdev->bus);
	ser->bdf = pci_dev_id(pdev);

	saved_state_size = pci_saved_state_ser_size(vdev->pci_saved_state);
	if (saved_state_size) {
		saved_state = kho_alloc_preserve(saved_state_size);
		if (IS_ERR(saved_state)) {
			ret = PTR_ERR(saved_state);
			goto err_free;
		}

		ser->saved_state = virt_to_phys(saved_state);
		ret = pci_saved_state_serialize(vdev->pci_saved_state,
						saved_state, saved_state_size);
		if (ret)
			goto err_free;
	}

	ret = pci_liveupdate_preserve(pdev);
	if (ret) {
		pci_err(pdev, "Failed to preserve device (%d)\n", ret);
		goto err_free;
	}

	args->serialized_data = virt_to_phys(ser);
	return 0;

err_free:
	vfio_pci_liveupdate_unpreserve_ser(ser);
	return ret;
}

static void vfio_pci_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device *vdev = vfio_pci_liveupdate_vdev(args->file);

	guard(mutex)(&vdev->vdev.dev_set->lock);

	pci_liveupdate_unpreserve(vdev->pdev);
	vfio_pci_liveupdate_unpreserve_ser(phys_to_virt(args->serialized_data));
}

static int vfio_pci_liveupdate_freeze(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device *vdev = vfio_pci_liveupdate_vdev(args->file);
	int ret;

	ret = pci_liveupdate_freeze(vdev->pdev);
	if (ret)
		return ret;

	ret = vfio_pci_core_liveupdate_freeze(vdev);
	if (ret)
		pci_liveupdate_unfreeze(vdev->pdev);

	return ret;
}

static void vfio_pci_liveupdate_unfreeze(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device *vdev = vfio_pci_liveupdate_vdev(args->file);

	vfio_pci_core_liveupdate_unfreeze(vdev);
	pci_liveupdate_unfreeze(vdev->pdev);
}

static int vfio_pci_liveupdate_retrieve(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}

static bool vfio_pci_liveupdate_can_finish(struct liveupdate_file_op_args *args)
{
	/* Preserved devices can't be retrieved yet, and thus can't finish. */
	return false;
}

static void vfio_pci_liveupdate_finish(struct liveupdate_file_op_args *args)
{
}

static const struct liveupdate_file_ops vfio_pci_liveupdate_file_ops = {
	.can_preserve = vfio_pci_liveupdate_can_preserve,
	.preserve = vfio_pci_liveupdate_preserve,
	.unpreserve = vfio_pci_liveupdate_unpreserve,
	.freeze = vfio_pci_liveupdate_freeze,
	.unfreeze = vfio_pci_liveupdate_unfreeze,
	.retrieve = vfio_pci_liveupdate_retrieve,
	.can_finish = vfio_pci_liveupdate_can_finish,
	.finish = vfio_pci_liveupdate_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler vfio_pci_liveupdate_fh = {
	.ops = &vfio_pci_liveupdate_file_ops,
	.compatible = VFIO_PCI_LUO_FH_COMPATIBLE,
};

static bool vfio_pci_liveupdate_registered;

void __init vfio_pci_liveupdate_init(void)
{
	int ret;

	ret = liveupdate_register_file_handler(&vfio_pci_liveupdate_fh);
	if (ret) {
		/* -EOPNOTSUPP means Live Update is disabled. */
		if (ret != -EOPNOTSUPP)
			pr_warn("Failed to register Live Update file handler (%d)\n",
				ret);
		return;
	}

	ret = pci_liveupdate_register_flb(&vfio_pci_liveupdate_fh);
	if (ret) {
		pr_warn("Failed to register with PCI Live Update (%d)\n", ret);
		liveupdate_unregister_file_handler(&vfio_pci_liveupdate_fh);
		return;
	}

	vfio_pci_liveupdate_registered = true;
}

void vfio_pci_liveupdate_cleanup(void)
{
	if (!vfio_pci_liveupdate_registered)
		return;

	pci_liveupdate_unregister_flb(&vfio_pci_liveupdate_fh);
	liveupdate_unregister_file_handler(&vfio_pci_liveupdate_fh);
}
