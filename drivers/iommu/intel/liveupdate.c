// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#define pr_fmt(fmt)    "iommu: liveupdate: " fmt

#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "iommu.h"

struct iommu_unit_ser {
	u64 phys_addr;
	u64 root_table;
};

struct device_ser {
	u64 bdf;
	u64 pasid_table;
	u64 pasid_order;
	u64 iommu_phys;
};

struct iommu_ser {
	u64 nr_iommus;
	u64 nr_devices;

	union {
		u64 iommu_units_phys;
		struct iommu_unit_ser *iommu_units;
	};

	union {
		u64 devices_phys;
		struct device_ser *devices;
	};
};

int intel_iommu_domain_liveupdate_preserve(struct iommu_domain *domain)
{
	pr_warn("Not implemented\n");
	return 0;
}

static bool is_device_domain_preserved(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	return atomic_read(&info->domain->domain.preserved) == 1;
}

static int preserve_device_state(struct pci_dev *dev, struct device_ser *ser)
{
	pr_warn("Not implemented\n");
	return 0;
}

static int unpreserve_iommu_context(struct intel_iommu *iommu, int end)
{
	struct context_entry *context;
	int i;

	if (end < 0)
		end = ROOT_ENTRY_NR;

	for (i = 0; i < end; i++) {
		context = iommu_context_addr(iommu, i, 0, 0);
		if (context)
			WARN_ON_ONCE(kho_unpreserve_folio(virt_to_folio(context)));

		if (!sm_supported(iommu))
			continue;

		context = iommu_context_addr(iommu, i, 0x80, 0);
		if (context)
			WARN_ON_ONCE(kho_unpreserve_folio(virt_to_folio(context)));
	}

	return 0;
}

static int preserve_iommu_context(struct intel_iommu *iommu)
{
	struct context_entry *context;
	int ret;
	int i;

	for (i = 0; i < ROOT_ENTRY_NR; i++) {
		context = iommu_context_addr(iommu, i, 0, 0);
		if (context) {
			ret = kho_preserve_folio(virt_to_folio(context));
			if (ret)
				goto error;
		}

		if (!sm_supported(iommu))
			continue;

		context = iommu_context_addr(iommu, i, 0x80, 0);
		if (context) {
			ret = kho_preserve_folio(virt_to_folio(context));
			if (ret)
				goto error_sm;
		}
	}

	return 0;

error_sm:
	context = iommu_context_addr(iommu, i, 0, 0);
	WARN_ON_ONCE(kho_unpreserve_folio(virt_to_folio(context)));
error:
	WARN_ON_ONCE(unpreserve_iommu_context(iommu, i));
	return ret;
}

static int preserve_iommu_state(struct intel_iommu *iommu,
				struct iommu_unit_ser *ser)
{
	int ret;

	spin_lock(&iommu->lock);
	ret = preserve_iommu_context(iommu);
	if (ret)
		goto error;

	ret = kho_preserve_folio(virt_to_folio(iommu->root_entry));
	if (ret) {
		unpreserve_iommu_context(iommu, -1);
		goto error;
	}

	ser->phys_addr = iommu->reg_phys;
	ser->root_table = __pa(iommu->root_entry);
	atomic_set(&iommu->preserved, 1);
error:
	spin_unlock(&iommu->lock);
	return ret;
}

static void unpreserve_state(struct iommu_ser *ser)
{
	pr_warn("Not implemented\n");
}

static int preserve_state(struct iommu_ser *ser)
{
	struct device_domain_info *info;
	struct pci_dev *pdev = NULL;
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu;
	int ret = 0;

	for_each_pci_dev(pdev) {
		if (!is_device_domain_preserved(&pdev->dev))
			continue;

		info = dev_iommu_priv_get(&pdev->dev);
		if (!info)
			return -EINVAL;

		if (ser->devices)
			ret = preserve_device_state(pdev, &ser->devices[ser->nr_devices]);

		if (ret)
			return ret;

		atomic_set(&info->iommu->preserved, 1);
		ser->nr_devices++;
	}

	for_each_iommu(iommu, drhd) {
		if (!atomic_read(&iommu->preserved))
			continue;

		atomic_set(&iommu->preserved, 0);
		if (ser->iommu_units)
			ret = preserve_iommu_state(iommu, &ser->iommu_units[ser->nr_iommus]);

		if (ret)
			return ret;

		ser->nr_iommus++;
	}

	return 0;
}

static struct iommu_ser *alloc_preserve_state_mem(void)
{
	struct iommu_ser *ser_ptr;
	struct iommu_ser ser;
	struct folio *folio;
	size_t sz;
	int ret;

	memset(&ser, 0, sizeof(ser));
	ret = preserve_state(&ser);
	if (ret)
		goto error;

	sz = sizeof(struct iommu_ser) +
			(ser.nr_iommus * sizeof(struct iommu_unit_ser)) +
			(ser.nr_devices * sizeof(struct device_ser));

	folio = folio_alloc(GFP_KERNEL, get_order(sz));
	if (!folio)
		return ERR_PTR(-ENOMEM);

	ret = kho_preserve_folio(folio);
	if (ret)
		goto error_preserve;

	ser_ptr = folio_address(folio);
	memset(ser_ptr, 0, sz);
	ser_ptr->iommu_units = (void *)(ser_ptr + 1);
	ser_ptr->devices = (void *)(ser_ptr->iommu_units + ser.nr_iommus);

	return ser_ptr;

error_preserve:
	folio_put(folio);
error:
	return ERR_PTR(ret);
}

static int intel_liveupdate_prepare(struct liveupdate_subsystem *handle, u64 *data)
{
	struct iommu_ser *ser;
	int ret;

	guard_liveupdate_state_write();
	ser = alloc_preserve_state_mem();
	if (IS_ERR(ser))
		return PTR_ERR(ser);

	ret = preserve_state(ser);
	if (ret)
		unpreserve_state(ser);

	if (!ret)
		*data = __pa(ser);

	return ret;
}

static void intel_liveupdate_cancel(struct liveupdate_subsystem *handle, u64 data)
{
	pr_warn("Not implemented\n");
}

static void intel_liveupdate_finish(struct liveupdate_subsystem *handle, u64 data)
{
	pr_warn("Not implemented\n");
}

static struct liveupdate_subsystem_ops intel_liveupdate_subsystem_ops = {
	.prepare = intel_liveupdate_prepare,
	.finish = intel_liveupdate_finish,
	.cancel = intel_liveupdate_cancel,
};

static struct liveupdate_subsystem intel_liveupdate_subsystem = {
	.name = "intel-iommu",
	.ops = &intel_liveupdate_subsystem_ops,
};

static int __init intel_liveupdate_init(void)
{
	WARN_ON_ONCE(liveupdate_register_subsystem(&intel_liveupdate_subsystem));
	return 0;
}

late_initcall(intel_liveupdate_init);
