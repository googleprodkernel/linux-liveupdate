// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Live Update test driver.
 *
 * pci-lu-test is a test-only driver that exercises the PCI core's Live Update
 * support without any device-specific driver (e.g. vfio-pci). It binds to any
 * PCI device, but only via driver_override, and does nothing with the device
 * beyond keeping it enabled.
 *
 * Userspace preserves a device by opening /dev/pci_lu_test, associating the
 * file with a device bound to pci-lu-test (PCI_LU_TEST_IOCTL_SET_DEVICE), and
 * preserving the file in a LUO session (LIVEUPDATE_SESSION_PRESERVE_FD). The
 * file handler then preserves the device with pci_liveupdate_preserve().
 *
 * After kexec, userspace binds the device to pci-lu-test again and retrieves
 * the file (LIVEUPDATE_SESSION_RETRIEVE_FD), which returns a new file
 * associated with the device. Finishing the session finishes the device's
 * participation in Live Update with pci_liveupdate_finish().
 *
 * pci-lu-test also implements sriov_configure, so that it can be bound to an
 * SR-IOV PF and enable VFs, which can then be bound to pci-lu-test and
 * preserved too. It supports Live Update as a PF driver (struct
 * pci_liveupdate_pf_ops), so the PF does not need to be preserved explicitly:
 * the PCI core preserves it automatically along with its VFs, and the driver
 * checks the data it passes to the next kernel for the PF.
 *
 * The device's state (configuration space, BARs, etc.) is checked by
 * userspace through sysfs, see luo_pci.c.
 *
 * pci-lu-test also passes the device's saved state (pci_store_saved_state())
 * to the next kernel, serialized with pci_saved_state_serialize(), the way a
 * driver like vfio-pci would, and lets userspace check what the next kernel
 * gets back from pci_saved_state_deserialize().
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "../pci_lu_test_uapi.h"

#define PCI_LU_TEST_FH_COMPATIBLE	"pci-lu-test-v2"
#define PCI_LU_TEST_PF_MAGIC		0x5043504600000000ULL

/*
 * Serialized state of a preserved file, preserved with kho_alloc_preserve().
 * @saved_state is a struct pci_saved_state_ser of @saved_state_size bytes.
 */
struct pci_lu_test_ser {
	u32 domain;
	u16 dev_id;
	u16 reserved;
	u32 saved_state_size;
	u8 saved_state[];
} __packed;

/*
 * A file that represents (at most) one PCI device.
 * @saved_ser: The device's saved state, serialized, of @saved_ser_size bytes.
 *             For a preserved file, the state passed to the next kernel. For
 *             a retrieved file, @saved_state serialized again.
 * @saved_state: For a retrieved file, the saved state passed by the previous
 *               kernel.
 */
struct pci_lu_test_file {
	struct pci_dev *dev;
	bool preserved;
	void *saved_ser;
	size_t saved_ser_size;
	struct pci_saved_state *saved_state;
};

/* Driver data of a device bound to pci-lu-test. */
struct pci_lu_test_priv {
	struct pci_lu_test_file *file;
};

/*
 * Protects the association between files and devices, and the preservation
 * status of the files.
 */
static DEFINE_MUTEX(pci_lu_test_lock);

static struct pci_driver pci_lu_test_driver;
static const struct file_operations pci_lu_test_fops;

static bool pci_lu_test_bound(struct pci_dev *dev)
{
	return READ_ONCE(dev->driver) == &pci_lu_test_driver;
}

/* Associate @file with @dev. Takes ownership of the reference on @dev. */
static int pci_lu_test_attach(struct pci_lu_test_file *file,
			      struct pci_dev *dev)
{
	struct pci_lu_test_priv *priv;

	lockdep_assert_held(&pci_lu_test_lock);

	if (!pci_lu_test_bound(dev))
		return -EINVAL;

	priv = pci_get_drvdata(dev);
	if (priv->file)
		return -EBUSY;

	priv->file = file;
	file->dev = dev;
	return 0;
}

