// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test the uAPI for preserving VFIO PCI device files across Live Update,
 * without actually doing a Live Update (see vfio_pci_liveupdate_kexec_test).
 *
 * A VFIO cdev file can be preserved in a LUO session once it is bound to an
 * iommufd. While it is preserved, userspace must not do anything that would
 * disrupt the device during the Live Update, so resetting the device and
 * putting it into a low power state are refused with -EBUSY. Closing the
 * session unpreserves the file and lifts these restrictions.
 *
 * The rules for freezing the device at kexec (interrupts must be disabled)
 * can only be tested with a kexec image loaded, and are tested by
 * vfio_pci_liveupdate_kexec_test instead.
 */
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/vfio.h>

#include <libliveupdate.h>
#include <libvfio.h>

#include "kselftest_harness.h"

#define SESSION_NAME "vfio_pci_liveupdate_uapi_test"
#define DEVICE_TOKEN 0x1234

static const char *device_bdf;

static int device_feature(int fd, u32 flags, void *data, size_t size)
{
	u8 buf[sizeof(struct vfio_device_feature) + 64] = {};
	struct vfio_device_feature *feature = (void *)buf;

	VFIO_ASSERT_LE(size, sizeof(buf) - sizeof(*feature));

	feature->argsz = sizeof(*feature) + size;
	feature->flags = flags;
	memcpy(feature->data, data, size);

	if (ioctl(fd, VFIO_DEVICE_FEATURE, feature))
		return -errno;

	return 0;
}

static bool low_power_supported(int fd)
{
	return !device_feature(fd, VFIO_DEVICE_FEATURE_PROBE |
				   VFIO_DEVICE_FEATURE_SET |
				   VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY, NULL, 0);
}

static int low_power_entry(int fd)
{
	return device_feature(fd, VFIO_DEVICE_FEATURE_SET |
				  VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY, NULL, 0);
}

static int low_power_entry_with_wakeup(int fd, int eventfd)
{
	struct vfio_device_low_power_entry_with_wakeup entry = {
		.wakeup_eventfd = eventfd,
	};

	return device_feature(fd, VFIO_DEVICE_FEATURE_SET |
				  VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY_WITH_WAKEUP,
			      &entry, sizeof(entry));
}

static int low_power_exit(int fd)
{
	return device_feature(fd, VFIO_DEVICE_FEATURE_SET |
				  VFIO_DEVICE_FEATURE_LOW_POWER_EXIT, NULL, 0);
}

static int hot_reset(int fd)
{
	struct vfio_pci_hot_reset reset = {
		.argsz = sizeof(reset),
	};

	/* An empty group array means "use the iommufd of the cdev". */
	if (ioctl(fd, VFIO_DEVICE_PCI_HOT_RESET, &reset))
		return -errno;

	return 0;
}

static int config_writew(struct vfio_pci_device *device, size_t offset, u16 val)
{
	return __vfio_pci_config_access(device, true, offset, sizeof(val), &val);
}

/* Toggle INTx Disable through the VFIO config region, and restore it. */
static void toggle_intx_disable(struct vfio_pci_device *device)
{
	u16 cmd = vfio_pci_config_readw(device, PCI_COMMAND);

	VFIO_ASSERT_EQ(config_writew(device, PCI_COMMAND, cmd ^ PCI_COMMAND_INTX_DISABLE), 0);
	VFIO_ASSERT_EQ(vfio_pci_config_readw(device, PCI_COMMAND),
		       cmd ^ PCI_COMMAND_INTX_DISABLE);

	VFIO_ASSERT_EQ(config_writew(device, PCI_COMMAND, cmd), 0);
	VFIO_ASSERT_EQ(vfio_pci_config_readw(device, PCI_COMMAND), cmd);
}

/* Find a standard capability through the VFIO config region. */
static u8 find_cap(struct vfio_pci_device *device, u8 id)
{
	u8 pos = vfio_pci_config_readb(device, PCI_CAPABILITY_LIST);
	int ttl = 48;

	while (pos >= PCI_STD_HEADER_SIZEOF && ttl--) {
		pos &= ~3;
		if (vfio_pci_config_readb(device, pos + PCI_CAP_LIST_ID) == id)
			return pos;

		pos = vfio_pci_config_readb(device, pos + PCI_CAP_LIST_NEXT);
	}

	return 0;
}

