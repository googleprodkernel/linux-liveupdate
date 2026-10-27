// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test that a device bound to vfio-pci keeps running across a kexec Live
 * Update when its VFIO cdev file is preserved in a LUO session.
 *
 * Usage: vfio_pci_liveupdate_kexec_test [-s 1|2] [-v] segment:bus:device.function
 *
 * The device must not be behind an IOMMU and must be bound to vfio-pci with
 * vfio.enable_unsafe_noiommu_mode=1 (in both kernels), since iommufd can't be
 * preserved across Live Update yet: Without an IOMMU there is no IOMMU state
 * that needs to survive kexec for the device to keep doing DMA, it just needs
 * its buffers to stay at the same physical addresses.
 *
 * With -v, the device must be an SR-IOV PF. Stage 1 then binds it to
 * pci-pf-stub, enables one VF, binds the VF to vfio-pci and tests the VF
 * instead. The PCI core preserves the PF automatically.
 *
 * Stage 1 (before kexec):
 *  - Map a memfd, which holds the driver region, the memcpy buffers and the
 *    test state, at fixed IOVAs, and record the physical addresses.
 *  - If the device has a selftests driver, initialize it and start a memcpy
 *    as long as the driver allows, which keeps running across kexec on real
 *    hardware.
 *  - Preserve the memfd and the device in a LUO session, and check that the
 *    device can't be reset or put in a low power state while preserved, and
 *    that kexec fails while the device's interrupts are enabled.
 *  - Disable interrupts and snapshot config space.
 *
 * Stage 2 (after kexec):
 *  - Before touching the device, check that it is bound to vfio-pci, that its
 *    config space (and the PF's) is unchanged, i.e. the device was neither
 *    reset nor reconfigured, and that it can't be opened through its cdev.
 *  - Retrieve the memfd and the device, bind the device to a new iommufd,
 *    map the memfd at the same IOVAs and check the physical addresses are
 *    the same. Check that the session can't be finished before all that,
 *    and that the device can't be reset or preserved again until then.
 *  - If the device has a selftests driver, resume the driver without
 *    resetting the device (the driver checks the device is still programmed
 *    the same way), wait for the memcpy started before kexec and check its
 *    result, then check the device still works by doing another memcpy and
 *    sending an MSI.
 *  - Finish the session, check the device can be opened normally again and
 *    clean up, which resets the device.
 */
#include <fcntl.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

#include <linux/limits.h>
#include <linux/pci_regs.h>
#include <linux/sizes.h>

#include <libliveupdate.h>
#include <libvfio.h>

#define STATE_SESSION_NAME	"vfio_pci_liveupdate_kexec_test_state"
#define DEVICE_SESSION_NAME	"vfio_pci_liveupdate_kexec_test"

enum {
	STATE_TOKEN = 1,
	MEMFD_TOKEN,
	DEVICE_TOKEN,
};

/*
 * Layout of the preserved memfd: the driver region, and the source and
 * destination buffers of memcpys, each 2MiB, followed by the test state. Each
 * region must be physically contiguous since there is no IOMMU, which requires
 * the memfd to be backed by 2MiB THPs (transparent_hugepage_shmem=always).
 */
#define REGION_SIZE		SZ_2M
#define DRIVER_REGION_OFFSET	0
#define SRC_REGION_OFFSET	(1 * REGION_SIZE)
#define DST_REGION_OFFSET	(2 * REGION_SIZE)
#define TEST_STATE_OFFSET	(3 * REGION_SIZE)
#define MEMFD_SIZE		(4 * REGION_SIZE)

/* The regions are mapped at the same IOVAs before and after kexec. */
#define REGIONS_IOVA		SZ_4G

#define BDF_LEN			16
#define CFG_SIZE		PCI_CFG_SPACE_EXP_SIZE
#define RESOURCE_SIZE		2048
#define TEST_STATE_MAGIC	0x5646494f4c555354ULL	/* "VFIOLUST" */

/* Snapshot of the config space and resources of a device. */
struct snapshot {
	char bdf[BDF_LEN];
	int cfg_len;
	u8 cfg[CFG_SIZE];
	char resource[RESOURCE_SIZE];
};

/* State passed from stage 1 to stage 2, in the preserved memfd. */
struct test_state {
	u64 magic;

	/* The device that is preserved, and its PF in VF mode. */
	char bdf[BDF_LEN];
	char pf_bdf[BDF_LEN];
	int numvfs;

	/* Physical addresses of the regions before kexec. */
	u64 driver_region_pa;
	u64 src_region_pa;
	u64 dst_region_pa;

	/* The memcpy started before kexec, if the device has a driver. */
	bool has_driver;
	u64 memcpy_size;
	u64 memcpy_count;
	u8 memcpy_pattern;

	struct snapshot device_snapshot;
	struct snapshot pf_snapshot;

	/* Config space as seen through VFIO, see save_vconfig(). */
	int vconfig_len;
	u8 vconfig[CFG_SIZE];
};

static bool vf_mode;

static void *memfd_vaddr;
static struct dma_region driver_region;
static struct dma_region src_region;
static struct dma_region dst_region;
static struct test_state *state;

static void read_sysfs(const char *bdf, const char *file, void *buf, int len,
		       int *out_len)
{
	char path[PATH_MAX];
	int fd, ret, total = 0;

	snprintf_assert(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", bdf, file);
	fd = open(path, O_RDONLY);
	VFIO_ASSERT_GE(fd, 0, "Failed to open %s\n", path);

	while (total < len) {
		ret = read(fd, buf + total, len - total);
		VFIO_ASSERT_GE(ret, 0, "Failed to read %s\n", path);
		if (!ret)
			break;
		total += ret;
	}

	VFIO_ASSERT_EQ(close(fd), 0);
	*out_len = total;
}

static void take_snapshot(struct snapshot *snap, const char *bdf)
{
	int len;

	memset(snap, 0, sizeof(*snap));
	snprintf_assert(snap->bdf, sizeof(snap->bdf), "%s", bdf);

	read_sysfs(bdf, "config", snap->cfg, sizeof(snap->cfg), &snap->cfg_len);
	VFIO_ASSERT_GE(snap->cfg_len, PCI_STD_HEADER_SIZEOF);

	read_sysfs(bdf, "resource", snap->resource, sizeof(snap->resource) - 1, &len);
}

static u32 cfg_dword(const u8 *cfg, int off)
{
	return cfg[off] | cfg[off + 1] << 8 | cfg[off + 2] << 16 |
	       (u32)cfg[off + 3] << 24;
}

static void mask_range(u8 *mask, int off, int len, u8 val)
{
	if (off < 0 || off + len > CFG_SIZE)
		return;

	memset(mask + off, val, len);
}

#define MAX_EXT_CAPS	64
#define MAX_EXT_CAP_LEN	0x80

/*
 * Build the mask of config space bits that must not change across Live
 * Update. Status registers can change at any time, so they are excluded.
 * VFIO masks INTx when it freezes the device for kexec (interrupts must be
 * disabled by then, and nothing in the next kernel handles them until
 * userspace sets them up again), so INTx Disable may be set afterwards.
 * Devices can keep live, device-private data in config space, e.g. in vendor
 * specific capabilities or outside of any capability in extended config
 * space, so only compare the header and the standard (extended) capabilities.
 * The size of an extended capability is not recorded anywhere, so assume it
 * extends up to the next one, but at most MAX_EXT_CAP_LEN bytes.
 */
static void build_mask(const struct snapshot *snap, u8 *mask)
{
	int ext_pos[MAX_EXT_CAPS], ext_id[MAX_EXT_CAPS];
	int pos, ttl, i, j, end, nr_ext = 0;
	const u8 *cfg = snap->cfg;

	memset(mask, 0, CFG_SIZE);
	mask_range(mask, 0, PCI_CFG_SPACE_SIZE, 0xff);
	mask_range(mask, PCI_STATUS, 2, 0);
	mask[PCI_COMMAND + 1] &= ~(PCI_COMMAND_INTX_DISABLE >> 8);

	pos = cfg[PCI_CAPABILITY_LIST] & ~3;
	for (ttl = 48; pos >= PCI_STD_HEADER_SIZEOF && ttl; ttl--) {
		switch (cfg[pos + PCI_CAP_LIST_ID]) {
		case PCI_CAP_ID_PM:
			mask[pos + PCI_PM_CTRL + 1] &= ~(PCI_PM_CTRL_PME_STATUS >> 8);
			break;
		case PCI_CAP_ID_EXP:
			mask_range(mask, pos + PCI_EXP_DEVSTA, 2, 0);
			mask_range(mask, pos + PCI_EXP_LNKSTA, 2, 0);
			mask_range(mask, pos + PCI_EXP_SLTSTA, 2, 0);
			mask_range(mask, pos + PCI_EXP_RTSTA, 4, 0);
			mask_range(mask, pos + PCI_EXP_LNKSTA2, 2, 0);
			mask_range(mask, pos + PCI_EXP_SLTSTA2, 2, 0);
			break;
		case PCI_CAP_ID_VNDR:
			if (cfg[pos + PCI_CAP_FLAGS] > 2)
				mask_range(mask, pos + 2, cfg[pos + PCI_CAP_FLAGS] - 2, 0);
			break;
		}
		pos = cfg[pos + PCI_CAP_LIST_NEXT] & ~3;
	}

	if (snap->cfg_len <= PCI_CFG_SPACE_SIZE)
		return;

	pos = PCI_CFG_SPACE_SIZE;
	for (ttl = 960; pos >= PCI_CFG_SPACE_SIZE && ttl && nr_ext < MAX_EXT_CAPS; ttl--) {
		u32 hdr = cfg_dword(cfg, pos);

		if (!hdr || hdr == 0xffffffff)
			break;

		ext_pos[nr_ext] = pos;
		ext_id[nr_ext++] = PCI_EXT_CAP_ID(hdr);
		pos = PCI_EXT_CAP_NEXT(hdr);
	}

	for (i = 0; i < nr_ext; i++) {
		pos = ext_pos[i];

		mask_range(mask, pos, 4, 0xff);
		if (ext_id[i] == PCI_EXT_CAP_ID_VNDR || ext_id[i] == PCI_EXT_CAP_ID_DVSEC)
			continue;

		end = pos + MAX_EXT_CAP_LEN;
		for (j = 0; j < nr_ext; j++) {
			if (ext_pos[j] > pos && ext_pos[j] < end)
				end = ext_pos[j];
		}
		mask_range(mask, pos, end - pos, 0xff);

		switch (ext_id[i]) {
		case PCI_EXT_CAP_ID_ERR:
			mask_range(mask, pos + PCI_ERR_UNCOR_STATUS, 4, 0);
			mask_range(mask, pos + PCI_ERR_COR_STATUS, 4, 0);
			mask_range(mask, pos + PCI_ERR_HEADER_LOG, 16, 0);
			mask_range(mask, pos + PCI_ERR_ROOT_STATUS, 4, 0);
			mask_range(mask, pos + PCI_ERR_ROOT_ERR_SRC, 4, 0);
			break;
		case PCI_EXT_CAP_ID_SRIOV:
			mask_range(mask, pos + PCI_SRIOV_STATUS, 2, 0);
			break;
		}
	}
}

/* Check that the device still looks exactly like @before. */
static void check_snapshot(const struct snapshot *before, const char *when)
{
	struct snapshot *after;
	u16 before_cmd, after_cmd;
	u8 mask[CFG_SIZE];
	int i, errors = 0;

	after = calloc_assert(1, sizeof(*after));
	take_snapshot(after, before->bdf);
	build_mask(before, mask);

	printf("Checking that config space of %s is unchanged %s\n", before->bdf, when);

	if (after->cfg_len != before->cfg_len) {
		printf("%s: config space size changed from %d to %d\n",
		       before->bdf, before->cfg_len, after->cfg_len);
		errors++;
	}

	for (i = 0; i < before->cfg_len && i < after->cfg_len; i++) {
		if (!((before->cfg[i] ^ after->cfg[i]) & mask[i]))
			continue;

		printf("%s: config[0x%03x] changed from 0x%02x to 0x%02x\n",
		       before->bdf, i, before->cfg[i], after->cfg[i]);
		errors++;
	}

	if (strcmp(before->resource, after->resource)) {
		printf("%s: resources changed from:\n%s\nto:\n%s\n",
		       before->bdf, before->resource, after->resource);
		errors++;
	}

	/* Already covered above, but call out the most important bit. */
	before_cmd = before->cfg[PCI_COMMAND] | before->cfg[PCI_COMMAND + 1] << 8;
	after_cmd = after->cfg[PCI_COMMAND] | after->cfg[PCI_COMMAND + 1] << 8;
	if ((before_cmd & PCI_COMMAND_MASTER) && !(after_cmd & PCI_COMMAND_MASTER))
		printf("%s: Bus Master Enable was cleared (command 0x%04x)\n",
		       before->bdf, after_cmd);

	VFIO_ASSERT_EQ(errors, 0, "%s changed across Live Update\n", before->bdf);
	free(after);
}

static void check_driver(const char *bdf, const char *driver)
{
	char *current = sysfs_driver_get(bdf);

	VFIO_ASSERT_NOT_NULL(current, "%s is not bound to a driver\n", bdf);
	VFIO_ASSERT_EQ(strcmp(current, driver), 0, "%s is bound to %s, not %s\n",
		       bdf, current, driver);
	free(current);
}

static void memfd_map_region(struct vfio_pci_device *device, int memfd,
			     struct dma_region *region, u64 offset)
{
	*region = (struct dma_region) {
		.vaddr = memfd_vaddr + offset,
		.iova = REGIONS_IOVA + offset,
		.size = REGION_SIZE,
	};
	iommu_map_file(device->iommu, region, memfd, offset);
}

/*
 * Map the regions of the memfd at fixed IOVAs, so that they can be mapped at
 * the same IOVAs again after kexec.
 */
static void memfd_map(struct vfio_pci_device *device, int memfd)
{
	memfd_vaddr = mmap_reserve(MEMFD_SIZE, SZ_2M, 0);
	memfd_vaddr = mmap(memfd_vaddr, MEMFD_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_FIXED, memfd, 0);
	VFIO_ASSERT_NE(memfd_vaddr, MAP_FAILED);

	memfd_map_region(device, memfd, &driver_region, DRIVER_REGION_OFFSET);
	memfd_map_region(device, memfd, &src_region, SRC_REGION_OFFSET);
	memfd_map_region(device, memfd, &dst_region, DST_REGION_OFFSET);

	device->driver.region = driver_region;
	state = memfd_vaddr + TEST_STATE_OFFSET;
}

static u64 region_pa(struct vfio_pci_device *device, struct dma_region *region)
{
	u64 length = region->size;
	u64 pa;

	pa = iommu_noiommu_get_pa(device->iommu, region->iova, &length);
	VFIO_ASSERT_EQ(length, region->size,
		       "Region at IOVA 0x%lx is not physically contiguous. Is the memfd backed by THPs (transparent_hugepage_shmem=always)?\n",
		       region->iova);

	return pa;
}

/* The largest memcpy the driver and the buffers allow. */
static u64 memcpy_size(struct vfio_pci_device *device)
{
	return min_t(u64, device->driver.max_memcpy_size, REGION_SIZE);
}

static void memcpy_buffers(struct vfio_pci_device *device, u64 size, void **src,
			   void **dst)
{
	VFIO_ASSERT_LE(size, REGION_SIZE);

	*src = src_region.vaddr;
	*dst = dst_region.vaddr;
}

/* Do one memcpy with the device and check the result. */
static void do_memcpy(struct vfio_pci_device *device, u8 pattern)
{
	u64 size = memcpy_size(device);
	void *src, *dst;

	memcpy_buffers(device, size, &src, &dst);
	memset(src, pattern, size);
	memset(dst, ~pattern, size);

	printf("Doing a memcpy of 0x%lx bytes\n", size);
	VFIO_ASSERT_EQ(vfio_pci_driver_memcpy(device, to_iova(device, src),
					      to_iova(device, dst), size), 0);
	VFIO_ASSERT_EQ(memcmp(src, dst, size), 0);
}

static double now(void)
{
	struct timespec ts;

	VFIO_ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &ts), 0);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/*
 * Start as many memcpys of the largest size as the driver allows without
 * waiting for them. On real hardware this keeps the device busy across kexec.
 * Devices emulated by QEMU tend to complete DMA synchronously when it is
 * kicked off, in which case this only checks that the result of DMA done
 * right before kexec survives.
 */
static void start_memcpy(struct vfio_pci_device *device, u8 pattern)
{
	void *src, *dst;

	state->memcpy_size = memcpy_size(device);
	state->memcpy_count = device->driver.max_memcpy_count;
	state->memcpy_pattern = pattern;

	memcpy_buffers(device, state->memcpy_size, &src, &dst);
	memset(src, state->memcpy_pattern, state->memcpy_size);
	memset(dst, ~state->memcpy_pattern, state->memcpy_size);

	printf("Starting %lu memcpys of 0x%lx bytes\n", state->memcpy_count,
	       state->memcpy_size);
	vfio_pci_driver_memcpy_start(device, to_iova(device, src),
				     to_iova(device, dst), state->memcpy_size,
				     state->memcpy_count);
}

/*
 * Time how long the memcpy started by start_memcpy() takes, to show how long
 * the device keeps doing DMA across kexec.
 */
static void time_memcpy(struct vfio_pci_device *device)
{
	double start = now();

	start_memcpy(device, 0x69);
	VFIO_ASSERT_EQ(vfio_pci_driver_memcpy_wait(device), 0);
	printf("The memcpys took %.3f seconds\n", now() - start);
}

static void check_memcpy(struct vfio_pci_device *device)
{
	void *src, *dst;

	printf("Waiting for the memcpys started before kexec\n");
	VFIO_ASSERT_EQ(vfio_pci_driver_memcpy_wait(device), 0);

	memcpy_buffers(device, state->memcpy_size, &src, &dst);
	VFIO_ASSERT_EQ(*(u8 *)src, state->memcpy_pattern);
	VFIO_ASSERT_EQ(memcmp(src, dst, state->memcpy_size), 0);
}

static void check_msi(struct vfio_pci_device *device)
{
	int msi = device->driver.msi;
	u64 value;

	printf("Sending an MSI\n");
	vfio_pci_msix_enable(device, msi, 1);
	vfio_pci_driver_send_msi(device);
	VFIO_ASSERT_EQ(read(device->msi_eventfds[msi], &value, 8), 8);
	VFIO_ASSERT_EQ(value, 1);
}

static bool kexec_image_loaded(void)
{
	char buf[4] = {};
	int fd;

	fd = open("/sys/kernel/kexec_loaded", O_RDONLY);
	if (fd < 0)
		return false;

	VFIO_ASSERT_GT(read(fd, buf, sizeof(buf) - 1), 0);
	VFIO_ASSERT_EQ(close(fd), 0);

	return buf[0] == '1';
}

static bool is_vf(const char *bdf)
{
	char path[PATH_MAX];

	snprintf_assert(path, sizeof(path), "/sys/bus/pci/devices/%s/physfn", bdf);
	return !access(path, F_OK);
}

/*
 * Disabling MSI or MSI-X of a preserved device must leave INTx masked in the
 * device, since nothing handles INTx during the Live Update. VFIO's virtual
 * config space, and thus what userspace sees through VFIO, is not affected.
 * VFs don't have INTx (Interrupt Disable is hardwired to 0), so there is
 * nothing to check for them.
 */
static void disable_msix_check_intx_masked(struct vfio_pci_device *device)
{
	u16 vfio_cmd = vfio_pci_config_readw(device, PCI_COMMAND);
	u8 cfg[PCI_COMMAND + 2];
	u16 cmd;
	int len;

	vfio_pci_msix_disable(device);

	if (is_vf(device->bdf))
		return;

	printf("Checking that INTx is masked after disabling MSI-X\n");
	read_sysfs(device->bdf, "config", cfg, sizeof(cfg), &len);
	VFIO_ASSERT_EQ(len, (int)sizeof(cfg));
	cmd = cfg[PCI_COMMAND] | cfg[PCI_COMMAND + 1] << 8;
	VFIO_ASSERT_TRUE(cmd & PCI_COMMAND_INTX_DISABLE,
			 "INTx Disable is not set (command 0x%04x)\n", cmd);
	VFIO_ASSERT_EQ(vfio_pci_config_readw(device, PCI_COMMAND), vfio_cmd);
}

/* Read a register from each mmap()ed BAR and check that it reads the same. */
static void read_bars(struct vfio_pci_device *device, u32 *vals)
{
	int i;

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (device->bars[i].vaddr)
			vals[i] = READ_ONCE(*(u32 *)device->bars[i].vaddr);
	}
}

