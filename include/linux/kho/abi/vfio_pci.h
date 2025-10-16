/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Vipin Sharma <vipinsh@google.com>
 * David Matlack <dmatlack@google.com>
 */

#ifndef _LINUX_LIVEUPDATE_ABI_VFIO_PCI_H
#define _LINUX_LIVEUPDATE_ABI_VFIO_PCI_H

#include <linux/compiler.h>
#include <linux/types.h>

/**
 * DOC: VFIO PCI Live Update ABI
 *
 * This header defines the ABI for preserving the state of a VFIO PCI device
 * files across a kexec reboot using LUO.
 *
 * Device metadata is serialized into memory which is then handed to the next
 * kernel via KHO.
 *
 * This interface is a contract. Any modification to any of the serialization
 * structs defined here constitutes a breaking change. Such changes require
 * incrementing the version number in the VFIO_PCI_LUO_FH_COMPATIBLE string.
 */

#define VFIO_PCI_LUO_FH_COMPATIBLE "vfio-pci-v1"

/**
 * struct vfio_pci_core_device_ser - Serialized state of a single VFIO PCI
 * device.
 *
 * @bdf: The device's PCI bus, device, and function number.
 * @domain: The device's PCI domain number (segment).
 * @reset_works: Non-zero if the device supports function resets.
 */
struct vfio_pci_core_device_ser {
	u16 bdf;
	u16 domain;
	u8 reset_works;
} __packed;

#endif /* _LINUX_LIVEUPDATE_ABI_VFIO_PCI_H */
