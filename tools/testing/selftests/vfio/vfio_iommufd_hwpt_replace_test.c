// SPDX-License-Identifier: GPL-2.0-only
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/sizes.h>
#include <linux/vfio.h>

#include <libvfio.h>

#include "kselftest_harness.h"

static const char *device_bdf;

static void region_setup(struct iommu *iommu,
			 struct iova_allocator *iova_allocator,
			 struct dma_region *region, u64 size)
{
	const int flags = MAP_SHARED | MAP_ANONYMOUS;
	const int prot = PROT_READ | PROT_WRITE;
	void *vaddr;

	vaddr = mmap(NULL, size, prot, flags, -1, 0);
	VFIO_ASSERT_NE(vaddr, MAP_FAILED);

	region->vaddr = vaddr;
	region->iova = iova_allocator_alloc(iova_allocator, size);
	region->size = size;

	iommu_map(iommu, region);
}

static void region_teardown(struct iommu *iommu, struct dma_region *region)
{
	iommu_unmap(iommu, region);
	VFIO_ASSERT_EQ(munmap(region->vaddr, region->size), 0);
}

FIXTURE(vfio_iommufd_replace_hwpt_test) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
	struct iova_allocator *iova_allocator;
	struct dma_region memcpy_region;
	void *vaddr;

	u64 size;
	void *src;
	void *dst;
	iova_t src_iova;
	iova_t dst_iova;
};

FIXTURE_SETUP(vfio_iommufd_replace_hwpt_test)
{
	struct vfio_pci_driver *driver;

	self->iommu = iommu_init("iommufd");
	self->device = vfio_pci_device_init(device_bdf, self->iommu);
	self->iova_allocator = iova_allocator_init(self->iommu);

	driver = &self->device->driver;

	region_setup(self->iommu, self->iova_allocator, &self->memcpy_region, SZ_1G);
	region_setup(self->iommu, self->iova_allocator, &driver->region, SZ_2M);

	if (driver->ops)
		vfio_pci_driver_init(self->device);

	self->size = self->memcpy_region.size / 2;
	self->src = self->memcpy_region.vaddr;
	self->dst = self->src + self->size;

	self->src_iova = to_iova(self->device, self->src);
	self->dst_iova = to_iova(self->device, self->dst);
}

FIXTURE_TEARDOWN(vfio_iommufd_replace_hwpt_test)
{
	struct vfio_pci_driver *driver = &self->device->driver;

	if (driver->ops)
		vfio_pci_driver_remove(self->device);

	region_teardown(self->iommu, &self->memcpy_region);
	region_teardown(self->iommu, &driver->region);

	iova_allocator_cleanup(self->iova_allocator);
	vfio_pci_device_cleanup(self->device);
	iommu_cleanup(self->iommu);
}

FIXTURE_VARIANT(vfio_iommufd_replace_hwpt_test) {
	bool replace_hwpt;
};

FIXTURE_VARIANT_ADD(vfio_iommufd_replace_hwpt_test, domain_replace) {
	.replace_hwpt = true,
};

FIXTURE_VARIANT_ADD(vfio_iommufd_replace_hwpt_test, noreplace) {
	.replace_hwpt = false,
};

TEST_F(vfio_iommufd_replace_hwpt_test, memcpy)
{
	struct dma_region memcpy_region, driver_region;
	struct iommu *iommu2;

	if (self->device->driver.ops) {
		memset(self->src, 'x', self->size);
		memset(self->dst, 'y', self->size);

		vfio_pci_driver_memcpy_start(self->device,
					     self->src_iova,
					     self->dst_iova,
					     self->size,
					     100);
	}

	if (variant->replace_hwpt) {
		iommu2 = iommufd_iommu_init(self->iommu->iommufd,
					    self->device->dev_id);

		memcpy_region = self->memcpy_region;
		driver_region = self->device->driver.region;

		iommu_map(iommu2, &memcpy_region);
		iommu_map(iommu2, &driver_region);

		vfio_pci_device_attach_iommu(self->device, iommu2);
	}

	if (self->device->driver.ops) {
		ASSERT_EQ(0, vfio_pci_driver_memcpy_wait(self->device));
		ASSERT_EQ(0, memcmp(self->src, self->dst, self->size));
	}

	if (variant->replace_hwpt) {
		vfio_pci_device_attach_iommu(self->device, self->iommu);

		iommu_unmap(iommu2, &memcpy_region);
		iommu_unmap(iommu2, &driver_region);
		iommu_cleanup(iommu2);
	}
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	return test_harness_run(argc, argv);
}