/*
 * The device can't be frozen for kexec while it has interrupts enabled, so
 * kexec must fail and leave the device usable. This is only possible to test
 * if a kexec image is loaded (and if the test is wrong, the system kexecs).
 */
static void check_kexec_fails_with_irqs(struct vfio_pci_device *device)
{
	u32 before[PCI_STD_NUM_BARS] = {}, after[PCI_STD_NUM_BARS] = {};
	bool enabled_msix = false;

	if (!kexec_image_loaded()) {
		printf("No kexec image, not checking that kexec fails with IRQs enabled\n");
		return;
	}

	if (!device->driver.initialized) {
		if (!device->msix_info.count) {
			printf("No MSI-X, not checking that kexec fails with IRQs enabled\n");
			return;
		}

		vfio_pci_msix_enable(device, 0, 1);
		enabled_msix = true;
	}

	read_bars(device, before);

	printf("Checking that kexec fails while IRQs are enabled\n");
	VFIO_ASSERT_EQ(reboot(RB_KEXEC), -1);
	VFIO_ASSERT_EQ(errno, EBUSY);

	/* The device must be usable again after the failed kexec. */
	read_bars(device, after);
	VFIO_ASSERT_EQ(memcmp(before, after, sizeof(before)), 0);
	VFIO_ASSERT_NE(vfio_pci_config_readw(device, PCI_VENDOR_ID), 0xffff);

	if (device->driver.initialized)
		do_memcpy(device, 0x3c);

	if (enabled_msix)
		disable_msix_check_intx_masked(device);
}

