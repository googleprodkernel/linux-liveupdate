// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2025, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#define pr_fmt(fmt)    "iommu: liveupdate: " fmt

#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/iommu-lu.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "iommu.h"
#include "../iommu-pages.h"

static void unpreserve_iommu_context(struct intel_iommu *iommu, int end)
{
	struct context_entry *context;
	int i;

	if (end < 0)
		end = ROOT_ENTRY_NR;

	for (i = 0; i < end; i++) {
		context = iommu_context_addr(iommu, i, 0, 0);
		if (context)
			iommu_unpreserve_page(context);

		if (!sm_supported(iommu))
			continue;

		context = iommu_context_addr(iommu, i, 0x80, 0);
		if (context)
			iommu_unpreserve_page(context);
	}
}

static int preserve_iommu_context(struct intel_iommu *iommu)
{
	struct context_entry *context;
	int ret;
	int i;

	for (i = 0; i < ROOT_ENTRY_NR; i++) {
		context = iommu_context_addr(iommu, i, 0, 0);
		if (context) {
			ret = iommu_preserve_page(context);
			if (ret)
				goto error;
		}

		if (!sm_supported(iommu))
			continue;

		context = iommu_context_addr(iommu, i, 0x80, 0);
		if (context) {
			ret = iommu_preserve_page(context);
			if (ret)
				goto error_sm;
		}
	}

	return 0;

error_sm:
	context = iommu_context_addr(iommu, i, 0, 0);
	iommu_unpreserve_page(context);
error:
	unpreserve_iommu_context(iommu, i);
	return ret;
}

static void restore_iommu_context(struct intel_iommu *iommu)
{
	struct context_entry *context;
	int i;

	for (i = 0; i < ROOT_ENTRY_NR; i++) {
		context = iommu_context_addr(iommu, i, 0, 0);
		if (context)
			BUG_ON(!kho_restore_folio(virt_to_phys(context)));

		if (!sm_supported(iommu))
			continue;

		context = iommu_context_addr(iommu, i, 0x80, 0);
		if (context)
			BUG_ON(!kho_restore_folio(virt_to_phys(context)));
	}
}

void intel_iommu_liveupdate_restore_root_table(struct intel_iommu *iommu,
					       struct iommu_ser *iommu_ser)
{
	BUG_ON(!kho_restore_folio(iommu_ser->intel.root_table));
	iommu->root_entry = __va(iommu_ser->intel.root_table);

	restore_iommu_context(iommu);
	pr_info("Restored IOMMU[0x%llx] Root Table at: 0x%llx\n",
		iommu->reg_phys, iommu_ser->intel.root_table);
}

int intel_iommu_preserve_device(struct device *dev, struct device_ser *device_ser)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	if (!dev_is_pci(dev))
		return -EOPNOTSUPP;

	if (!info)
		return -EINVAL;

	device_ser->domain_iommu_ser.did = domain_id_iommu(info->domain, info->iommu);

	/* TODO: Add support preservation of PASIDs. */
	return 0;
}

void intel_iommu_unpreserve_device(struct device *dev, struct device_ser *device_ser)
{
}

int intel_iommu_preserve(struct iommu_device *iommu_dev, struct iommu_ser *ser)
{
	struct intel_iommu *iommu;
	int ret;

	iommu = container_of(iommu_dev, struct intel_iommu, iommu);

	spin_lock(&iommu->lock);
	ret = preserve_iommu_context(iommu);
	if (ret)
		goto err;

	ret = iommu_preserve_page(iommu->root_entry);
	if (ret) {
		unpreserve_iommu_context(iommu, -1);
		goto err;
	}

	ser->intel.phys_addr = iommu->reg_phys;
	ser->intel.root_table = __pa(iommu->root_entry);
	ser->type = IOMMU_INTEL;
	ser->token = ser->intel.phys_addr;
	spin_unlock(&iommu->lock);

	return 0;
err:
	spin_unlock(&iommu->lock);
	return ret;
}

void intel_iommu_unpreserve(struct iommu_device *iommu, struct iommu_ser *iommu_ser)
{
}
