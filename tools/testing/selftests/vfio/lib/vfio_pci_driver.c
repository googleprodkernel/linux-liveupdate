// SPDX-License-Identifier: GPL-2.0-only
#include "kselftest.h"
#include <libvfio.h>

#ifdef __x86_64__
extern struct vfio_pci_driver_ops dsa_ops;
extern struct vfio_pci_driver_ops ioat_ops;
#endif
extern struct vfio_pci_driver_ops nv_falcon_ops;
extern struct vfio_pci_driver_ops igb_ops;

static struct vfio_pci_driver_ops *driver_ops[] = {
#ifdef __x86_64__
	&dsa_ops,
	&ioat_ops,
#endif
	&nv_falcon_ops,
	&igb_ops,
};

void vfio_pci_driver_probe(struct vfio_pci_device *device)
{
	struct vfio_pci_driver_ops *ops;
	int i;

	VFIO_ASSERT_NULL(device->driver.ops);

	for (i = 0; i < ARRAY_SIZE(driver_ops); i++) {
		ops = driver_ops[i];

		if (ops->probe(device))
			continue;

		device->driver.ops = ops;
	}
}

static void vfio_check_driver_op(struct vfio_pci_driver *driver, void *op,
				 const char *op_name)
{
	VFIO_ASSERT_NOT_NULL(driver->ops);
	VFIO_ASSERT_NOT_NULL(op, "Driver has no %s()\n", op_name);
	VFIO_ASSERT_EQ(driver->initialized, op != driver->ops->init);
	VFIO_ASSERT_EQ(driver->memcpy_in_progress, op == driver->ops->memcpy_wait);
}

#define VFIO_CHECK_DRIVER_OP(_driver, _op) do {				\
	struct vfio_pci_driver *__driver = (_driver);			\
	vfio_check_driver_op(__driver, __driver->ops->_op, #_op);	\
} while (0)

/*
 * Drivers lay out descriptor rings and other structures that the device
 * accesses with DMA in driver->region and address them relative to the start
 * of the region. Without an IOMMU that requires the region to be physically
 * contiguous.
 */
static void vfio_pci_driver_check_region(struct vfio_pci_device *device)
{
	struct dma_region *region = &device->driver.region;
	u64 length = region->size;

	if (!device->iommu->mode->noiommu)
		return;

	iommu_noiommu_get_pa(device->iommu, region->iova, &length);
	VFIO_ASSERT_EQ(length, region->size,
		       "Driver region must be physically contiguous in noiommu mode\n");
}

void vfio_pci_driver_init(struct vfio_pci_device *device)
{
	struct vfio_pci_driver *driver = &device->driver;

	VFIO_ASSERT_NOT_NULL(driver->region.vaddr);
	VFIO_CHECK_DRIVER_OP(driver, init);
	vfio_pci_driver_check_region(device);

	driver->ops->init(device);

	driver->initialized = true;
}

void vfio_pci_driver_remove(struct vfio_pci_device *device)
{
	struct vfio_pci_driver *driver = &device->driver;

	VFIO_CHECK_DRIVER_OP(driver, remove);

	driver->ops->remove(device);
	driver->initialized = false;
}

void vfio_pci_driver_send_msi(struct vfio_pci_device *device)
{
	struct vfio_pci_driver *driver = &device->driver;

	VFIO_CHECK_DRIVER_OP(driver, send_msi);

	driver->ops->send_msi(device);
}

void vfio_pci_driver_memcpy_start(struct vfio_pci_device *device,
				  iova_t src, iova_t dst, u64 size,
				  u64 count)
{
	struct vfio_pci_driver *driver = &device->driver;

	VFIO_ASSERT_LE(size, driver->max_memcpy_size);
	VFIO_ASSERT_LE(count, driver->max_memcpy_count);
	VFIO_CHECK_DRIVER_OP(driver, memcpy_start);

	driver->ops->memcpy_start(device, src, dst, size, count);
	driver->memcpy_in_progress = true;
}

int vfio_pci_driver_memcpy_wait(struct vfio_pci_device *device)
{
	struct vfio_pci_driver *driver = &device->driver;
	int r;

	VFIO_CHECK_DRIVER_OP(driver, memcpy_wait);

	r = driver->ops->memcpy_wait(device);
	driver->memcpy_in_progress = false;

	return r;
}

int vfio_pci_driver_memcpy(struct vfio_pci_device *device,
			   iova_t src, iova_t dst, u64 size)
{
	struct vfio_pci_driver *driver = &device->driver;
	u64 offset = 0;

	while (offset < size) {
		u64 chunk = min(size - offset, driver->max_memcpy_size);
		int ret;

		vfio_pci_driver_memcpy_start(device, src + offset,
					     dst + offset, chunk, 1);
		ret = vfio_pci_driver_memcpy_wait(device);
		if (ret)
			return ret;

		offset += chunk;
	}

	return 0;
}