/*
 * Write emulated (virtualized) fields of config space through VFIO. These only
 * change VFIO's virtual config space, which VFIO preserves across Live Update,
 * see save_vconfig().
 */
static void write_vconfig(struct vfio_pci_device *device)
{
	struct vfio_pci_bar *bar = &device->bars[0];
	u32 bar0;

	printf("Writing Interrupt Line and BAR 0 through VFIO\n");
	vfio_pci_config_writeb(device, PCI_INTERRUPT_LINE, 0x5a);
	VFIO_ASSERT_EQ(vfio_pci_config_readb(device, PCI_INTERRUPT_LINE), 0x5a);

	/* Move BAR 0 by its size, i.e. to another valid, aligned address. */
	if (bar->info.size && bar->info.size <= SZ_1G) {
		bar0 = vfio_pci_config_readl(device, PCI_BASE_ADDRESS_0);
		bar0 ^= bar->info.size;
		vfio_pci_config_writel(device, PCI_BASE_ADDRESS_0, bar0);
		VFIO_ASSERT_EQ(vfio_pci_config_readl(device, PCI_BASE_ADDRESS_0), bar0);
	}
}

/*
 * Record config space as seen through VFIO. VFIO snapshots its virtual config
 * space when it freezes the device for kexec (config space writes are refused
 * after that) and restores it when the device is opened after kexec, so it
 * must read exactly the same after kexec, including the emulated fields
 * written by write_vconfig().
 */
