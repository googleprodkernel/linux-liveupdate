// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Virtual Channel support
 *
 * Copyright (C) 2013 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/types.h>

#include "pci.h"

/* What to do with the registers of a VC capability */
enum pci_vc_op {
	PCI_VC_RESERVE,		/* Reserve room to save them */
	PCI_VC_RESTORE,		/* Write the saved capabilities back */
};

/**
 * pci_vc_do_word - Reserve or restore a word
 * @dev: device
 * @pos: config space position
 * @op: what to do with it
 *
 * Return: 0 on success, negative errno otherwise.
 */
static int pci_vc_do_word(struct pci_dev *dev, int pos, enum pci_vc_op op)
{
	switch (op) {
	case PCI_VC_RESERVE:
		return pci_reserve_saved_cap(dev, pos, sizeof(u16));
	case PCI_VC_RESTORE:
		if (!pci_saved_cap_reserved(dev, pos))
			return -ENOENT;

		pci_restore_cap_word(dev, pos);
		break;
	}

	return 0;
}

/**
 * pci_vc_do_dwords - Reserve or restore a series of dwords
 * @dev: device
 * @pos: starting config space position
 * @dwords: number of dwords
 * @op: what to do with them
 *
 * Return: 0 on success, negative errno otherwise.
 */
static int pci_vc_do_dwords(struct pci_dev *dev, int pos, int dwords,
			    enum pci_vc_op op)
{
	int i;

	if (op == PCI_VC_RESERVE)
		return pci_reserve_saved_cap(dev, pos, dwords * sizeof(u32));

	/*
	 * The device supplies the table offsets and sizes, so what it reports
	 * now may not match what was reserved during setup, e.g. if it is no
	 * longer responding. Restore all of the table or none of it.
	 */
	for (i = 0; i < dwords; i++)
		if (!pci_saved_cap_reserved(dev, pos + i * sizeof(u32)))
			return -ENOENT;

	for (i = 0; i < dwords; i++, pos += sizeof(u32))
		pci_restore_cap_dword(dev, pos);

	return 0;
}

/**
 * pci_vc_load_arb_table - load and wait for VC arbitration table
 * @dev: device
 * @pos: starting position of VC capability (VC/VC9/MFVC)
 *
 * Set Load VC Arbitration Table bit requesting hardware to apply the VC
 * Arbitration Table (previously loaded).  When the VC Arbitration Table
 * Status clears, hardware has latched the table into VC arbitration logic.
 */
static void pci_vc_load_arb_table(struct pci_dev *dev, int pos)
{
	u16 ctrl;

	pci_read_config_word(dev, pos + PCI_VC_PORT_CTRL, &ctrl);
	pci_write_config_word(dev, pos + PCI_VC_PORT_CTRL,
			      ctrl | PCI_VC_PORT_CTRL_LOAD_TABLE);
	if (pci_wait_for_pending(dev, pos + PCI_VC_PORT_STATUS,
				 PCI_VC_PORT_STATUS_TABLE))
		return;

	pci_err(dev, "VC arbitration table failed to load\n");
}

/**
 * pci_vc_load_port_arb_table - Load and wait for VC port arbitration table
 * @dev: device
 * @pos: starting position of VC capability (VC/VC9/MFVC)
 * @res: VC resource number, ie. VCn (0-7)
 *
 * Set Load Port Arbitration Table bit requesting hardware to apply the Port
 * Arbitration Table (previously loaded).  When the Port Arbitration Table
 * Status clears, hardware has latched the table into port arbitration logic.
 */
