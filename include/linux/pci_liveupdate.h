/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PCI Live Update support (Public/Driver API)
 *
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */
#ifndef LINUX_PCI_LIVEUPDATE_H
#define LINUX_PCI_LIVEUPDATE_H

#include <linux/kho/abi/pci.h>
#include <linux/liveupdate.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>

/**
 * struct pci_liveupdate - PCI Live Update state for a struct pci_dev
 * @outgoing: State preserved for the next kernel.
 * @incoming: State preserved by the previous kernel.
 * @was_incoming: True if this struct pci_dev was incoming-preserved when it was
 *                set up, i.e. it was matched to state preserved by the previous
 *                kernel. Unlike @incoming, this is never cleared, so it stays
 *                true after the device finishes participating in Live Update.
 * @frozen: True if the outgoing preservation status of this device is frozen
 *          and thus cannot be changed.
 */
struct pci_liveupdate {
	struct pci_dev_ser *outgoing;
	struct pci_dev_ser *incoming;
	bool was_incoming;
	bool frozen;
	unsigned int nr_auto_vfs;
};

struct pci_dev;

/**
 * struct pci_liveupdate_pf_ops - Live Update support of a PF driver
 * @preserve: Called when the PF is preserved automatically, because the first
 *            of its VFs is preserved. Preserve whatever the driver needs to
 *            keep the PF running across Live Update and store a handle to it
 *            in @data. Optional.
 * @unpreserve: Called when the PF is no longer preserved, because the last of
 *              its preserved VFs is unpreserved. Undo @preserve. Optional.
 * @finish: Called in the next kernel when the PF finishes participating in
 *          the Live Update, after all of its VFs did. Free whatever @preserve
 *          preserved. Optional.
 *
 * A PF driver that sets struct pci_driver::liveupdate advertises that it keeps
 * its PF running across Live Update, so that the PCI core can preserve the PF
 * automatically when some of its VFs are preserved, rather than requiring
 * userspace to preserve the PF explicitly. In the next kernel, the driver can
 * get @data with pci_liveupdate_get_driver_data() while the PF is incoming.
 *
 * The callbacks are called with the PCI Live Update lock held, so they must not
 * call back into the PCI Live Update API.
 */
struct pci_liveupdate_pf_ops {
	int (*preserve)(struct pci_dev *pf, u64 *data);
	void (*unpreserve)(struct pci_dev *pf, u64 data);
	void (*finish)(struct pci_dev *pf, u64 data);
};

#ifdef CONFIG_PCI_LIVEUPDATE
int pci_liveupdate_register_flb(struct liveupdate_file_handler *fh);
void pci_liveupdate_unregister_flb(struct liveupdate_file_handler *fh);
int pci_liveupdate_preserve(struct pci_dev *dev);
void pci_liveupdate_unpreserve(struct pci_dev *dev);
int pci_liveupdate_freeze(struct pci_dev *dev);
void pci_liveupdate_unfreeze(struct pci_dev *dev);
bool pci_liveupdate_can_finish(struct pci_dev *dev);
void pci_liveupdate_finish(struct pci_dev *dev);
bool pci_liveupdate_is_incoming(struct pci_dev *dev);
bool pci_liveupdate_is_preserved(struct pci_dev *dev);
int pci_liveupdate_get_driver_data(struct pci_dev *dev, u64 *data);
#else
static inline int pci_liveupdate_register_flb(struct liveupdate_file_handler *fh)
{
	return -EOPNOTSUPP;
}

static inline void pci_liveupdate_unregister_flb(struct liveupdate_file_handler *fh)
{
}

static inline int pci_liveupdate_preserve(struct pci_dev *dev)
{
	return -EOPNOTSUPP;
}

static inline void pci_liveupdate_unpreserve(struct pci_dev *dev)
{
}

static inline int pci_liveupdate_freeze(struct pci_dev *dev)
{
	return 0;
}

static inline void pci_liveupdate_unfreeze(struct pci_dev *dev)
{
}

static inline bool pci_liveupdate_can_finish(struct pci_dev *dev)
{
	return true;
}

static inline void pci_liveupdate_finish(struct pci_dev *dev)
{
}

static inline bool pci_liveupdate_is_incoming(struct pci_dev *dev)
{
	return false;
}

static inline bool pci_liveupdate_is_preserved(struct pci_dev *dev)
{
	return false;
}

static inline int pci_liveupdate_get_driver_data(struct pci_dev *dev, u64 *data)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* LINUX_PCI_LIVEUPDATE_H */
