// SPDX-License-Identifier: GPL-2.0
/*
 * Saved capability registers.
 *
 * The capability registers that the PCI core saves and restores across resets
 * and power transitions are kept in a per-device store indexed by the offset
 * of the register in the device's configuration space. The layout of the
 * store is therefore described by the device rather than by the kernel, and
 * does not change when the core starts or stops saving a given register.
 *
 * The store covers capability registers only. The configuration space header
 * is saved in pci_dev.saved_config_space, which is embedded in struct pci_dev
 * so that saving it cannot fail.
 *
 * Only registers that have been reserved during device setup are saved. All
 * reservations must be made before the store is finalized, which lets the
 * values be held in a single allocation.
 *
 * Registers are stored a DWORD at a time, since reading configuration space
 * has no side effects and a DWORD read is always safe. Restoring is done at
 * the width of the register being restored, because neighbouring registers
 * within the same DWORD may not tolerate being written.
 */

#include <linux/bitmap.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "pci.h"

/*
 * Return the slot holding the value saved for the DWORD containing @off, or
 * NULL if there is nowhere to save it.
 */
static u32 *pci_saved_cap_slot(struct pci_dev *dev, unsigned int off)
{
	struct pci_saved_caps *caps = &dev->saved_caps;
	unsigned int dword = off / sizeof(u32);

	/*
	 * Capabilities whose layout comes from the device can name a register
	 * beyond the end of configuration space, which is refused at
	 * reservation time. Degrade quietly.
	 */
	if (dword >= PCI_CFG_SPACE_EXP_DWORDS)
		return NULL;

	if (!test_bit(dword, caps->dword_map)) {
		/*
		 * Reaching a register that nobody reserved is a kernel bug,
		 * unless the device described a capability that does not fit
		 * in its configuration space. That was refused and reported
		 * at reservation time.
		 */
		WARN_ONCE(!caps->incomplete,
			  "%s: config space offset %#x was not reserved\n",
			  pci_name(dev), off);
		return NULL;
	}

	/*
	 * The values are allocated once per device and never reallocated, so
	 * a device whose allocation failed simply has no saved state. Degrade
	 * quietly, the failure has already been reported.
	 */
	if (!caps->dword_val)
		return NULL;

	return &caps->dword_val[bitmap_weight(caps->dword_map, dword)];
}

/**
 * pci_saved_cap_reserved - test whether a register can be saved
 * @dev: the PCI device
 * @off: offset of the register in configuration space
 *
 * Return: true if the DWORD containing @off has a slot in the store, false
 * if nothing reserved it or the values could not be allocated.
 */
bool pci_saved_cap_reserved(struct pci_dev *dev, unsigned int off)
{
	struct pci_saved_caps *caps = &dev->saved_caps;
	unsigned int dword = off / sizeof(u32);

	if (!caps->dword_val || dword >= PCI_CFG_SPACE_EXP_DWORDS)
		return false;

	return test_bit(dword, caps->dword_map);
}

/**
 * pci_reserve_saved_cap - reserve space to save a capability register
 * @dev: the PCI device
 * @off: offset of the register in configuration space
 * @len: size of the register in bytes
 *
 * Reserve room to save the register at @off, which may then be saved with
 * pci_save_cap_word() and friends. Must be called during device setup, before
 * the store is finalized.
 *
 * Return: 0 on success, negative errno otherwise.
 */
int pci_reserve_saved_cap(struct pci_dev *dev, unsigned int off, unsigned int len)
{
	struct pci_saved_caps *caps = &dev->saved_caps;
	unsigned int first, last;

	if (WARN_ON(caps->finalized))
		return -EBUSY;

	/*
	 * The configuration space header is saved in
	 * pci_dev.saved_config_space, so only capability registers may be
	 * reserved here.
	 */
	if (WARN_ON(!len || off < PCI_STD_HEADER_SIZEOF))
		return -EINVAL;

	/*
	 * Capability sizes and table offsets can come from the device, so a
	 * malformed or unresponsive device can describe a range that runs past
	 * the end of configuration space. That is not a kernel bug, so fail
	 * quietly and let the caller report it.
	 */
	if (off + len > PCI_CFG_SPACE_EXP_SIZE) {
		caps->incomplete = true;
		return -EINVAL;
	}

	first = off / sizeof(u32);
	last = (off + len - 1) / sizeof(u32);

	bitmap_set(caps->dword_map, first, last - first + 1);

	return 0;
}