static void pci_lu_test_detach(struct pci_lu_test_file *file)
{
	struct pci_lu_test_priv *priv;
	struct pci_dev *dev = file->dev;

	lockdep_assert_held(&pci_lu_test_lock);

	if (!dev)
		return;

	if (pci_lu_test_bound(dev)) {
		priv = pci_get_drvdata(dev);
		if (priv && priv->file == file)
			priv->file = NULL;
	}

	file->dev = NULL;
	pci_dev_put(dev);
}

static long pci_lu_test_set_device(struct pci_lu_test_file *file,
				   void __user *uarg)
{
	struct pci_lu_test_set_device arg;
	struct pci_dev *dev;
	int ret;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	if (arg.argsz < sizeof(arg) || arg.reserved)
		return -EINVAL;

	dev = pci_get_domain_bus_and_slot(arg.domain, arg.bus, arg.devfn);
	if (!dev)
		return -ENODEV;

	guard(mutex)(&pci_lu_test_lock);

	if (file->preserved) {
		ret = -EBUSY;
		goto err_put;
	}

	pci_lu_test_detach(file);

	ret = pci_lu_test_attach(file, dev);
	if (ret)
		goto err_put;

	return 0;

err_put:
	pci_dev_put(dev);
	return ret;
}

static long pci_lu_test_get_saved_state(struct pci_lu_test_file *file,
					void __user *uarg)
{
	struct pci_lu_test_saved_state arg;
	size_t size;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	if (arg.argsz < sizeof(arg))
		return -EINVAL;

	guard(mutex)(&pci_lu_test_lock);

	if (!file->saved_ser)
		return -ENODATA;

	size = arg.size;
	arg.size = file->saved_ser_size;
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;

	if (size < file->saved_ser_size)
		return -ENOSPC;

	if (copy_to_user(u64_to_user_ptr(arg.buf), file->saved_ser,
			 file->saved_ser_size))
		return -EFAULT;

	return 0;
}

static long pci_lu_test_restore_saved_state(struct pci_lu_test_file *file)
{
	int ret;

	guard(mutex)(&pci_lu_test_lock);

	if (!file->dev || !file->saved_state)
		return -ENODATA;

	if (pci_liveupdate_is_preserved(file->dev))
		return -EBUSY;

	ret = pci_load_saved_state(file->dev, file->saved_state);
	if (ret)
		return ret;

	pci_restore_state(file->dev);
	pci_info(file->dev, "Restored saved state from the previous kernel\n");
	return 0;
}

static long pci_lu_test_ioctl(struct file *filp, unsigned int cmd,
			      unsigned long arg)
{
	struct pci_lu_test_file *file = filp->private_data;

	switch (cmd) {
	case PCI_LU_TEST_IOCTL_SET_DEVICE:
		return pci_lu_test_set_device(file, (void __user *)arg);
	case PCI_LU_TEST_IOCTL_GET_SAVED_STATE:
		return pci_lu_test_get_saved_state(file, (void __user *)arg);
	case PCI_LU_TEST_IOCTL_RESTORE_SAVED_STATE:
		return pci_lu_test_restore_saved_state(file);
	default:
		return -ENOTTY;
	}
}

static struct pci_lu_test_file *pci_lu_test_file_alloc(void)
{
	return kzalloc(sizeof(struct pci_lu_test_file), GFP_KERNEL);
}

static void pci_lu_test_file_free(struct pci_lu_test_file *file)
{
	kfree(file->saved_state);
	kfree(file->saved_ser);
	kfree(file);
}

static int pci_lu_test_open(struct inode *inode, struct file *filp)
{
	struct pci_lu_test_file *file;

	file = pci_lu_test_file_alloc();
	if (!file)
		return -ENOMEM;

	filp->private_data = file;
	return 0;
}

static int pci_lu_test_release(struct inode *inode, struct file *filp)
{
	struct pci_lu_test_file *file = filp->private_data;

	scoped_guard(mutex, &pci_lu_test_lock) {
		/* LUO holds a reference on preserved files. */
		WARN_ON(file->preserved);
		pci_lu_test_detach(file);
	}

	pci_lu_test_file_free(file);
	return 0;
}

static const struct file_operations pci_lu_test_fops = {
	.owner = THIS_MODULE,
	.open = pci_lu_test_open,
	.release = pci_lu_test_release,
	.unlocked_ioctl = pci_lu_test_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static struct miscdevice pci_lu_test_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = PCI_LU_TEST_DEV_NAME,
	.fops = &pci_lu_test_fops,
};