static void save_vconfig(struct vfio_pci_device *device)
{
	state->vconfig_len = min_t(u64, device->config_space.size, CFG_SIZE);
	vfio_pci_config_access(device, false, 0, state->vconfig_len, state->vconfig);
}

static void check_vconfig(struct vfio_pci_device *device, const struct test_state *s)
{
	int i, errors = 0;
	u8 *vconfig;

	printf("Checking that config space through VFIO is unchanged\n");

	VFIO_ASSERT_EQ(min_t(u64, device->config_space.size, CFG_SIZE), s->vconfig_len);
	vconfig = calloc_assert(1, s->vconfig_len);
	vfio_pci_config_access(device, false, 0, s->vconfig_len, vconfig);

	for (i = 0; i < s->vconfig_len; i++) {
		if (vconfig[i] == s->vconfig[i])
			continue;

		printf("%s: VFIO config[0x%03x] changed from 0x%02x to 0x%02x\n",
		       s->bdf, i, s->vconfig[i], vconfig[i]);
		errors++;
	}

	VFIO_ASSERT_EQ(errors, 0, "Config space through VFIO changed across Live Update\n");
	free(vconfig);
}

/*
 * Toggle INTx Disable through the VFIO config region and restore it, which
 * must work on a retrieved device (it only must not be written while the
 * device is frozen for kexec).
 */