static void pci_vc_load_port_arb_table(struct pci_dev *dev, int pos, int res)
{
	int ctrl_pos, status_pos;
	u32 ctrl;

	ctrl_pos = pos + PCI_VC_RES_CTRL + (res * PCI_CAP_VC_PER_VC_SIZEOF);
	status_pos = pos + PCI_VC_RES_STATUS + (res * PCI_CAP_VC_PER_VC_SIZEOF);

	pci_read_config_dword(dev, ctrl_pos, &ctrl);
	pci_write_config_dword(dev, ctrl_pos,
			       ctrl | PCI_VC_RES_CTRL_LOAD_TABLE);

	if (pci_wait_for_pending(dev, status_pos, PCI_VC_RES_STATUS_TABLE))
		return;

	pci_err(dev, "VC%d port arbitration table failed to load\n", res);
}

/**
 * pci_vc_enable - Enable virtual channel
 * @dev: device
 * @pos: starting position of VC capability (VC/VC9/MFVC)
 * @res: VC res number, ie. VCn (0-7)
 *
 * A VC is enabled by setting the enable bit in matching resource control
 * registers on both sides of a link.  We therefore need to find the opposite
 * end of the link.  To keep this simple we enable from the downstream device.
 * RC devices do not have an upstream device, nor does it seem that VC9 do
 * (spec is unclear).  Once we find the upstream device, match the VC ID to
 * get the correct resource, disable and enable on both ends.
 */
static void pci_vc_enable(struct pci_dev *dev, int pos, int res)
{
	int ctrl_pos, status_pos, id, pos2, evcc, i, ctrl_pos2, status_pos2;
	u32 ctrl, header, cap1, ctrl2;
	struct pci_dev *link = NULL;

	/* Enable VCs from the downstream device */
	if (!pci_is_pcie(dev) || !pcie_downstream_port(dev))
		return;

	ctrl_pos = pos + PCI_VC_RES_CTRL + (res * PCI_CAP_VC_PER_VC_SIZEOF);
	status_pos = pos + PCI_VC_RES_STATUS + (res * PCI_CAP_VC_PER_VC_SIZEOF);

	pci_read_config_dword(dev, ctrl_pos, &ctrl);
	id = ctrl & PCI_VC_RES_CTRL_ID;

	pci_read_config_dword(dev, pos, &header);

	/* If there is no opposite end of the link, skip to enable */
	if (PCI_EXT_CAP_ID(header) == PCI_EXT_CAP_ID_VC9 ||
	    pci_is_root_bus(dev->bus))
		goto enable;

	pos2 = pci_find_ext_capability(dev->bus->self, PCI_EXT_CAP_ID_VC);
	if (!pos2)
		goto enable;

	pci_read_config_dword(dev->bus->self, pos2 + PCI_VC_PORT_CAP1, &cap1);
	evcc = cap1 & PCI_VC_CAP1_EVCC;

	/* VC0 is hardwired enabled, so we can start with 1 */
	for (i = 1; i < evcc + 1; i++) {
		ctrl_pos2 = pos2 + PCI_VC_RES_CTRL +
				(i * PCI_CAP_VC_PER_VC_SIZEOF);
		status_pos2 = pos2 + PCI_VC_RES_STATUS +
				(i * PCI_CAP_VC_PER_VC_SIZEOF);
		pci_read_config_dword(dev->bus->self, ctrl_pos2, &ctrl2);
		if ((ctrl2 & PCI_VC_RES_CTRL_ID) == id) {
			link = dev->bus->self;
			break;
		}
	}

	if (!link)
		goto enable;

	/* Disable if enabled */
	if (ctrl2 & PCI_VC_RES_CTRL_ENABLE) {
		ctrl2 &= ~PCI_VC_RES_CTRL_ENABLE;
		pci_write_config_dword(link, ctrl_pos2, ctrl2);
	}

	/* Enable on both ends */
	ctrl2 |= PCI_VC_RES_CTRL_ENABLE;
	pci_write_config_dword(link, ctrl_pos2, ctrl2);
enable:
	ctrl |= PCI_VC_RES_CTRL_ENABLE;
	pci_write_config_dword(dev, ctrl_pos, ctrl);

	if (!pci_wait_for_pending(dev, status_pos, PCI_VC_RES_STATUS_NEGO))
		pci_err(dev, "VC%d negotiation stuck pending\n", id);

	if (link && !pci_wait_for_pending(link, status_pos2,
					  PCI_VC_RES_STATUS_NEGO))
		pci_err(link, "VC%d negotiation stuck pending\n", id);
}

