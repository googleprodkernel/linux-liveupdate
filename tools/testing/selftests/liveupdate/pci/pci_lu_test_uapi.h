/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Interface of the PCI Live Update test driver (test_modules/pci_lu_test.c),
 * shared by the driver and luo_pci. Test-only, not a stable ABI.
 */
#ifndef _PCI_LU_TEST_UAPI_H
#define _PCI_LU_TEST_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define PCI_LU_TEST_DEV_NAME	"pci_lu_test"

/**
 * struct pci_lu_test_set_device - Argument of PCI_LU_TEST_IOCTL_SET_DEVICE
 * @argsz: sizeof(struct pci_lu_test_set_device)
 * @domain: PCI domain (segment) number of the device
 * @bus: Bus number of the device
 * @devfn: Device and function number of the device
 * @reserved: Must be zero
 *
 * Associate the file with a PCI device bound to the pci-lu-test driver. The
 * file can then be preserved with LIVEUPDATE_SESSION_PRESERVE_FD, which
 * preserves the device across Live Update. Each device can be associated with
 * at most one file at a time.
 */
struct pci_lu_test_set_device {
	__u32 argsz;
	__u32 domain;
	__u8 bus;
	__u8 devfn;
	__u16 reserved;
};

#define PCI_LU_TEST_IOCTL_SET_DEVICE \
	_IOW('P', 0x70, struct pci_lu_test_set_device)

#endif /* _PCI_LU_TEST_UAPI_H */