/**
 * pci_save_cap_dword - save a 32-bit capability register
 * @dev: the PCI device
 * @off: offset of the register in configuration space
 */
void pci_save_cap_dword(struct pci_dev *dev, unsigned int off)
{
	u32 *slot = pci_saved_cap_slot(dev, off);

	if (!slot)
		return;

	pci_read_config_dword(dev, ALIGN_DOWN(off, sizeof(u32)), slot);
}

/**
 * pci_save_cap_word - save a 16-bit capability register
 * @dev: the PCI device
 * @off: offset of the register in configuration space
 */
void pci_save_cap_word(struct pci_dev *dev, unsigned int off)
{
	pci_save_cap_dword(dev, ALIGN_DOWN(off, sizeof(u32)));
}

/**
 * pci_read_saved_cap_dword - read the saved value of a 32-bit register
 * @dev: the PCI device
 * @off: offset of the register in configuration space
 * @val: where to store the saved value
 *
 * Return: true if a saved value was available, false otherwise.
 */
bool pci_read_saved_cap_dword(struct pci_dev *dev, unsigned int off, u32 *val)
{
	u32 *slot = pci_saved_cap_slot(dev, off);

	if (!slot)
		return false;

	*val = *slot;

	return true;
}

/* Bit offset of the register at @off within its containing DWORD */
static unsigned int pci_saved_cap_shift(unsigned int off)
{
	return 8 * (off & (sizeof(u32) - 1));
}

/**
 * pci_read_saved_cap_word - read the saved value of a 16-bit register
 * @dev: the PCI device
 * @off: offset of the register in configuration space
 * @val: where to store the saved value
 *
 * Return: true if a saved value was available, false otherwise.
 */
bool pci_read_saved_cap_word(struct pci_dev *dev, unsigned int off, u16 *val)
{
	u32 dword;

	if (!pci_read_saved_cap_dword(dev, ALIGN_DOWN(off, sizeof(u32)), &dword))
		return false;

	*val = dword >> pci_saved_cap_shift(off);

	return true;
}

/**
 * pci_write_saved_cap_word - change the saved value of a 16-bit register
 * @dev: the PCI device
 * @off: offset of the register in configuration space
 * @val: value to save
 */
void pci_write_saved_cap_word(struct pci_dev *dev, unsigned int off, u16 val)
{
	unsigned int shift = pci_saved_cap_shift(off);
	u32 *slot = pci_saved_cap_slot(dev, off);

	if (!slot)
		return;

	*slot &= ~((u32)U16_MAX << shift);
	*slot |= (u32)val << shift;
}

/**
 * pci_restore_cap_dword - restore a 32-bit capability register
 * @dev: the PCI device
 * @off: offset of the register in configuration space
 */
void pci_restore_cap_dword(struct pci_dev *dev, unsigned int off)
{
	u32 val;

	if (pci_read_saved_cap_dword(dev, off, &val))
		pci_write_config_dword(dev, off, val);
}

/**
 * pci_restore_cap_word - restore a 16-bit capability register
 * @dev: the PCI device
 * @off: offset of the register in configuration space
 */
void pci_restore_cap_word(struct pci_dev *dev, unsigned int off)
{
	u16 val;

	if (pci_read_saved_cap_word(dev, off, &val))
		pci_write_config_word(dev, off, val);
}

/**
 * pci_saved_caps_finalize - allocate room for a device's saved registers
 * @dev: the PCI device
 *
 * Called once every capability that reserves registers has been initialized.
 * No register may be reserved after this.
 */
void pci_saved_caps_finalize(struct pci_dev *dev)
{
	struct pci_saved_caps *caps = &dev->saved_caps;

	caps->finalized = true;

	caps->nr_dwords = bitmap_weight(caps->dword_map, PCI_CFG_SPACE_EXP_DWORDS);
	if (!caps->nr_dwords)
		return;

	caps->dword_val = kzalloc_objs(*caps->dword_val, caps->nr_dwords, GFP_KERNEL);
	if (!caps->dword_val)
		pci_err(dev, "unable to allocate %u saved capability registers\n",
			caps->nr_dwords);
}

/**
 * pci_saved_caps_release - free a device's saved capability registers
 * @dev: the PCI device
 */
void pci_saved_caps_release(struct pci_dev *dev)
{
	struct pci_saved_caps *caps = &dev->saved_caps;

	kfree(caps->dword_val);
	caps->dword_val = NULL;
}
