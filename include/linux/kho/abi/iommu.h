/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2025, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#ifndef _LINUX_KHO_ABI_IOMMU_H
#define _LINUX_KHO_ABI_IOMMU_H

#include <linux/mutex_types.h>
#include <linux/compiler.h>
#include <linux/types.h>

/**
 * DOC: IOMMU File-Lifecycle Bound (FLB) Live Update ABI
 *
 * This header defines the ABI for preserving IOMMU state across kexec using
 * Live Update File-Lifecycle Bound (FLB) data.
 *
 * This interface is a contract. Any modification to any of the serialization
 * structs defined here constitutes a breaking change. Such changes require
 * incrementing the version number in the IOMMU_LUO_FLB_COMPATIBLE string.
 */

#define IOMMU_LUO_FLB_COMPATIBLE "iommu-v1"

enum iommu_lu_type {
	IOMMU_INVALID,
	IOMMU_INTEL,
};

struct iommu_obj_ser {
	u32 idx;
	u32 ref_count;
	u32 deleted:1;
	u32 incoming:1;
} __packed;

struct iommu_domain_ser {
	struct iommu_obj_ser obj;
	u64 top_table;
	u64 top_level;
	u64 attach_count;
	struct iommu_domain *restored_domain;
} __packed;

struct device_domain_iommu_ser {
	u32 did;
	u64 domain_phys;
	u64 iommu_phys;
};

struct device_ser {
	struct iommu_obj_ser obj;
	u64 token;
	u32 devid;
	u32 pci_domain;
	struct device_domain_iommu_ser domain_iommu_ser;
	enum iommu_lu_type type;
} __packed;

struct iommu_intel_ser {
	u64 phys_addr;
	u64 root_table;
} __packed;

struct iommu_ser {
	struct iommu_obj_ser obj;
	u64 token;
	enum iommu_lu_type type;
	union {
		struct iommu_intel_ser intel;
	};
};

struct iommu_objs_ser {
	u64 next_objs;
	u64 nr_objs;
};

struct iommus_ser {
	struct iommu_objs_ser objs;
	struct iommu_ser iommus[];
} __packed;

struct iommu_domains_ser {
	struct iommu_objs_ser objs;
	struct iommu_domain_ser iommu_domains[];
} __packed;

struct devices_ser {
	struct iommu_objs_ser objs;
	struct device_ser devices[];
} __packed;

#define MAX_IOMMU_SERS ((PAGE_SIZE - sizeof(struct iommus_ser)) / sizeof(struct iommu_ser))
#define MAX_IOMMU_DOMAIN_SERS ((PAGE_SIZE - sizeof(struct iommu_domains_ser)) / sizeof(struct iommu_domain_ser))
#define MAX_DEVICE_SERS ((PAGE_SIZE - sizeof(struct devices_ser)) / sizeof(struct device_ser))

struct iommu_lu_flb_ser {
	u64 iommus_phys;
	u64 nr_iommus;
	u64 iommu_domains_phys;
	u64 nr_domains;
	u64 devices_phys;
	u64 nr_devices;
} __packed;

struct iommu_lu_flb_obj {
	struct mutex lock;
	struct iommu_lu_flb_ser *ser;

	struct iommu_domains_ser *iommu_domains;
	struct iommus_ser *iommus;
	struct devices_ser *devices;
};

#endif /* _LINUX_KHO_ABI_IOMMU_H */