FIXTURE(vfio_pci_liveupdate_uapi_test) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
	int luo_fd;
	int session_fd;
};

FIXTURE_SETUP(vfio_pci_liveupdate_uapi_test)
{
	self->luo_fd = luo_open_device();
	if (self->luo_fd < 0)
		SKIP(return, "Failed to open " LUO_DEVICE "\n");

	self->iommu = iommu_init(vfio_pci_default_iommu_mode(device_bdf));
	self->device = vfio_pci_device_init(device_bdf, self->iommu);

	self->session_fd = luo_create_session(self->luo_fd, SESSION_NAME);
	ASSERT_GE(self->session_fd, 0);
}

FIXTURE_TEARDOWN(vfio_pci_liveupdate_uapi_test)
{
	if (self->session_fd >= 0)
		ASSERT_EQ(0, close(self->session_fd));

	vfio_pci_device_cleanup(self->device);
	iommu_cleanup(self->iommu);
	ASSERT_EQ(0, close(self->luo_fd));
}

static void unpreserve(int *session_fd)
{
	/* Closing the session unpreserves all files in it. */
	VFIO_ASSERT_EQ(close(*session_fd), 0);
	*session_fd = -1;
}

TEST_F(vfio_pci_liveupdate_uapi_test, preserve_unpreserve)
{
	int i;

	for (i = 0; i < 3; i++) {
		ASSERT_EQ(0, luo_session_preserve_fd(self->session_fd,
						     self->device->fd,
						     DEVICE_TOKEN));

		/* The same file can't be preserved twice. */
		ASSERT_EQ(-EBUSY, luo_session_preserve_fd(self->session_fd,
						     self->device->fd,
						     DEVICE_TOKEN + 1));

		unpreserve(&self->session_fd);

		self->session_fd = luo_create_session(self->luo_fd, SESSION_NAME);
		ASSERT_GE(self->session_fd, 0);
	}

	/* The device must be fully usable again after unpreserving. */
	ASSERT_NE(0xffff, vfio_pci_config_readw(self->device, PCI_VENDOR_ID));
}

TEST_F(vfio_pci_liveupdate_uapi_test, preserve_unbound_cdev)
{
	const char *cdev_path = vfio_pci_get_cdev_path(device_bdf);
	int fd;

	/*
	 * A second file for the same device that is not bound to an iommufd
	 * must not be preservable, even though the device is in use through
	 * self->device->fd: Nothing would keep the device open if
	 * self->device->fd was closed.
	 */
	fd = open(cdev_path, O_RDWR);
	ASSERT_GE(fd, 0);

	ASSERT_NE(0, luo_session_preserve_fd(self->session_fd, fd, DEVICE_TOKEN));

	ASSERT_EQ(0, close(fd));
	free((void *)cdev_path);
}

TEST_F(vfio_pci_liveupdate_uapi_test, reset_while_preserved)
{
	if (!(self->device->info.flags & VFIO_DEVICE_FLAGS_RESET))
		SKIP(return, "Device does not support VFIO_DEVICE_RESET\n");

	ASSERT_EQ(0, luo_session_preserve_fd(self->session_fd, self->device->fd,
					     DEVICE_TOKEN));

	ASSERT_EQ(-EBUSY, __vfio_pci_device_reset(self->device));

	unpreserve(&self->session_fd);
	vfio_pci_device_reset(self->device);
}

TEST_F(vfio_pci_liveupdate_uapi_test, hot_reset_while_preserved)
{
	int ret;

	/*
	 * Hot reset may not be possible at all (e.g. because of other devices
	 * on the bus), but it must be refused because the device is preserved
	 * either way.
	 */
	ret = hot_reset(self->device->fd);
	if (ret)
		printf("VFIO_DEVICE_PCI_HOT_RESET without Live Update: %s\n",
		       strerror(-ret));

	ASSERT_EQ(0, luo_session_preserve_fd(self->session_fd, self->device->fd,
					     DEVICE_TOKEN));

	ASSERT_EQ(-EBUSY, hot_reset(self->device->fd));

	unpreserve(&self->session_fd);
	ASSERT_EQ(ret, hot_reset(self->device->fd));
}

