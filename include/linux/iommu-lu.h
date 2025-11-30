/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2025, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#ifndef _LINUX_IOMMU_LU_H
#define _LINUX_IOMMU_LU_H

#include <linux/liveupdate.h>
#include <linux/kho/abi/iommu.h>

int iommu_liveupdate_register_flb(struct liveupdate_file_handler *handler);
int iommu_liveupdate_unregister_flb(struct liveupdate_file_handler *handler);

#endif /* _LINUX_IOMMU_LU_H */
