// SPDX-License-Identifier: GPL-2.0
/* pci-pf-stub - simple stub driver for PCI SR-IOV PF device
 *
 * This driver is meant to act as a "whitelist" for devices that provide
 * SR-IOV functionality while at the same time not actually needing a
 * driver of their own.
 */

#include "asm-generic/int-ll64.h"
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kexec_handover.h>
#include "pci.h"

#define PCI_PF_STUB_KHO_NODE_NAME "pci_pf_stub_driver"
#define PCI_PF_STUB_KHO_PROP_NAME "number_of_vfs"

/*
 * pci_pf_stub_whitelist - White list of devices to bind pci-pf-stub onto
 *
 * This table provides the list of IDs this driver is supposed to bind
 * onto.  You could think of this as a list of "quirked" devices where we
 * are adding support for SR-IOV here since there are no other drivers
 * that they would be running under.
 */
static const struct pci_device_id pci_pf_stub_whitelist[] = {
	{ PCI_VDEVICE(AMAZON, 0x0053) },
	{ PCI_VDEVICE(REDHAT_QUMRANET, 0x1041) }, /* Red Hat, Inc. Virtio 1.0 network device for testing */
	/* required last entry */
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, pci_pf_stub_whitelist);

static struct pci_driver pf_stub_driver;

static struct kho_node pci_pf_stub_kho_node = KHO_NODE_INIT;

static u32 num_vfs = 0;

static int pci_pf_stub_probe(struct pci_dev *dev,
			     const struct pci_device_id *id)
{
	if (dev->is_virtfn) {
		return -ENXIO;
	}
	pci_info(dev, "claimed by pci-pf-stub\n");

	struct kho_in_node pci_pf_stub_kho_in_node;
	const u64 *kho_num_vfs = NULL;
	int len = 0;

	if (kho_get_node(NULL, PCI_PF_STUB_KHO_NODE_NAME,
			 &pci_pf_stub_kho_in_node) == 0) {
		kho_num_vfs = kho_get_prop(&pci_pf_stub_kho_in_node,
				       PCI_PF_STUB_KHO_PROP_NAME,
				       &len);
		pci_info(
			dev,
			"KHO found. len %d. data 0x%llx\n",
			len, *kho_num_vfs);

		pf_stub_driver.sriov_configure(dev, *kho_num_vfs);
	}

	return 0;
}

#ifdef CONFIG_LIVEUPDATE
static int pci_pf_stub_liveupdate_prepare(struct device *dev)
{
	int ret = 0;

	pr_err("%s\n", __func__);
	ret = kho_add_node(NULL, PCI_PF_STUB_KHO_NODE_NAME,
			   &pci_pf_stub_kho_node);

	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct pci_sriov *iov = pci_dev->sriov;

	num_vfs = iov->num_VFs;

	pr_err("JXX: save num vfs %u\n" , num_vfs);

	if (!ret) {
		ret = kho_add_prop(&pci_pf_stub_kho_node,
				   PCI_PF_STUB_KHO_PROP_NAME, &num_vfs,
				   sizeof(num_vfs));
	}

	if (ret) {
		pr_err("Prepare KHO node %s failed\n",
		       PCI_PF_STUB_KHO_NODE_NAME);
		return ret;
	}

	return 0;
}

static int pci_pf_stub_liveupdate_reboot(struct device *dev)
{
	pr_err("%s\n", __func__);
	return 0;
}

static void pci_pf_stub_liveupdate_finish(struct device *dev)
{
	pr_err("%s\n", __func__);
}


static void pci_pf_stub_liveupdate_cancel(struct device *dev)
{
	pr_err("%s\n", __func__);
}

static struct dev_liveupdate_cbs liveupdate_cbs = {
	.prepare = pci_pf_stub_liveupdate_prepare,
	.reboot = pci_pf_stub_liveupdate_reboot,
	.finish = pci_pf_stub_liveupdate_finish,
	.cancel = pci_pf_stub_liveupdate_cancel,
};
#endif /* CONFIG_LIVEUPDATE */


static struct pci_driver pf_stub_driver = {
	.name = "pci-pf-stub",
	.id_table = pci_pf_stub_whitelist,
	.probe = pci_pf_stub_probe,
	.sriov_configure = pci_sriov_configure_simple,
#ifdef CONFIG_LIVEUPDATE
	.driver.liveupdate = &liveupdate_cbs,
#endif
};
module_pci_driver(pf_stub_driver);

MODULE_DESCRIPTION("SR-IOV PF stub driver with no functionality");
MODULE_LICENSE("GPL");