static void check_intx_disable_toggle(struct vfio_pci_device *device)
{
	u16 cmd = vfio_pci_config_readw(device, PCI_COMMAND);

	printf("Checking that INTx Disable can be toggled through VFIO\n");
	vfio_pci_config_writew(device, PCI_COMMAND, cmd ^ PCI_COMMAND_INTX_DISABLE);
	VFIO_ASSERT_EQ(vfio_pci_config_readw(device, PCI_COMMAND),
		       cmd ^ PCI_COMMAND_INTX_DISABLE);
	vfio_pci_config_writew(device, PCI_COMMAND, cmd);
	VFIO_ASSERT_EQ(vfio_pci_config_readw(device, PCI_COMMAND), cmd);
}

static void check_reset_fails(struct vfio_pci_device *device)
{
	if (!(device->info.flags & VFIO_DEVICE_FLAGS_RESET))
		return;

	printf("Checking that the device can't be reset while preserved\n");
	VFIO_ASSERT_EQ(__vfio_pci_device_reset(device), -EBUSY);
}

/*
 * Bind @pf_bdf to pci-pf-stub, enable one VF and bind the VF to vfio-pci.
 * Returns the BDF of the VF.
 */
static char *setup_vf(const char *pf_bdf)
{
	char *vf_bdf;

	sysfs_driver_bind(pf_bdf, "pci-pf-stub");

	if (sysfs_sriov_numvfs_get(pf_bdf) < 1)
		sysfs_sriov_numvfs_set(pf_bdf, 1);

	vf_bdf = sysfs_sriov_vf_bdf_get(pf_bdf, 0);
	sysfs_driver_bind(vf_bdf, "vfio-pci");

	return vf_bdf;
}

