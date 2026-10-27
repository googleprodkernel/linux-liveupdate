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
 * vfio-pci also passes the virtual configuration space it presents to
 * userspace for the device (as of when the device file is frozen) to the next
 * kernel. It differs from the device's configuration space in the registers
 * vfio-pci emulates, e.g. the BARs and Expansion ROM BAR (as written by
 * userspace, e.g. to size them), Interrupt Line, INTx Disable, PME Enable and
 * Status, some Device Control fields, and the MSI and MSI-X capabilities, so
 * it can't be rebuilt from the device. When userspace first opens the device,
 * the next kernel builds the virtual configuration space as usual and then
 * restores the bits of it that userspace can write and that vfio-pci emulates
 * (according to the next kernel's emulation) from the previous kernel's.
 * Everything else, e.g. the virtualized capability list, comes from the next
 * kernel's emulation, which can differ from the previous kernel's. If the size
 * of the previous kernel's virtual configuration space doesn't match the
 * device's configuration space, it is ignored.
 *
 * Everything else is re-derived when userspace first opens the device in the
 * next kernel, without resetting the device or writing to it:
 *
 *  * Whether resetting the device works, which vfio-pci normally finds out by
 *    resetting the device when userspace first opens it, is assumed if the
 *    device supports a reset method (pci_reset_supported()). This decides
 *    whether ``VFIO_DEVICE_RESET`` is allowed and whether
 *    ``VFIO_DEVICE_FLAGS_RESET`` is reported by ``VFIO_DEVICE_GET_INFO``,
 *    which can therefore change across a Live Update if resetting the device
 *    failed in the previous kernel even though it supports a reset method.
 *
 *  * The map of capabilities in the device's configuration space and the BAR
 *    values vfio-pci restores after a reset are rebuilt from the device, like
 *    for any first open. The device is not reset and the PCI core keeps its
 *    BARs, so these match the previous kernel's.
 *
 *  * Interrupts must be disabled before kexec (see Freezing below), and
 *    userspace sets them up again after retrieving the device's file, along
 *    with everything else it sets up through the file (e.g. eventfds,
 *    ioeventfds, BAR mappings, dma-bufs, and the iommufd binding).
 *
 * When userspace first opens the device in the next kernel, vfio-pci leaves
 * the device as it is: unlike for a device that nobody used before, it does
 * not clear the device's Bus Master Enable, reset it, save its state, or
 * change its INTx Disable bit. After the device finishes participating in the
 * Live Update, closing it restores the saved state passed by the previous
 * kernel and resets it, like it would have without a Live Update.
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
 * while it is frozen, but enabling interrupts and writing to the device's
 * configuration space, which would change the virtual configuration space
 * passed to the next kernel, fail with -EBUSY. If the Live Update is aborted,
 * the device file is unfrozen.
 *
 * The only thing freezing changes is INTx Disable, which it sets: disabling
 * MSI or MSI-X clears INTx Disable (see pci_intx_for_msi()), and nothing in
 * the next kernel handles INTx until userspace sets up interrupts again, so
 * an INTx asserted on a line shared with other devices would get the line
 * disabled for all of them. INTx Disable stays set until userspace sets up
 * interrupts again (in either kernel), which reprograms it. The emulated INTx
 * Disable bit userspace sees is not changed. For the same reason, disabling
 * MSI or MSI-X of a preserved device (as userspace must before kexec) sets
 * INTx Disable right away, so that INTx is not left unmasked and unhandled
 * between disabling interrupts and freezing.
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
 *
 * In the next kernel, vfio-pci normally lets an idle device (i.e. one that
 * userspace has not opened) runtime suspend to D3 right after binding to it.
 * Instead, vfio-pci keeps an incoming preserved device runtime active (by
 * holding the runtime PM reference taken for probe) whenever it is not open,
 * from binding to it until it finishes participating in the Live Update,
 * which also keeps the bridges above it active. While the device is open, the
 * reference vfio-pci takes for opening it does the same. After the device
 * finishes, the usual rules apply.
 *
 * Retrieving
 * ==========
 *
 * After kexec, the PCI core binds each preserved device to vfio-pci again.
 * Userspace then retrieves a file for the device from its Live Update session
 * with ``ioctl(LIVEUPDATE_SESSION_RETRIEVE_FD)``. The retrieved file is in the
 * same state as a file just opened through the device's character device:
 * userspace must bind it to an iommufd with ``VFIO_DEVICE_BIND_IOMMUFD`` (a
 * new one, since iommufds are not preserved), and then sets the device up
 * again, e.g. its interrupts and DMA mappings.
 *
 * Until the device finishes participating in the Live Update, it can only be
 * opened through the retrieved file: opening it through its character device
 * or with ``VFIO_GROUP_GET_DEVICE_FD`` fails with -EBUSY.
 *
 * Retrieving a file fails if its device is not bound to vfio-pci, and LUO
 * remembers the failure, so userspace must make sure the device is bound
 * first. In particular, vfio-pci only binds to devices without an IOMMU if
 * ``vfio.enable_unsafe_noiommu_mode=1`` is set when it probes them (e.g. on the
 * kernel command line), and VFs are only enumerated once their PF adopts them
 * (e.g. when userspace writes the PF's ``sriov_numvfs`` sysfs attribute).
 *
 * Finishing
 * =========
 *
 * ``ioctl(LIVEUPDATE_SESSION_FINISH)`` fails with -EBUSY until userspace has
 * retrieved the files of all devices preserved in the session and bound them
 * to an iommufd, i.e. taken the devices over, and until all of the preserved
 * VFs of a preserved PF have finished. There is no way around this: if
 * retrieving a file fails (LUO does not retry it), or userspace never binds a
 * retrieved file, the session can't be finished (and closing it fails), and
 * the device stays preserved, i.e. it can't be opened otherwise, reset, or
 * unbound from vfio-pci, until the next reboot.
 *
 * Once a device finishes participating in the Live Update, vfio-pci handles it
 * like any other device: it can be opened through its character device again
 * (though it is still open through the retrieved file), and it can enter low
 * power states and be reset. Closing it (when both userspace and LUO have
 * released the retrieved file) restores the saved state passed by the
 * previous kernel and resets the device, exactly as it would have without a
 * Live Update.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kexec_handover.h>
#include <linux/kho/abi/vfio_pci.h>
#include <linux/liveupdate.h>
#include <linux/mm.h>
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

	if (ser->vconfig)
		kho_unpreserve_free(phys_to_virt(ser->vconfig));

	kho_unpreserve_free(ser);
}

static int vfio_pci_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device *vdev = vfio_pci_liveupdate_vdev(args->file);
	struct pci_dev *pdev = vdev->pdev;
	struct vfio_pci_core_device_ser *ser;
	size_t saved_state_size;
	void *saved_state, *vconfig;
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

	/* Filled in when the file is frozen, see vfio_pci_liveupdate_freeze(). */
	vconfig = kho_alloc_preserve(pdev->cfg_size);
	if (IS_ERR(vconfig)) {
		ret = PTR_ERR(vconfig);
		goto err_free;
	}

	ser->vconfig = virt_to_phys(vconfig);
	ser->vconfig_size = pdev->cfg_size;

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
	struct vfio_pci_core_device_ser *ser = phys_to_virt(args->serialized_data);
	int ret;

	ret = pci_liveupdate_freeze(vdev->pdev);
	if (ret)
		return ret;

	ret = vfio_pci_core_liveupdate_freeze(vdev, phys_to_virt(ser->vconfig));
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
	struct folio *folio, *saved_state_folio = NULL, *vconfig_folio = NULL;
	struct pci_saved_state *saved_state = NULL;
	u8 *vconfig = NULL;
	struct vfio_pci_core_device_ser *ser;
	struct vfio_pci_core_device *vdev;
	struct vfio_device *device = NULL;
	struct pci_dev *pdev = NULL;
	struct file *file;
	int ret;

	/*
	 * The preserved state is freed whether retrieving succeeds or not:
	 * LUO does not retry a failed retrieve. Restore all of it before
	 * validating it, so that none of it leaks if it is invalid.
	 */
	folio = kho_restore_folio(args->serialized_data);
	if (!folio) {
		pr_err("Failed to restore preserved state\n");
		return -ENOENT;
	}

	/* A folio is always large enough for the fields read below. */
	ser = folio_address(folio);
	if (ser->saved_state)
		saved_state_folio = kho_restore_folio(ser->saved_state);
	if (ser->vconfig)
		vconfig_folio = kho_restore_folio(ser->vconfig);

	if (ser->size < VFIO_PCI_CORE_DEVICE_SER_MIN_SIZE ||
	    ser->size > folio_size(folio) || ser->flags || ser->reserved) {
		pr_err("Invalid preserved state for %04x:%02x:%02x.%d\n",
		       ser->domain, PCI_BUS_NUM(ser->bdf),
		       PCI_SLOT(ser->bdf & 0xff), PCI_FUNC(ser->bdf & 0xff));
		ret = -EINVAL;
		goto out_free;
	}

	if (ser->saved_state && !saved_state_folio) {
		pr_err("Failed to restore saved state\n");
		ret = -ENOENT;
		goto out_free;
	}

	if (ser->vconfig && !vconfig_folio) {
		pr_err("Failed to restore virtual config space\n");
		ret = -ENOENT;
		goto out_free;
	}

	pdev = pci_get_domain_bus_and_slot(ser->domain, PCI_BUS_NUM(ser->bdf),
					   ser->bdf & 0xff);
	if (!pdev) {
		pr_err("Preserved device %04x:%02x:%02x.%d not found\n",
		       ser->domain, PCI_BUS_NUM(ser->bdf),
		       PCI_SLOT(ser->bdf & 0xff), PCI_FUNC(ser->bdf & 0xff));
		ret = -ENODEV;
		goto out_free;
	}

	device = vfio_find_device(&pdev->dev);
	if (!device || device->ops != &vfio_pci_ops) {
		pci_err(pdev, "Preserved device not bound to vfio-pci\n");
		ret = -ENODEV;
		goto out_put;
	}

	if (saved_state_folio) {
		saved_state = pci_saved_state_deserialize(pdev,
							  folio_address(saved_state_folio),
							  folio_size(saved_state_folio));
		if (IS_ERR(saved_state)) {
			ret = PTR_ERR(saved_state);
			saved_state = NULL;
			goto out_put;
		}
	}

	/*
	 * Without the virtual config space passed by the previous kernel, the
	 * first open rebuilds it from the device, like for any first open.
	 */
	if (vconfig_folio) {
		if (ser->vconfig_size != pdev->cfg_size ||
		    ser->vconfig_size > folio_size(vconfig_folio)) {
			pci_warn(pdev, "Ignoring preserved virtual config space of %u bytes\n",
				 ser->vconfig_size);
		} else {
			vconfig = kmemdup(folio_address(vconfig_folio),
					  ser->vconfig_size, GFP_KERNEL);
			if (!vconfig) {
				ret = -ENOMEM;
				goto out_put;
			}
		}
	}

	/*
	 * Hand the saved state and virtual config space to the first open of
	 * the device, which can only happen through the retrieved file.
	 */
	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	scoped_guard(mutex, &device->dev_set->lock) {
		if (!device->liveupdate_incoming || device->open_count) {
			pci_err(pdev, "Device is not preserved by the previous kernel\n");
			ret = -EINVAL;
			goto out_put;
		}

		kfree(vdev->liveupdate_saved_state);
		vdev->liveupdate_saved_state = saved_state;
		saved_state = NULL;
		kfree(vdev->liveupdate_vconfig);
		vdev->liveupdate_vconfig = vconfig;
		vconfig = NULL;
	}

	file = vfio_device_cdev_open_file(device);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto out_put;
	}

	args->file = file;
	ret = 0;
