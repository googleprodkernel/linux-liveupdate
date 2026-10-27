/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_IOMMU_H
#define SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_IOMMU_H

#include <linux/list.h>
#include <linux/types.h>

#include <libvfio/assert.h>

typedef u64 iova_t;

struct iommu_mode {
	const char *name;
	const char *container_path;
	unsigned long iommu_type;
	/*
	 * The device is not behind an IOMMU and DMAs to physical addresses
	 * (VFIO noiommu cdev bound to iommufd), see iommu_hva2iova().
	 */
	bool noiommu;
};

extern const char *default_iommu_mode;

struct dma_region {
	struct list_head link;
	void *vaddr;
	iova_t iova;
	u64 size;
};

struct iommu {
	const struct iommu_mode *mode;
	int container_fd;
	int iommufd;
	u32 ioas_id;
	struct list_head dma_regions;
};

struct iommu *iommu_init(const char *iommu_mode);
void iommu_cleanup(struct iommu *iommu);

int __iommu_map(struct iommu *iommu, struct dma_region *region);

static inline void iommu_map(struct iommu *iommu, struct dma_region *region)
{
	VFIO_ASSERT_EQ(__iommu_map(iommu, region), 0);
}

/*
 * Map @region->size bytes of the memfd @fd starting at @offset to
 * @region->iova with IOMMU_IOAS_MAP_FILE. @region->vaddr must point to a
 * mapping of the same part of @fd, so that it can be translated with
 * iommu_hva2iova(). Requires an iommufd IOMMU mode.
 */
int __iommu_map_file(struct iommu *iommu, struct dma_region *region, int fd,
		     u64 offset);

static inline void iommu_map_file(struct iommu *iommu, struct dma_region *region,
				  int fd, u64 offset)
{
	VFIO_ASSERT_EQ(__iommu_map_file(iommu, region, fd, offset), 0);
}

int __iommu_unmap(struct iommu *iommu, struct dma_region *region, u64 *unmapped);

static inline void iommu_unmap(struct iommu *iommu, struct dma_region *region)
{
	VFIO_ASSERT_EQ(__iommu_unmap(iommu, region, NULL), 0);
}

int __iommu_unmap_all(struct iommu *iommu, u64 *unmapped);

static inline void iommu_unmap_all(struct iommu *iommu)
{
	VFIO_ASSERT_EQ(__iommu_unmap_all(iommu, NULL), 0);
}

/*
 * Translate @vaddr, which must be inside a mapped dma_region, to the address
 * the device must use to access it with DMA.
 *
 * That is the IOVA, except in noiommu mode: Without an IOMMU there is no
 * translation and devices DMA to physical addresses. dma_region.iova is then
 * only used to look up the physical address with IOMMU_IOAS_NOIOMMU_GET_PA,
 * and the physical address is returned instead.
 */
int __iommu_hva2iova(struct iommu *iommu, void *vaddr, iova_t *iova);
iova_t iommu_hva2iova(struct iommu *iommu, void *vaddr);

bool iommu_mode_is_noiommu(const char *iommu_mode);

/*
 * Return the physical address that @iova is mapped to in noiommu mode. On
 * input @length is the maximum number of bytes to look up, on output the
 * number of bytes that are physically contiguous starting at @iova.
 */
int __iommu_noiommu_get_pa(struct iommu *iommu, iova_t iova, u64 *pa, u64 *length);
u64 iommu_noiommu_get_pa(struct iommu *iommu, iova_t iova, u64 *length);

struct iommu_iova_range *iommu_iova_ranges(struct iommu *iommu, u32 *nranges);

#define MODE_VFIO_TYPE1_IOMMU "vfio_type1_iommu"
#define MODE_VFIO_TYPE1V2_IOMMU "vfio_type1v2_iommu"
#define MODE_IOMMUFD_COMPAT_TYPE1 "iommufd_compat_type1"
#define MODE_IOMMUFD_COMPAT_TYPE1V2 "iommufd_compat_type1v2"
#define MODE_IOMMUFD "iommufd"
#define MODE_IOMMUFD_NOIOMMU "iommufd_noiommu"

/*
 * Generator for VFIO selftests fixture variants that replicate across all
 * possible IOMMU modes. Tests must define FIXTURE_VARIANT_ADD_IOMMU_MODE()
 * which should then use FIXTURE_VARIANT_ADD() to create the variant.
 *
 * iommufd_noiommu is not included since it only works with devices that are
 * not behind an IOMMU, and the other modes only work with devices that are.
 * Tests that support noiommu mode must add it explicitly.
 */
#define FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(...) \
FIXTURE_VARIANT_ADD_IOMMU_MODE(vfio_type1_iommu, ##__VA_ARGS__); \
FIXTURE_VARIANT_ADD_IOMMU_MODE(vfio_type1v2_iommu, ##__VA_ARGS__); \
FIXTURE_VARIANT_ADD_IOMMU_MODE(iommufd_compat_type1, ##__VA_ARGS__); \
FIXTURE_VARIANT_ADD_IOMMU_MODE(iommufd_compat_type1v2, ##__VA_ARGS__); \
FIXTURE_VARIANT_ADD_IOMMU_MODE(iommufd, ##__VA_ARGS__)

#endif /* SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_IOMMU_H */