static void stage1(int luo_fd, const char *bdf)
{
	struct vfio_pci_device *device;
	const char *pf_bdf = NULL;
	struct iommu *iommu;
	int session_fd;
	int memfd;

	if (vf_mode) {
		pf_bdf = bdf;
		bdf = setup_vf(pf_bdf);
	}

	if (!vfio_pci_cdev_is_noiommu(bdf))
		ksft_exit_skip("%s is not bound to vfio-pci in noiommu mode\n", bdf);

	iommu = iommu_init(MODE_IOMMUFD_NOIOMMU);
	device = vfio_pci_device_init(bdf, iommu);

	memfd = memfd_create("vfio_pci_liveupdate_kexec_test", 0);
	VFIO_ASSERT_GE(memfd, 0);
	VFIO_ASSERT_EQ(ftruncate(memfd, MEMFD_SIZE), 0);
	VFIO_ASSERT_EQ(fallocate(memfd, 0, 0, MEMFD_SIZE), 0);

	memfd_map(device, memfd);

	memset(state, 0, sizeof(*state));
	state->magic = TEST_STATE_MAGIC;
	snprintf_assert(state->bdf, sizeof(state->bdf), "%s", bdf);
	if (pf_bdf) {
		snprintf_assert(state->pf_bdf, sizeof(state->pf_bdf), "%s", pf_bdf);
		state->numvfs = sysfs_sriov_numvfs_get(pf_bdf);
	}

	state->driver_region_pa = region_pa(device, &driver_region);
	state->src_region_pa = region_pa(device, &src_region);
	state->dst_region_pa = region_pa(device, &dst_region);
	printf("Regions at IOVA 0x%lx: driver PA 0x%lx, src PA 0x%lx, dst PA 0x%lx\n",
	       driver_region.iova, state->driver_region_pa,
	       state->src_region_pa, state->dst_region_pa);

	state->has_driver = !!device->driver.ops;
	if (state->has_driver) {
		printf("Initializing the %s driver\n", device->driver.ops->name);
		vfio_pci_driver_init(device);
		do_memcpy(device, 0x5a);
	} else {
		printf("No selftests driver for %s, not doing DMA\n", bdf);
	}

	write_vconfig(device);

	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_TOKEN, 2);

	session_fd = luo_create_session(luo_fd, DEVICE_SESSION_NAME);
	VFIO_ASSERT_GE(session_fd, 0);

	printf("Preserving the memfd and %s\n", bdf);
	VFIO_ASSERT_EQ(luo_session_preserve_fd(session_fd, memfd, MEMFD_TOKEN), 0);
	VFIO_ASSERT_EQ(luo_session_preserve_fd(session_fd, device->fd, DEVICE_TOKEN), 0);

	check_reset_fails(device);
	check_intx_disable_toggle(device);
	check_kexec_fails_with_irqs(device);

	if (state->has_driver) {
		time_memcpy(device);
		start_memcpy(device, 0xa5);

		/* VFIO can't preserve interrupts, freeze fails if enabled. */
		disable_msix_check_intx_masked(device);
	}

	/* Snapshot right before kexec, after all changes to config space. */
	save_vconfig(device);
	take_snapshot(&state->device_snapshot, bdf);
	if (pf_bdf)
		take_snapshot(&state->pf_snapshot, pf_bdf);

	close(luo_fd);
	daemonize_and_wait();
}