/* LUO file handler */

static bool pci_lu_test_can_preserve(struct liveupdate_file_handler *handler,
				     struct file *filp)
{
	return filp->f_op == &pci_lu_test_fops;
}

static int pci_lu_test_preserve(struct liveupdate_file_op_args *args)
{
	struct pci_lu_test_file *file = args->file->private_data;
	struct pci_saved_state *state;
	struct pci_lu_test_ser *ser;
	struct pci_dev *dev;
	void *saved_ser;
	size_t size;
	int ret;

	guard(mutex)(&pci_lu_test_lock);

	dev = file->dev;
	if (!dev)
		return -ENODEV;

	if (!pci_lu_test_bound(dev))
		return -EINVAL;

	/* Pass the device's current state, like vfio-pci passes its initial state. */
	pci_save_state(dev);
	state = pci_store_saved_state(dev);
	if (!state)
		return -ENOMEM;

	size = pci_saved_state_ser_size(state);
	ser = kho_alloc_preserve(struct_size(ser, saved_state, size));
	if (IS_ERR(ser)) {
		ret = PTR_ERR(ser);
		goto out_free_state;
	}

	ser->domain = pci_domain_nr(dev->bus);
	ser->dev_id = pci_dev_id(dev);
	ser->saved_state_size = size;

	ret = pci_saved_state_serialize(state, (void *)ser->saved_state, size);
	if (ret)
		goto err_free_ser;

	saved_ser = kmemdup(ser->saved_state, size, GFP_KERNEL);
	if (!saved_ser) {
		ret = -ENOMEM;
		goto err_free_ser;
	}

	ret = pci_liveupdate_preserve(dev);
	if (ret) {
		pci_err(dev, "Failed to preserve device (%d)\n", ret);
		kfree(saved_ser);
		goto err_free_ser;
	}

	pci_info(dev, "Preserved device (%zu bytes of saved state)\n", size);
	file->preserved = true;
	file->saved_ser = saved_ser;
	file->saved_ser_size = size;
	args->serialized_data = virt_to_phys(ser);
	goto out_free_state;

err_free_ser:
	kho_unpreserve_free(ser);
out_free_state:
	kfree(state);
	return ret;
}

static void pci_lu_test_unpreserve(struct liveupdate_file_op_args *args)
{
	struct pci_lu_test_file *file = args->file->private_data;

	guard(mutex)(&pci_lu_test_lock);

	pci_info(file->dev, "Unpreserved device\n");
	pci_liveupdate_unpreserve(file->dev);
	kho_unpreserve_free(phys_to_virt(args->serialized_data));
	file->preserved = false;
	kfree(file->saved_ser);
	file->saved_ser = NULL;
	file->saved_ser_size = 0;
}

static int pci_lu_test_freeze(struct liveupdate_file_op_args *args)
{
	struct pci_lu_test_file *file = args->file->private_data;

	return pci_liveupdate_freeze(file->dev);
}

static void pci_lu_test_unfreeze(struct liveupdate_file_op_args *args)
{
	struct pci_lu_test_file *file = args->file->private_data;

	pci_liveupdate_unfreeze(file->dev);
}

/* Look up the device of a file that has not been retrieved. */
static struct pci_dev *pci_lu_test_ser_dev(struct liveupdate_file_op_args *args)
{
	const struct pci_lu_test_ser *ser = phys_to_virt(args->serialized_data);

	return pci_get_domain_bus_and_slot(ser->domain, PCI_BUS_NUM(ser->dev_id),
					   ser->dev_id & 0xff);
}

/* Return a reference to the device of a file that has been preserved. */
static struct pci_dev *pci_lu_test_args_dev(struct liveupdate_file_op_args *args)
{
	struct pci_lu_test_file *file;

	if (args->retrieve_status <= 0)
		return pci_lu_test_ser_dev(args);

	file = args->file->private_data;
	return pci_dev_get(file->dev);
}

/*
 * Check that pci_saved_state_deserialize() refuses a malformed @ser, and drops
 * a register that is not saved for @dev rather than refusing @ser.
 */
