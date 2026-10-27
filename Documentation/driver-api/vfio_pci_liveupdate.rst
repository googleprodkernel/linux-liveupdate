.. SPDX-License-Identifier: GPL-2.0

================================
VFIO PCI Support for Live Update
================================

.. kernel-doc:: drivers/vfio/pci/vfio_pci_liveupdate.c
   :doc: VFIO PCI Live Update

Usage
=====

Before kexec::

  devfd = open("/dev/vfio/devices/noiommu-vfio0", O_RDWR);
  ioctl(devfd, VFIO_DEVICE_BIND_IOMMUFD, ...);
  /* Use the device, keeping DMA buffers in preserved memory (e.g. memfd) */

  session_fd = ioctl(luo_fd, LIVEUPDATE_IOCTL_CREATE_SESSION, ...);
  ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, { .fd = devfd, .token = ... });

  /* Disable interrupts before kexec */
  ioctl(devfd, VFIO_DEVICE_SET_IRQS, { .flags = VFIO_IRQ_SET_DATA_NONE |
                                                VFIO_IRQ_SET_ACTION_TRIGGER,
                                       .index = ..., .count = 0 });

  /* Load the next kernel, then reboot(LINUX_REBOOT_CMD_KEXEC) */

After kexec::

  session_fd = ioctl(luo_fd, LIVEUPDATE_IOCTL_RETRIEVE_SESSION, ...);
  devfd = ioctl(session_fd, LIVEUPDATE_SESSION_RETRIEVE_FD, { .token = ... });
  ioctl(devfd, VFIO_DEVICE_BIND_IOMMUFD, ...);
  ioctl(devfd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, ...);
  /* Map the preserved DMA buffers again, re-enable interrupts, ... */

  ioctl(session_fd, LIVEUPDATE_SESSION_FINISH, ...);

Driver API
==========

.. kernel-doc:: drivers/vfio/pci/vfio_pci_core.c
   :identifiers: vfio_pci_core_liveupdate_freeze vfio_pci_core_liveupdate_unfreeze
                 vfio_pci_core_liveupdate_finish

.. kernel-doc:: drivers/vfio/device_cdev.c
   :identifiers: vfio_device_cdev_open_file vfio_device_cdev_file_device
                 vfio_device_cdev_file_is_bound

Live Update ABI
===============

.. kernel-doc:: include/linux/kho/abi/vfio_pci.h
   :doc: VFIO PCI Live Update ABI

.. kernel-doc:: include/linux/kho/abi/vfio_pci.h
   :internal:

See Also
========

 * :doc:`/PCI/liveupdate`
 * :doc:`/core-api/liveupdate`
 * :doc:`/driver-api/vfio`