TEST_F(vfio_pci_liveupdate_uapi_test, low_power_while_preserved)
{
	int wakeup_fd;

	if (!low_power_supported(self->device->fd))
		SKIP(return, "Device does not support low power entry\n");

	wakeup_fd = eventfd(0, 0);
	ASSERT_GE(wakeup_fd, 0);

	ASSERT_EQ(0, luo_session_preserve_fd(self->session_fd, self->device->fd,
					     DEVICE_TOKEN));

	ASSERT_EQ(-EBUSY, low_power_entry(self->device->fd));
	ASSERT_EQ(-EBUSY, low_power_entry_with_wakeup(self->device->fd, wakeup_fd));

	/* The device must still be usable, i.e. not in a low power state. */
	ASSERT_NE(0xffff, vfio_pci_config_readw(self->device, PCI_VENDOR_ID));

	unpreserve(&self->session_fd);

	ASSERT_EQ(0, low_power_entry(self->device->fd));
	ASSERT_EQ(0, low_power_exit(self->device->fd));

	ASSERT_EQ(0, close(wakeup_fd));
}

/*
 * Writing INTx Disable through VFIO must work whether or not the device is
 * preserved (it only must not be written while the device is frozen for
 * kexec, which can't be tested from userspace).
 */
TEST_F(vfio_pci_liveupdate_uapi_test, toggle_intx_disable)
{
	toggle_intx_disable(self->device);

	ASSERT_EQ(0, luo_session_preserve_fd(self->session_fd, self->device->fd,
					     DEVICE_TOKEN));

	toggle_intx_disable(self->device);
}

/* An FLR through config space is refused like any other reset. */
TEST_F(vfio_pci_liveupdate_uapi_test, flr_while_preserved)
{
	u8 pos = find_cap(self->device, PCI_CAP_ID_EXP);
	u16 devctl;

	if (!pos || !(vfio_pci_config_readl(self->device, pos + PCI_EXP_DEVCAP) &
		      PCI_EXP_DEVCAP_FLR))
		SKIP(return, "Device does not support PCIe FLR\n");

	ASSERT_EQ(0, luo_session_preserve_fd(self->session_fd, self->device->fd,
					     DEVICE_TOKEN));

	devctl = vfio_pci_config_readw(self->device, pos + PCI_EXP_DEVCTL);
	ASSERT_EQ(-EBUSY, config_writew(self->device, pos + PCI_EXP_DEVCTL,
					devctl | PCI_EXP_DEVCTL_BCR_FLR));

	/* The device must still be usable, i.e. not have been reset. */
	ASSERT_NE(0xffff, vfio_pci_config_readw(self->device, PCI_VENDOR_ID));
	ASSERT_EQ(devctl, vfio_pci_config_readw(self->device, pos + PCI_EXP_DEVCTL));
}

/* A cdev that has never been bound to an iommufd can't be preserved. */
TEST(preserve_never_bound_cdev)
{
	const char *cdev_path = vfio_pci_get_cdev_path(device_bdf);
	int luo_fd, session_fd, fd;

	luo_fd = luo_open_device();
	if (luo_fd < 0)
		SKIP(return, "Failed to open " LUO_DEVICE "\n");

	session_fd = luo_create_session(luo_fd, SESSION_NAME);
	ASSERT_GE(session_fd, 0);

	fd = open(cdev_path, O_RDWR);
	ASSERT_GE(fd, 0);

	ASSERT_NE(0, luo_session_preserve_fd(session_fd, fd, DEVICE_TOKEN));

	ASSERT_EQ(0, close(fd));
	ASSERT_EQ(0, close(session_fd));
	ASSERT_EQ(0, close(luo_fd));
	free((void *)cdev_path);
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}