static int pci_lu_test_check_deserialize(struct pci_dev *dev,
					 const struct pci_saved_state_ser *ser,
					 size_t size)
{
	unsigned int dword, extra, i = 0, j = 0;
	struct pci_saved_state_ser *bad;
	struct pci_saved_state *state;
	size_t bad_size;
	int ret = 0;

	bad_size = size + sizeof(u32);
	bad = kzalloc(bad_size, GFP_KERNEL);
	if (!bad)
		return -ENOMEM;

	memcpy(bad, ser, size);
	bad->flags = 1;
	state = pci_saved_state_deserialize(dev, bad, size);
	if (!IS_ERR(state) || PTR_ERR(state) != -EINVAL) {
		pci_err(dev, "Saved state with unknown flags not refused\n");
		ret = -EINVAL;
		goto out;
	}

	/* Add the first register after the header that is not saved. */
	for (extra = PCI_STD_HEADER_SIZEOF / 4; extra < PCI_CFG_SPACE_EXP_SIZE / 4; extra++) {
		if (!(ser->dword_map[extra / 64] & BIT_ULL(extra % 64)))
			break;
	}

	/* Every register is saved, so there is none to add. */
	if (extra == PCI_CFG_SPACE_EXP_SIZE / 4)
		goto out;

	memcpy(bad, ser, offsetof(struct pci_saved_state_ser, dword_val));
	bad->size = bad_size;
	bad->nr_dwords++;
	bad->dword_map[extra / 64] |= BIT_ULL(extra % 64);
	for (dword = 0; dword < PCI_CFG_SPACE_EXP_SIZE / 4; dword++) {
		if (dword == extra)
			bad->dword_val[j++] = 0xdeadbeef;
		else if (ser->dword_map[dword / 64] & BIT_ULL(dword % 64))
			bad->dword_val[j++] = ser->dword_val[i++];
	}

	state = pci_saved_state_deserialize(dev, bad, bad_size);
	if (IS_ERR(state)) {
		pci_err(dev, "Saved state with an extra register refused (%ld)\n",
			PTR_ERR(state));
		ret = PTR_ERR(state);
		goto out;
	}

	memset(bad, 0, bad_size);
	ret = pci_saved_state_serialize(state, bad, size);
	if (!ret && memcmp(bad, ser, size)) {
		pci_err(dev, "Extra register at %#x not dropped\n", extra * 4);
		ret = -EINVAL;
	}

	kfree(state);
out:
	kfree(bad);
	return ret;
}

/*
 * Get the saved state passed by the previous kernel for @dev, and serialize it
 * again for PCI_LU_TEST_IOCTL_GET_SAVED_STATE.
 */
static int pci_lu_test_retrieve_saved_state(struct pci_lu_test_file *file,
					    struct pci_dev *dev,
					    const struct pci_lu_test_ser *ser)
{
	struct pci_saved_state *state;
	void *saved_ser;
	size_t size;
	int ret;

	ret = pci_lu_test_check_deserialize(dev, (const void *)ser->saved_state,
					    ser->saved_state_size);
	if (ret)
		return ret;

	state = pci_saved_state_deserialize(dev, (const void *)ser->saved_state,
					    ser->saved_state_size);
	if (IS_ERR(state))
		return PTR_ERR(state);

	size = pci_saved_state_ser_size(state);
	saved_ser = kmalloc(size, GFP_KERNEL);
	if (!saved_ser) {
		kfree(state);
		return -ENOMEM;
	}

	ret = pci_saved_state_serialize(state, saved_ser, size);
	if (ret) {
		kfree(saved_ser);
		kfree(state);
		return ret;
	}

	file->saved_state = state;
	file->saved_ser = saved_ser;
	file->saved_ser_size = size;
	return 0;
}

