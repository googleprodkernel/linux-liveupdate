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

/**
 * struct pci_lu_test_saved_state - Argument of PCI_LU_TEST_IOCTL_GET_SAVED_STATE
 * @argsz: sizeof(struct pci_lu_test_saved_state)
 * @size: In: size of the buffer at @buf. Out: size of the saved state.
 * @buf: Pointer to a buffer receiving the saved state
 *
 * Get the saved state of the device associated with the file, serialized as a
 * struct pci_saved_state_ser (include/linux/kho/abi/pci.h). Before kexec, this
 * is the state saved when the file was preserved, as passed to the next
 * kernel. After kexec, this is the state passed by the previous kernel, as
 * deserialized when the file was retrieved, serialized again.
 *
 * Fails with ENODATA if the file has no saved state (i.e. it was neither
 * preserved nor retrieved), or ENOSPC (with @size set) if @buf is too small.
 */
struct pci_lu_test_saved_state {
	__u32 argsz;
	__u32 size;
	__u64 buf;
};

#define PCI_LU_TEST_IOCTL_GET_SAVED_STATE \
	_IOWR('P', 0x71, struct pci_lu_test_saved_state)

/*
 * Load the saved state passed by the previous kernel into the device
 * associated with a retrieved file, with pci_load_saved_state(), and restore
 * it with pci_restore_state(). Fails with EBUSY while the device is still
 * preserved, and ENODATA if the file has no such saved state.
 */
#define PCI_LU_TEST_IOCTL_RESTORE_SAVED_STATE	_IO('P', 0x72)

#endif /* _PCI_LU_TEST_UAPI_H */