/**
 * pci_vc_restore_res_ctrl - Restore a VC Resource Control Register
 * @dev: device
 * @pos: starting position of VC capability (VC/VC9/MFVC)
 * @res: VC resource number, ie. VCn (0-7)
 */
static void pci_vc_restore_res_ctrl(struct pci_dev *dev, int pos, int res)
{
	int ctrl_pos = pos + PCI_VC_RES_CTRL + (res * PCI_CAP_VC_PER_VC_SIZEOF);
	u32 ctrl, tmp;

	if (!pci_read_saved_cap_dword(dev, ctrl_pos, &ctrl))
		return;

	/*
	 * For an FLR case, the VC config may remain.  Preserve enable bit,
	 * restore the rest.
	 */
	pci_read_config_dword(dev, ctrl_pos, &tmp);
	tmp &= PCI_VC_RES_CTRL_ENABLE;
	tmp |= ctrl & ~PCI_VC_RES_CTRL_ENABLE;
	pci_write_config_dword(dev, ctrl_pos, tmp);

	/* Load port arbitration table if used */
	if (ctrl & PCI_VC_RES_CTRL_ARB_SELECT)
		pci_vc_load_port_arb_table(dev, pos, res);

	/* Re-enable if needed */
	if ((ctrl ^ tmp) & PCI_VC_RES_CTRL_ENABLE)
		pci_vc_enable(dev, pos, res);
}

/**
 * pci_vc_do_saved_caps - Reserve or restore VC state
 * @dev: device
 * @pos: starting position of VC capability (VC/VC9/MFVC)
 * @op: what to do with the registers found along the way
 *
 * Walking Virtual Channel config space is complicated, so reserving and
 * restoring it are both driven from one function to reduce code and
 * guarantee that they agree on which registers are involved.
 *
 * Return: 0 on success, negative errno otherwise.
 */