static int pci_lu_test_retrieve(struct liveupdate_file_op_args *args)
{
	const struct pci_lu_test_ser *ser = phys_to_virt(args->serialized_data);
	struct pci_lu_test_file *file;
	struct file *filp;
	struct pci_dev *dev;
	int ret;

	dev = pci_lu_test_ser_dev(args);
	if (!dev)
		return -ENODEV;

	if (!pci_liveupdate_is_incoming(dev)) {
		pci_err(dev, "Device is not incoming preserved\n");
		ret = -EINVAL;
		goto err_put;
	}

	file = pci_lu_test_file_alloc();
	if (!file) {
		ret = -ENOMEM;
		goto err_put;
	}

	ret = pci_lu_test_retrieve_saved_state(file, dev, ser);
	if (ret) {
		pci_err(dev, "Failed to get saved state (%d)\n", ret);
		goto err_free;
	}

	scoped_guard(mutex, &pci_lu_test_lock) {
		ret = pci_lu_test_attach(file, dev);
		if (ret) {
			pci_err(dev, "Device not bound to %s (%d)\n",
				pci_lu_test_driver.name, ret);
			goto err_free;
		}
	}

	filp = anon_inode_getfile("[pci_lu_test]", &pci_lu_test_fops, file,
				  O_RDWR);
	if (IS_ERR(filp)) {
		ret = PTR_ERR(filp);
		goto err_detach;
	}

	/*
	 * The serialized state is not needed anymore, the device can be found
	 * through the retrieved file from now on.
	 */
	kho_restore_free(phys_to_virt(args->serialized_data));

	pci_info(dev, "Retrieved device (%zu bytes of saved state)\n",
		 file->saved_ser_size);
	args->file = filp;
	return 0;

err_detach:
	scoped_guard(mutex, &pci_lu_test_lock)
		pci_lu_test_detach(file);
	pci_lu_test_file_free(file);
	return ret;
err_free:
	pci_lu_test_file_free(file);
err_put:
	pci_dev_put(dev);
	return ret;
}

static bool pci_lu_test_can_finish(struct liveupdate_file_op_args *args)
{
	struct pci_dev *dev;
	bool ret;

	dev = pci_lu_test_args_dev(args);
	if (!dev)
		return true;

	ret = pci_liveupdate_can_finish(dev);
	pci_dev_put(dev);
	return ret;
}

static void pci_lu_test_finish(struct liveupdate_file_op_args *args)
{
	struct pci_dev *dev;

	dev = pci_lu_test_args_dev(args);
	if (dev) {
		pci_info(dev, "Finished device\n");
		pci_liveupdate_finish(dev);
		pci_dev_put(dev);
	} else {
		pr_err("Preserved device not found\n");
	}

	/* The serialized state is freed by a successful retrieve. */
	if (args->retrieve_status <= 0)
		kho_restore_free(phys_to_virt(args->serialized_data));
}