out_put:
	kfree(vconfig);
	kfree(saved_state);
	if (device)
		vfio_device_put_registration(device);
	pci_dev_put(pdev);
out_free:
	if (vconfig_folio)
		folio_put(vconfig_folio);
	if (saved_state_folio)
		folio_put(saved_state_folio);
	folio_put(folio);
	return ret;
}

static bool vfio_pci_liveupdate_can_finish(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device *vdev;

	/*
	 * The device can't finish until userspace takes it over, i.e. retrieves
	 * its file and binds it to an iommufd. The PCI core doesn't let a PF
	 * finish before its VFs.
	 */
	if (args->retrieve_status <= 0 ||
	    !vfio_device_cdev_file_is_bound(args->file))
		return false;

	vdev = vfio_pci_liveupdate_vdev(args->file);
	return pci_liveupdate_can_finish(vdev->pdev);
}

static void vfio_pci_liveupdate_finish(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device *vdev;

	/* Ensured by vfio_pci_liveupdate_can_finish() */
	if (WARN_ON_ONCE(args->retrieve_status <= 0))
		return;

	vdev = vfio_pci_liveupdate_vdev(args->file);

	scoped_guard(mutex, &vdev->vdev.dev_set->lock)
		vfio_pci_core_liveupdate_finish(vdev);

	pci_liveupdate_finish(vdev->pdev);
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
