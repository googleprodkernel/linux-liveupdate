/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */

#ifndef _LINUX_KHO_ABI_VFIO_PCI_H
#define _LINUX_KHO_ABI_VFIO_PCI_H

#include <linux/compiler.h>
#include <linux/stddef.h>
#include <linux/types.h>

/**
 * DOC: VFIO PCI Live Update ABI
 *
 * vfio-pci preserves VFIO device files across kexec with a Live Update file
 * handler identified by the compatible string ``"vfio-pci-ovm-v1"``. The state of
 * each preserved file is a struct vfio_pci_core_device_ser, preserved with
 * KHO, whose physical address is passed to the next kernel as the file's
 * serialized data.
 *
 * This layout is a contract between kernels. Rather than being versioned, it
 * is extended according to the following rules:
 *
 *  * Existing fields must never be moved, resized, removed, or have their
 *    meaning changed. New fields may only be appended to the end of the
 *    struct, and ``vfio_pci_core_device_ser.size`` records how large the
 *    struct is.
 *
 *  * A bit set in ``vfio_pci_core_device_ser.flags`` marks a feature that the
 *    next kernel must understand to safely take over the device. The next
 *    kernel refuses to retrieve a file with a bit set that it does not know.
 */

#define VFIO_PCI_LUO_FH_COMPATIBLE "vfio-pci-ovm-v1"

/**
 * struct vfio_pci_core_device_ser - Serialized state of a vfio-pci device file
 *
 * @size: The size of this struct in bytes.
 * @flags: Features the next kernel must support to take over the device. No
 *         flags are currently defined.
 * @domain: The device's PCI domain number (segment).
 * @bdf: The device's PCI bus, device, and function number.
 * @reserved: Must be 0.
 * @saved_state: The physical address of the device's saved state, preserved
 *               with KHO (e.g. kho_alloc_preserve()), or 0 if the device has
 *               no saved state. The saved state is the state that vfio-pci
 *               restores when userspace closes the device, i.e. the state the
 *               device was in when it was first opened (right after it was
 *               reset), serialized as a struct pci_saved_state_ser (see
 *               include/linux/kho/abi/pci.h), which records its own size.
 * @vconfig: The physical address of the virtual configuration space that
 *           vfio-pci presented to userspace for the device when it was
 *           frozen (vfio_pci_core_device.vconfig), preserved with KHO, or 0
 *           if none was preserved.
 * @vconfig_size: The size of @vconfig in bytes, i.e. the size of the device's
 *                configuration space.
 */
struct vfio_pci_core_device_ser {
	u32 size;
	u32 flags;
	u32 domain;
	u16 bdf;
	u16 reserved;
	u64 saved_state;
	u64 vconfig;
	u32 vconfig_size;
} __packed;

/*
 * The size of struct vfio_pci_core_device_ser when this ABI was introduced.
 * Since fields can only be appended, every struct vfio_pci_core_device_ser is
 * at least this large.
 */
#define VFIO_PCI_CORE_DEVICE_SER_MIN_SIZE \
	offsetofend(struct vfio_pci_core_device_ser, vconfig_size)

#endif /* _LINUX_KHO_ABI_VFIO_PCI_H */
