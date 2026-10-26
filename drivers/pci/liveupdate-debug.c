// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Live Update debug checks
 *
 * Warn about configuration space writes that disrupt devices preserved across
 * Live Update, e.g. disabling Bus Mastering, resetting the device or moving
 * its BARs, whether they come from the kernel or from userspace (e.g. setpci).
 * These writes are bugs in whoever issues them, but they are easy to miss
 * since their effect (e.g. DMA stopping) is often only visible to the user of
 * the device, so log them as they happen.
 *
 * Based on ProdKernel commit 1c7d426cdd00 ("PCI: liveupdate: Log warnings for
 * critical config writes on preserved devices").
 */

#include <linux/pci.h>

#include "liveupdate.h"
#include "pci.h"

/*
 * Config accessors can run in atomic context, so this can't take the PCI Live
 * Update rwsem. Racing with preservation changing is harmless for a debug
 * check.
 */
static bool pci_liveupdate_is_preserved_unlocked(const struct pci_dev *dev)
{
	return READ_ONCE(dev->liveupdate.outgoing) ||
	       READ_ONCE(dev->liveupdate.incoming);
}

static bool pci_write_overlaps(int where, int size, int reg, int reg_size)
{
	return where < reg + reg_size && where + size > reg;
}

/*
 * Return the value the 16-bit register at @reg will have after writing @size
 * bytes of @val at @where, which must overlap it, and its current value in
 * @old.
 */
static u16 pci_lu_new_value(const struct pci_dev *dev, int reg, int where,
			    int size, u32 val, bool locked, u16 *old)
{
	u64 bytes, mask;
	u32 data = 0;
	u16 word = 0;
	int shift;

	/* Bypass the accessors if the caller holds pci_lock. */
	if (locked) {
		dev->bus->ops->read(dev->bus, dev->devfn, reg, 2, &data);
		*old = data;
	} else {
		pci_bus_read_config_word(dev->bus, dev->devfn, reg, &word);
		*old = word;
	}

	/* Work in a 64-bit window that starts 4 bytes below @reg. */
	shift = (where - reg + 4) * 8;
	mask = (size == 4 ? 0xffffffffULL : (1ULL << (size * 8)) - 1) << shift;
	bytes = ((u64)*old << 32) & ~mask;
	bytes |= ((u64)val << shift) & mask;

	return bytes >> 32;
}

/*
 * Warn with @msg if the write changes the bits of the 16-bit register at @reg
 * selected by @mask to @target from something else.
 */
static void pci_lu_warn_on_transition(const struct pci_dev *dev, int reg,
				      int where, int size, u32 val, u16 mask,
				      u16 target, const char *msg, bool locked)
{
	u16 old, new;

	if (!pci_write_overlaps(where, size, reg, 2))
		return;

	new = pci_lu_new_value(dev, reg, where, size, val, locked, &old);
	if ((old & mask) != target && (new & mask) == target)
		pci_warn(dev, "%s on device preserved across Live Update\n", msg);
}

static bool pci_lu_is_bar_write(const struct pci_dev *dev, int where, int size)
{
	int bars_end;

	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE)
		bars_end = PCI_BASE_ADDRESS_1 + 4;
	else if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL)
		bars_end = PCI_BASE_ADDRESS_5 + 4;
	else
		return false;

	if (pci_write_overlaps(where, size, PCI_BASE_ADDRESS_0,
			       bars_end - PCI_BASE_ADDRESS_0))
		return true;

	if (dev->rom_base_reg &&
	    pci_write_overlaps(where, size, dev->rom_base_reg, 4))
		return true;

#ifdef CONFIG_PCI_IOV
	if (dev->is_physfn &&
	    pci_write_overlaps(where, size, dev->sriov->pos + PCI_SRIOV_BAR,
			       PCI_SRIOV_NUM_BARS * 4))
		return true;
#endif

	return false;
}

static bool pci_lu_is_window_write(const struct pci_dev *dev, int where,
				   int size)
{
	if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE)
		return false;

	/* Memory and prefetchable memory base and limit (incl. upper 32 bits) */
	return pci_write_overlaps(where, size, PCI_MEMORY_BASE, 4) ||
	       pci_write_overlaps(where, size, PCI_PREF_MEMORY_BASE, 4) ||
	       pci_write_overlaps(where, size, PCI_PREF_BASE_UPPER32, 8);
}

/**
 * pci_liveupdate_check_config_write() - Warn about disruptive config writes
 * @dev: The PCI device being written to
 * @where: Offset of the write in configuration space
 * @size: Size of the write in bytes (1, 2 or 4)
 * @val: The value being written
 * @locked: True if the caller holds pci_lock
 *
 * Called before every configuration space write through the pci_dev
 * accessors. Warns if the write disrupts @dev while it is preserved across Live
 * Update (outgoing or incoming).
 */
void pci_liveupdate_check_config_write(const struct pci_dev *dev, int where,
				       int size, u32 val, bool locked)
{
	if (likely(!pci_liveupdate_is_preserved_unlocked(dev)))
		return;

	pci_lu_warn_on_transition(dev, PCI_COMMAND, where, size, val,
				  PCI_COMMAND_MASTER, 0,
				  "Disabling Bus Mastering", locked);

	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE)
		pci_lu_warn_on_transition(dev, PCI_BRIDGE_CONTROL, where, size,
					  val, PCI_BRIDGE_CTL_BUS_RESET,
					  PCI_BRIDGE_CTL_BUS_RESET,
					  "Secondary Bus Reset", locked);

	if (dev->pm_cap) {
		pci_lu_warn_on_transition(dev, dev->pm_cap + PCI_PM_CTRL, where,
					  size, val, PCI_PM_CTRL_STATE_MASK,
					  PCI_D1, "Entering D1", locked);
		pci_lu_warn_on_transition(dev, dev->pm_cap + PCI_PM_CTRL, where,
					  size, val, PCI_PM_CTRL_STATE_MASK,
					  PCI_D2, "Entering D2", locked);
		pci_lu_warn_on_transition(dev, dev->pm_cap + PCI_PM_CTRL, where,
					  size, val, PCI_PM_CTRL_STATE_MASK,
					  PCI_D3hot, "Entering D3hot", locked);
	}

	if (pci_is_pcie((struct pci_dev *)dev)) {
		pci_lu_warn_on_transition(dev, dev->pcie_cap + PCI_EXP_DEVCTL,
					  where, size, val,
					  PCI_EXP_DEVCTL_BCR_FLR,
					  PCI_EXP_DEVCTL_BCR_FLR,
					  "Function Level Reset", locked);

		if (pcie_cap_has_lnkctl(dev))
			pci_lu_warn_on_transition(dev,
						  dev->pcie_cap + PCI_EXP_LNKCTL,
						  where, size, val,
						  PCI_EXP_LNKCTL_LD,
						  PCI_EXP_LNKCTL_LD,
						  "Disabling the link", locked);
	}

#ifdef CONFIG_PCI_IOV
	if (dev->is_physfn)
		pci_lu_warn_on_transition(dev, dev->sriov->pos + PCI_SRIOV_CTRL,
					  where, size, val, PCI_SRIOV_CTRL_VFE,
					  0, "Disabling VFs", locked);
#endif

	if (pci_lu_is_bar_write(dev, where, size))
		pci_warn(dev, "Writing BAR (offset %#x, size %d, value %#x) of device preserved across Live Update\n",
			 where, size, val);

	if (pci_lu_is_window_write(dev, where, size))
		pci_warn(dev, "Writing bridge window (offset %#x, size %d, value %#x) of bridge preserved across Live Update\n",
			 where, size, val);
}