static int pci_vc_do_saved_caps(struct pci_dev *dev, int pos, enum pci_vc_op op)
{
	u32 cap1;
	char evcc, lpevcc, parb_size;
	int i, ret;

	pci_read_config_dword(dev, pos + PCI_VC_PORT_CAP1, &cap1);
	/* Extended VC Count (not counting VC0) */
	evcc = cap1 & PCI_VC_CAP1_EVCC;
	/* Low Priority Extended VC Count (not counting VC0) */
	lpevcc = FIELD_GET(PCI_VC_CAP1_LPEVCC, cap1);
	/* Port Arbitration Table Entry Size (bits) */
	parb_size = 1 << FIELD_GET(PCI_VC_CAP1_ARB_SIZE, cap1);

	/*
	 * Port VC Control Register contains VC Arbitration Select, which
	 * cannot be modified when more than one LPVC is in operation.  We
	 * therefore save/restore it first, as only VC0 should be enabled
	 * after device reset.
	 */
	ret = pci_vc_do_word(dev, pos + PCI_VC_PORT_CTRL, op);
	if (ret)
		return ret;

	/*
	 * If we have any Low Priority VCs and a VC Arbitration Table Offset
	 * in Port VC Capability Register 2 then save/restore it next.
	 */
	if (lpevcc) {
		u32 cap2;
		int vcarb_offset;

		pci_read_config_dword(dev, pos + PCI_VC_PORT_CAP2, &cap2);
		vcarb_offset = FIELD_GET(PCI_VC_CAP2_ARB_OFF, cap2) * 16;

		if (vcarb_offset) {
			int size, vcarb_phases = 0;

			if (cap2 & PCI_VC_CAP2_128_PHASE)
				vcarb_phases = 128;
			else if (cap2 & PCI_VC_CAP2_64_PHASE)
				vcarb_phases = 64;
			else if (cap2 & PCI_VC_CAP2_32_PHASE)
				vcarb_phases = 32;

			/* Fixed 4 bits per phase per lpevcc (plus VC0) */
			size = ((lpevcc + 1) * vcarb_phases * 4) / 8;

			if (size) {
				int off = pos + vcarb_offset;

				ret = pci_vc_do_dwords(dev, off, size / 4, op);
				if (ret)
					return ret;

				/*
				 * On restore, we need to signal hardware to
				 * re-load the VC Arbitration Table.
				 */
				if (op == PCI_VC_RESTORE)
					pci_vc_load_arb_table(dev, pos);
			}
		}
	}

	/*
	 * In addition to each VC Resource Control Register, we may have a
	 * Port Arbitration Table attached to each VC.  The Port Arbitration
	 * Table Offset in each VC Resource Capability Register tells us if
	 * it exists.  The entry size is global from the Port VC Capability
	 * Register1 above.  The number of phases is determined per VC.
	 */
	for (i = 0; i < evcc + 1; i++) {
		u32 cap;
		int parb_offset;

		pci_read_config_dword(dev, pos + PCI_VC_RES_CAP +
				      (i * PCI_CAP_VC_PER_VC_SIZEOF), &cap);
		parb_offset = FIELD_GET(PCI_VC_RES_CAP_ARB_OFF, cap) * 16;
		if (parb_offset) {
			int size, parb_phases = 0;

			if (cap & PCI_VC_RES_CAP_256_PHASE)
				parb_phases = 256;
			else if (cap & (PCI_VC_RES_CAP_128_PHASE |
					PCI_VC_RES_CAP_128_PHASE_TB))
				parb_phases = 128;
			else if (cap & PCI_VC_RES_CAP_64_PHASE)
				parb_phases = 64;
			else if (cap & PCI_VC_RES_CAP_32_PHASE)
				parb_phases = 32;

			size = (parb_size * parb_phases) / 8;

			if (size) {
				int off = pos + parb_offset;

				ret = pci_vc_do_dwords(dev, off, size / 4, op);
				if (ret)
					return ret;
			}
		}

		/* VC Resource Control Register */
		if (op == PCI_VC_RESTORE) {
			pci_vc_restore_res_ctrl(dev, pos, i);
		} else {
			int ctrl_pos = pos + PCI_VC_RES_CTRL +
				       (i * PCI_CAP_VC_PER_VC_SIZEOF);

			ret = pci_vc_do_dwords(dev, ctrl_pos, 1, op);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static struct {
	u16 id;
	const char *name;
} vc_caps[] = { { PCI_EXT_CAP_ID_MFVC, "MFVC" },
		{ PCI_EXT_CAP_ID_VC, "VC" },
		{ PCI_EXT_CAP_ID_VC9, "VC9" } };

/**
 * pci_restore_vc_state - Restore VC state from the saved capability store
 * @dev: device
 *
 * For each type of VC capability, VC/VC9/MFVC, find the capability and
 * restore it from the saved capability store.
 */
void pci_restore_vc_state(struct pci_dev *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vc_caps); i++) {
		int pos = pci_find_ext_capability(dev, vc_caps[i].id);

		if (!pos)
			continue;

		pci_vc_do_saved_caps(dev, pos, PCI_VC_RESTORE);
	}
}

/**
 * pci_vc_reserve_saved_caps - Reserve room for VC capability registers
 * @dev: device
 *
 * For each type of VC capability, VC/VC9/MFVC, find the capability and
 * reserve room to save every register it needs.
 */
void pci_vc_reserve_saved_caps(struct pci_dev *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vc_caps); i++) {
		int pos = pci_find_ext_capability(dev, vc_caps[i].id);

		if (!pos)
			continue;

		if (pci_vc_do_saved_caps(dev, pos, PCI_VC_RESERVE))
			pci_err(dev, "unable to reserve %s save state\n",
				vc_caps[i].name);
	}
}
