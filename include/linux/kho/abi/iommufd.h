/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2025, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#ifndef _LINUX_KHO_ABI_IOMMUFD_H
#define _LINUX_KHO_ABI_IOMMUFD_H

#include <linux/mutex_types.h>
#include <linux/compiler.h>
#include <linux/types.h>

/**
 * DOC: IOMMUFD Live Update ABI
 *
 * This header defines the ABI for preserving the state of an IOMMUFD file
 * across a kexec reboot using LUO.
 *
 * This interface is a contract. Any modification to any of the serialization
 * structs defined here constitutes a breaking change. Such changes require
 * incrementing the version number in the IOMMUFD_LUO_COMPATIBLE string.
 */

#define IOMMUFD_LUO_COMPATIBLE "iommufd-v1"

struct iommufd_lu {
};

#endif /* _LINUX_KHO_ABI_IOMMUFD_H */