static void check_open_fails(const char *bdf)
{
	const char *cdev_path = vfio_pci_get_cdev_path(bdf);
	int fd;

	printf("Checking that open(%s) fails\n", cdev_path);
	fd = open(cdev_path, O_RDWR);
	VFIO_ASSERT_EQ(fd, -1);
	VFIO_ASSERT_EQ(errno, EBUSY);
	free((void *)cdev_path);
}

static void check_open_works(const char *bdf)
{
	const char *cdev_path = vfio_pci_get_cdev_path(bdf);
	int fd;

	printf("Checking that open(%s) works after finishing\n", cdev_path);
	fd = open(cdev_path, O_RDWR);
	VFIO_ASSERT_GE(fd, 0);
	VFIO_ASSERT_EQ(close(fd), 0);
	free((void *)cdev_path);
}

/*
 * The device can't be preserved for the next Live Update before the session
 * it came from is finished.
 */
static void check_preserve_incoming_fails(int luo_fd, struct vfio_pci_device *device)
{
	int session_fd;

	printf("Checking that the device can't be preserved again before finishing\n");
	session_fd = luo_create_session(luo_fd, DEVICE_SESSION_NAME "_next");
	VFIO_ASSERT_GE(session_fd, 0);
	VFIO_ASSERT_EQ(luo_session_preserve_fd(session_fd, device->fd, DEVICE_TOKEN),
		       -EBUSY);
	VFIO_ASSERT_EQ(close(session_fd), 0);
}

/* Read the test state out of the memfd without mapping it into the IOAS. */
static void read_state(int memfd, struct test_state *out)
{
	void *vaddr;

	vaddr = mmap(NULL, MEMFD_SIZE, PROT_READ, MAP_SHARED, memfd, 0);
	VFIO_ASSERT_NE(vaddr, MAP_FAILED);
	memcpy(out, vaddr + TEST_STATE_OFFSET, sizeof(*out));
	VFIO_ASSERT_EQ(munmap(vaddr, MEMFD_SIZE), 0);

	VFIO_ASSERT_EQ(out->magic, TEST_STATE_MAGIC);
}

/* The PF adopts its VFs after Live Update when sriov_numvfs is written. */
static void adopt_vfs(struct test_state *s)
{
	check_driver(s->pf_bdf, "pci-pf-stub");

	printf("Adopting %d VFs of %s\n", s->numvfs, s->pf_bdf);
	sysfs_sriov_numvfs_set(s->pf_bdf, s->numvfs);
}