static const struct liveupdate_file_ops pci_lu_test_file_ops = {
	.can_preserve = pci_lu_test_can_preserve,
	.preserve = pci_lu_test_preserve,
	.unpreserve = pci_lu_test_unpreserve,
	.freeze = pci_lu_test_freeze,
	.unfreeze = pci_lu_test_unfreeze,
	.retrieve = pci_lu_test_retrieve,
	.can_finish = pci_lu_test_can_finish,
	.finish = pci_lu_test_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler pci_lu_test_fh = {
	.ops = &pci_lu_test_file_ops,
	.compatible = PCI_LU_TEST_FH_COMPATIBLE,
};

/* PCI driver */

static const struct pci_device_id pci_lu_test_ids[] = {
	/* Only bind to devices via driver_override. */
	{ PCI_DEVICE_DRIVER_OVERRIDE(PCI_ANY_ID, PCI_ANY_ID, 1) },
	{}
};
MODULE_DEVICE_TABLE(pci, pci_lu_test_ids);

/* PF driver Live Update support */

static u64 pci_lu_test_pf_data(struct pci_dev *pf)
{
	return PCI_LU_TEST_PF_MAGIC | (pci_domain_nr(pf->bus) << 16) |
	       pci_dev_id(pf);
}

static int pci_lu_test_pf_preserve(struct pci_dev *pf, u64 *data)
{
	*data = pci_lu_test_pf_data(pf);
	pci_info(pf, "Preserved PF (data %#llx)\n", *data);
	return 0;
}

static void pci_lu_test_pf_unpreserve(struct pci_dev *pf, u64 data)
{
	WARN_ON(data != pci_lu_test_pf_data(pf));
	pci_info(pf, "Unpreserved PF\n");
}

static void pci_lu_test_pf_finish(struct pci_dev *pf, u64 data)
{
	WARN_ON(data != pci_lu_test_pf_data(pf));
	pci_info(pf, "Finished PF\n");
}

static const struct pci_liveupdate_pf_ops pci_lu_test_pf_ops = {
	.preserve = pci_lu_test_pf_preserve,
	.unpreserve = pci_lu_test_pf_unpreserve,
	.finish = pci_lu_test_pf_finish,
};

/* Check the data passed by the previous kernel for an automatically preserved PF. */
static int pci_lu_test_probe_pf(struct pci_dev *dev)
{
	u64 data;
	int ret;

	if (!dev->is_physfn || !pci_liveupdate_is_incoming(dev))
		return 0;

	ret = pci_liveupdate_get_driver_data(dev, &data);
	if (ret == -EINVAL)
		return 0;	/* PF was preserved explicitly */
	if (ret)
		return ret;

	if (data != pci_lu_test_pf_data(dev)) {
		pci_err(dev, "Unexpected PF data %#llx\n", data);
		return -EINVAL;
	}

	pci_info(dev, "Taking over PF preserved automatically (data %#llx)\n", data);
	return 0;
}

static int pci_lu_test_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct pci_lu_test_priv *priv;
	int ret;

	ret = pci_lu_test_probe_pf(dev);
	if (ret)
		return ret;

	priv = devm_kzalloc(&dev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	pci_set_drvdata(dev, priv);

	/*
	 * Keep the device enabled (memory decoding and bus mastering) so that
	 * tests can check that Live Update leaves enabled devices alone.
	 */
	ret = pci_enable_device(dev);
	if (ret)
		return ret;

	pci_set_master(dev);

	pci_info(dev, "Bound to %s%s\n", pci_lu_test_driver.name,
		 pci_liveupdate_is_incoming(dev) ? " (incoming preserved)" : "");
	return 0;
}

static void pci_lu_test_remove(struct pci_dev *dev)
{
	struct pci_lu_test_priv *priv = pci_get_drvdata(dev);

	/*
	 * Drivers must not release preserved devices (see "Driver Binding" in
	 * Documentation/PCI/liveupdate.rst). Tests are not expected to unbind
	 * preserved devices, so warn about it.
	 */
	WARN_ON(pci_liveupdate_is_preserved(dev));

	/*
	 * A file associated with the device keeps its reference on the device
	 * until the file is released, so that it can still be unpreserved.
	 */
	scoped_guard(mutex, &pci_lu_test_lock)
		priv->file = NULL;

	if (!pci_liveupdate_is_preserved(dev))
		pci_disable_device(dev);
}

static struct pci_driver pci_lu_test_driver = {
	.name = "pci-lu-test",
	.id_table = pci_lu_test_ids,
	.probe = pci_lu_test_probe,
	.remove = pci_lu_test_remove,
	.sriov_configure = pci_sriov_configure_simple,
	.liveupdate = &pci_lu_test_pf_ops,
};

static int __init pci_lu_test_init(void)
{
	int ret;

	ret = liveupdate_register_file_handler(&pci_lu_test_fh);
	if (ret)
		return ret;

	ret = pci_liveupdate_register_flb(&pci_lu_test_fh);
	if (ret)
		goto err_unregister_fh;

	ret = pci_register_driver(&pci_lu_test_driver);
	if (ret)
		goto err_unregister_flb;

	ret = misc_register(&pci_lu_test_misc);
	if (ret)
		goto err_unregister_driver;

	return 0;

err_unregister_driver:
	pci_unregister_driver(&pci_lu_test_driver);
err_unregister_flb:
	pci_liveupdate_unregister_flb(&pci_lu_test_fh);
err_unregister_fh:
	liveupdate_unregister_file_handler(&pci_lu_test_fh);
	return ret;
}
module_init(pci_lu_test_init);

static void __exit pci_lu_test_exit(void)
{
	misc_deregister(&pci_lu_test_misc);
	pci_unregister_driver(&pci_lu_test_driver);
	pci_liveupdate_unregister_flb(&pci_lu_test_fh);
	liveupdate_unregister_file_handler(&pci_lu_test_fh);
}
module_exit(pci_lu_test_exit);

MODULE_DESCRIPTION("PCI Live Update test driver");
MODULE_LICENSE("GPL");
