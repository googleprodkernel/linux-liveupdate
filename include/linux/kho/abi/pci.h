/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * David Matlack <dmatlack@google.com>
 */

#ifndef _LINUX_KHO_ABI_PCI_H
#define _LINUX_KHO_ABI_PCI_H

#include <linux/compiler.h>
#include <linux/types.h>

/**
 * DOC: PCI File-Lifecycle Bound (FLB) Live Update ABI
 *
 * This header defines the ABI for preserving core PCI state across kexec using
 * Live Update File-Lifecycle Bound (FLB) data.
 *
 * This interface is a contract. Any modification to any of the serialization
 * structs defined here constitutes a breaking change. Such changes require
 * incrementing the version number in the PCI_LUO_FLB_COMPATIBLE string.
 */

#define PCI_LUO_FLB_COMPATIBLE "pci-v1"

/**
 * struct pci_dev_ser - Serialized state about a single PCI device.
 *
 * @domain: The device's PCI domain number (segment).
 * @bdf: The device's PCI bus, device, and function number.
 */
struct pci_dev_ser {
	u16 domain;
	u16 bdf;
} __packed;

/**
 * struct pci_ser - PCI Subsystem Live Update State
 *
 * This struct tracks state about all devices that are being preserved across
 * a Live Update for the next kernel.
 *
 * @nr_devices: The number of devices that were preserved.
 * @devices: Flexible array of pci_dev_ser structs for each device. Guaranteed
 *           to be sorted ascending by domain and bdf.
 */
struct pci_ser {
	u64 nr_devices;
	struct pci_dev_ser devices[];
} __packed;

#endif /* _LINUX_KHO_ABI_PCI_H */
