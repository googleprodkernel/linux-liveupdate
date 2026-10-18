/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */

#ifndef _LINUX_KHO_ABI_PCI_H
#define _LINUX_KHO_ABI_PCI_H

#include <linux/bits.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/pci_regs.h>
#include <linux/stddef.h>
#include <linux/types.h>

/**
 * DOC: PCI File-Lifecycle Bound (FLB) Live Update ABI
 *
 * The PCI core preserves its state across kexec as Live Update
 * File-Lifecycle Bound (FLB) data, identified by the compatible string
 * ``"pci"`` and laid out as described by the structs below.
 *
 * This layout is a contract between kernels. Rather than being versioned, it
 * is extended according to the following rules:
 *
 *  * Existing fields must never be moved, resized, removed, or have their
 *    meaning changed. New fields may only be appended to the end of a struct.
 *
 *  * A bit set in ``pci_ser.required_flags`` marks a feature that the next
 *    kernel must understand to safely take over the preserved devices. The
 *    next kernel panics if it finds a bit it does not know how to handle.
 *
 *  * A bit set in ``pci_ser.optional_flags`` marks a feature that the next
 *    kernel can safely ignore if it does not understand it.
 *
 *  * Fields appended to struct pci_ser must be accompanied by a flag that
 *    indicates they are present, since the size of struct pci_ser is not
 *    recorded.
 *
 *  * The size of each struct pci_dev_ser is recorded in
 *    ``pci_ser.dev_ser_size`` and is never smaller than
 *    ``PCI_DEV_SER_MIN_SIZE``. The next kernel ignores any fields beyond the
 *    ones it knows about, and must handle entries that are smaller than its
 *    own struct pci_dev_ser.
 */

#define PCI_LUO_FLB_COMPATIBLE "pci"

/**
 * struct pci_dev_ser - Serialized state about a single PCI device.
 *
 * @domain: The device's PCI domain number (segment).
 * @bdf: The device's PCI bus, device, and function number.
 * @refcount: Reference count used by the PCI core to keep track of whether it
 *            is done using a device's struct pci_dev_ser. The value of the
 *            reference count is equal to the number of preserved devices at or
 *            below it in the PCI hierarchy when the struct pci_dev_ser is in
 *            use, and 0 otherwise.
 * @bar_masks: The value each standard BAR register returns when read after
 *             writing all 1s to it, i.e. the mask that encodes the size of the
 *             BAR. Only the address bits are meaningful. A 64-bit BAR uses two
 *             entries, like it uses two registers. 0 if the BAR is not
 *             implemented. Used by the next kernel instead of sizing the BARs,
 *             which requires disabling decoding and would disrupt memory
 *             transactions targeting the device.
 * @rom_mask: Same as @bar_masks, but for the Expansion ROM BAR.
 * @probed_supported: The ``PCI_DEV_SER_PROBED_*`` bits that are valid in
 *                    @probed.
 * @probed: The results of probing properties of the device that require
 *          temporarily writing to it (``PCI_DEV_SER_PROBED_*``). Used by the
 *          next kernel instead of probing the device again, which would
 *          disrupt memory transactions targeting the device or devices below
 *          it.
 *
 * Each field after @refcount is only present if the ``PCI_SER_OPTIONAL_*``
 * flag that covers it is set in ``pci_ser.optional_flags``.
 */
struct pci_dev_ser {
	u32 domain;
	u16 bdf;
	u16 refcount;
	u32 bar_masks[PCI_STD_NUM_BARS];
	u32 rom_mask;
	u64 probed_supported;
	u64 probed;
} __packed;

/*
 * Bits in struct pci_dev_ser::probed_supported and probed:
 *
 *  * PCI_DEV_SER_PROBED_IO_WINDOW: The bridge has an I/O window.
 *  * PCI_DEV_SER_PROBED_PREF_WINDOW: The bridge has a prefetchable memory
 *    window.
 *  * PCI_DEV_SER_PROBED_PREF_64_WINDOW: The bridge's prefetchable memory
 *    window is 64-bit.
 *  * PCI_DEV_SER_PROBED_BROKEN_INTX_MASKING: INTx masking cannot be used on
 *    the device, i.e. struct pci_dev::broken_intx_masking is set.
 */
#define PCI_DEV_SER_PROBED_IO_WINDOW		BIT_ULL(0)
#define PCI_DEV_SER_PROBED_PREF_WINDOW		BIT_ULL(1)
#define PCI_DEV_SER_PROBED_PREF_64_WINDOW	BIT_ULL(2)
#define PCI_DEV_SER_PROBED_BROKEN_INTX_MASKING	BIT_ULL(3)

/*
 * The size of struct pci_dev_ser when this ABI was introduced. Since fields
 * can only be appended, every struct pci_dev_ser is at least this large.
 */
#define PCI_DEV_SER_MIN_SIZE offsetofend(struct pci_dev_ser, refcount)

/**
 * struct pci_ser - PCI Subsystem Live Update State
 *
 * This struct tracks state about all devices that are being preserved across
 * a Live Update for the next kernel. It contains only state owned by the PCI
 * core; the state a driver needs to resume its device is preserved separately
 * by that driver.
 *
 * @required_flags: Features that the next kernel must support to take over
 *                  the preserved devices. No flags are currently defined.
 * @optional_flags: Features that the next kernel may ignore if it does not
 *                  support them (``PCI_SER_OPTIONAL_*``).
 * @dev_ser_size: The size in bytes of each struct pci_dev_ser in @devices.
 * @nr_devices: The number of devices that were preserved.
 * @devices: Physical address of the first KHO block containing pci_dev_ser.
 */
struct pci_ser {
	u64 required_flags;
	u64 optional_flags;
	u32 dev_ser_size;
	u32 nr_devices;
	u64 devices;
} __packed;

/*
 * Bits in struct pci_ser::optional_flags. If a bit is set, @dev_ser_size is
 * large enough to contain the fields of struct pci_dev_ser that it covers:
 *
 *  * PCI_SER_OPTIONAL_BAR_MASKS: @bar_masks and @rom_mask
 *  * PCI_SER_OPTIONAL_PROBED: @probed_supported and @probed
 */
#define PCI_SER_OPTIONAL_BAR_MASKS	BIT_ULL(0)
#define PCI_SER_OPTIONAL_PROBED		BIT_ULL(1)

#endif /* _LINUX_KHO_ABI_PCI_H */
