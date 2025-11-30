/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2025, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#ifndef _LINUX_IOMMU_LU_H
#define _LINUX_IOMMU_LU_H

#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/liveupdate.h>
#include <linux/kho/abi/iommu.h>

#ifdef CONFIG_LIVEUPDATE
static inline void *dev_iommu_preserved_state(struct device *dev)
{
	struct device_ser *ser;

	ser = dev->iommu->device_ser;
	if (ser && !ser->obj.incoming)
		return ser;

	return NULL;
}

static inline void *dev_iommu_restored_state(struct device *dev)
{
	struct device_ser *ser;

	ser = dev->iommu->device_ser;
	if (ser && ser->obj.incoming)
		return ser;

	return NULL;
}

static inline void *iommu_domain_restored_state(struct iommu_domain *domain)
{
	struct iommu_domain_ser *ser;

	ser = domain->preserved_state;
	if (ser && ser->obj.incoming)
		return ser;

	return NULL;
}

static inline int dev_iommu_restore_did(struct device *dev, struct iommu_domain *domain)
{
	struct device_ser *ser = dev_iommu_restored_state(dev);

	if (ser && iommu_domain_restored_state(domain))
		return ser->domain_iommu_ser.did;

	return -1;
}
#else
static inline void *dev_iommu_preserved_state(struct device *dev)
{
	return NULL;
}

static inline void *dev_iommu_restored_state(struct device *dev)
{
	return NULL;
}

static inline int dev_iommu_restore_did(struct device *dev, struct iommu_domain *domain)
{
	return -1;
}

static inline void *iommu_domain_restored_state(struct iommu_domain *domain)
{
	return NULL;
}
#endif

struct iommu_domain *iommu_restore_domain(struct device *dev, struct device_ser *ser);
int iommu_for_each_preserved_device(int (*fn)(struct device_ser *ser, void *arg), void *arg);
struct device_ser *iommu_get_device_preserved_data(struct device *dev);
struct iommu_ser *iommu_get_preserved_data(u64 token, enum iommu_lu_type type);
int iommu_domain_preserve(struct iommu_domain *domain, struct iommu_domain_ser **ser);
int iommu_domain_unpreserve(struct iommu_domain *domain);
int iommu_preserve_device(struct iommu_domain *domain, struct device *dev);
int iommu_unpreserve_device(struct iommu_domain *domain, struct device *dev);
int iommu_liveupdate_register_flb(struct liveupdate_file_handler *handler);
int iommu_liveupdate_unregister_flb(struct liveupdate_file_handler *handler);

#endif /* _LINUX_IOMMU_LU_H */