static void stage2(int luo_fd, int state_session_fd)
{
	struct vfio_pci_device *device;
	struct test_state *s;
	struct iommu *iommu;
	int session_fd;
	int device_fd;
	int memfd;
	int stage;

	restore_and_read_stage(state_session_fd, STATE_TOKEN, &stage);
	VFIO_ASSERT_EQ(stage, 2);

	session_fd = luo_retrieve_session(luo_fd, DEVICE_SESSION_NAME);
	VFIO_ASSERT_GE(session_fd, 0);

	printf("Retrieving the memfd\n");
	memfd = luo_session_retrieve_fd(session_fd, MEMFD_TOKEN);
	VFIO_ASSERT_GE(memfd, 0);

	s = calloc_assert(1, sizeof(*s));
	read_state(memfd, s);

	/* Check the device before anything touches it. */
	if (s->pf_bdf[0]) {
		check_snapshot(&s->pf_snapshot, "before adopting its VFs");
		adopt_vfs(s);
	}

	check_driver(s->bdf, "vfio-pci");
	check_snapshot(&s->device_snapshot, "before retrieving it");
	check_open_fails(s->bdf);

	printf("Checking that the session can't be finished before retrieving the device\n");
	VFIO_ASSERT_NE(luo_session_finish(session_fd), 0);

	printf("Retrieving %s\n", s->bdf);
	device_fd = luo_session_retrieve_fd(session_fd, DEVICE_TOKEN);
	VFIO_ASSERT_GE(device_fd, 0);

	printf("Checking that the session can't be finished before binding the device\n");
	VFIO_ASSERT_NE(luo_session_finish(session_fd), 0);

	printf("Binding %s to a new iommufd\n", s->bdf);
	iommu = iommu_init(MODE_IOMMUFD_NOIOMMU);
	device = vfio_pci_device_init_cdev(s->bdf, iommu, device_fd);

	memfd_map(device, memfd);
	VFIO_ASSERT_EQ(region_pa(device, &driver_region), s->driver_region_pa);
	VFIO_ASSERT_EQ(region_pa(device, &src_region), s->src_region_pa);
	VFIO_ASSERT_EQ(region_pa(device, &dst_region), s->dst_region_pa);

	check_snapshot(&s->device_snapshot, "after binding it");
	check_vconfig(device, s);
	check_intx_disable_toggle(device);
	if (s->pf_bdf[0])
		check_snapshot(&s->pf_snapshot, "after binding its VF");

	check_reset_fails(device);
	check_preserve_incoming_fails(luo_fd, device);

	if (s->has_driver) {
		VFIO_ASSERT_NOT_NULL(device->driver.ops);
		printf("Resuming the %s driver\n", device->driver.ops->name);
		vfio_pci_driver_resume(device, true);
		check_memcpy(device);
		do_memcpy(device, 0xc3);
		check_msi(device);
	}

	printf("Finishing the session\n");
	VFIO_ASSERT_EQ(luo_session_finish(session_fd), 0);
	VFIO_ASSERT_EQ(luo_session_finish(state_session_fd), 0);

	check_open_works(s->bdf);

	if (s->has_driver)
		do_memcpy(device, 0x96);

	iommu_unmap(iommu, &dst_region);
	iommu_unmap(iommu, &src_region);
	iommu_unmap(iommu, &driver_region);
	vfio_pci_device_cleanup(device);
	iommu_cleanup(iommu);

	VFIO_ASSERT_EQ(close(session_fd), 0);
	VFIO_ASSERT_EQ(close(state_session_fd), 0);
	free(s);

	printf("\n--- VFIO PCI LIVE UPDATE TEST PASSED ---\n");
}

static const char *device_bdf;

static void run_stage1(int luo_fd)
{
	stage1(luo_fd, device_bdf);
}

int main(int argc, char *argv[])
{
	char *luo_argv[4] = { argv[0] };
	int luo_argc = 1;
	int opt;

	/* Keep the output in order with the kernel log on the console. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* Takes the BDF out of argv. */
	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	while ((opt = getopt(argc, argv, "s:v")) != -1) {
		switch (opt) {
		case 's':
			luo_argv[luo_argc++] = "-s";
			luo_argv[luo_argc++] = optarg;
			break;
		case 'v':
			vf_mode = true;
			break;
		default:
			fprintf(stderr, "usage: %s [-s 1|2] [-v] segment:bus:device.function\n",
				argv[0]);
			return KSFT_FAIL;
		}
	}

	return luo_test(luo_argc, luo_argv, STATE_SESSION_NAME, run_stage1, stage2);
}
