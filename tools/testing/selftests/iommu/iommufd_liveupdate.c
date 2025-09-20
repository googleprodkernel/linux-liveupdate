// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Samiullah Khawaja <skhawaja@google.com>
 */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <unistd.h>

#define __EXPORTED_HEADERS__
#include <linux/liveupdate.h>
#include <linux/iommufd.h>
#include <linux/types.h>
#include <linux/vfio.h>

#include "../kselftest.h"

#define ksft_assert(condition) \
	do { if (!(condition)) \
	ksft_exit_fail_msg("Failed: %s at %s %d\n", \
	#condition, __FILE__, __LINE__); } while (0)

int setup_cdev(const char *vfio_cdev_path)
{
	int cdev_fd;

	cdev_fd = open(vfio_cdev_path, O_RDWR);
	if (cdev_fd < 0)
		ksft_exit_skip("Failed to open VFIO cdev: %s\n", vfio_cdev_path);

	return cdev_fd;
}

int open_iommufd(void)
{
	int iommufd;

	iommufd = open("/dev/iommu", O_RDWR);
	if (iommufd < 0)
		ksft_exit_skip("Failed to open /dev/iommu. IOMMUFD support not enabled.\n");

	return iommufd;
}

int setup_iommufd(int iommufd, int cdev_fd)
{
	int ret;

	struct vfio_device_bind_iommufd bind = {
		.argsz = sizeof(bind),
		.flags = 0,
	};
	struct iommu_ioas_alloc alloc_data  = {
		.size = sizeof(alloc_data),
		.flags = 0,
	};
	struct vfio_device_attach_iommufd_pt attach_data = {
		.argsz = sizeof(attach_data),
		.flags = 0,
	};

	bind.iommufd = iommufd;
	ret = ioctl(cdev_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
	ksft_assert(!ret);

	ret = ioctl(iommufd, IOMMU_IOAS_ALLOC, &alloc_data);
	ksft_assert(!ret);

	attach_data.pt_id = alloc_data.out_ioas_id;
	ret = ioctl(cdev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data);
	ksft_assert(!ret);

	return ret;
}

int open_liveupdate_orchestrator(void)
{
	int luo;

	luo = open("/dev/liveupdate", O_RDWR);
	ksft_assert(luo > 0);

	return luo;
}

__u32 liveupdate_get_state(int luo)
{
	struct liveupdate_ioctl_get_state state;
	int ret;

	state.size = sizeof(state);
	ret = ioctl(luo, LIVEUPDATE_IOCTL_GET_STATE, &state);
	ksft_assert(!ret);

	return state.state;
}

bool liveupdate_state_normal(int luo)
{
	return liveupdate_get_state(luo) == LIVEUPDATE_STATE_NORMAL;
}

bool liveupdate_state_updated(int luo)
{
	return liveupdate_get_state(luo) == LIVEUPDATE_STATE_UPDATED;
}

int liveupdate_set_event(int luo, enum liveupdate_event ev)
{
	struct liveupdate_ioctl_set_event event;
	int ret;

	event.event = ev;
	event.size = sizeof(event);

	ret = ioctl(luo, LIVEUPDATE_IOCTL_SET_EVENT, &event);
	ksft_assert(!ret);

	return ret;
}

int liveupdate_preserve_iommufd(int luo, int iommufd, int token)
{
	struct liveupdate_ioctl_fd_preserve preserve;
	int ret;

	preserve.fd = iommufd;
	preserve.token = token;
	preserve.size = sizeof(preserve);

	ret = ioctl(luo, LIVEUPDATE_IOCTL_FD_PRESERVE, &preserve);
	ksft_assert(!ret);

	return ret;
}

int liveupdate_restore_iommufd(int luo, int token)
{
	struct liveupdate_ioctl_fd_restore restore;
	int ret;

	restore.token = token;
	restore.size = sizeof(restore);

	ret = ioctl(luo, LIVEUPDATE_IOCTL_FD_RESTORE, &restore);
	ksft_assert(!ret);
	ksft_assert(restore.fd > 0);

	return restore.fd;
}

int main(int argc, char *argv[])
{
	int iommufd, cdev_fd, luo, ret;
	const int token = 0x123456;

	if (argc < 2) {
		printf("Usage: ./iommufd_liveupdate <vfio_cdev_path>\n");
		return 1;
	}

	cdev_fd = setup_cdev(argv[1]);

	luo = open_liveupdate_orchestrator();
	ksft_assert(luo > 0);

	if (liveupdate_state_normal(luo))
		iommufd = open_iommufd();
	else if (liveupdate_state_updated(luo))
		iommufd = liveupdate_restore_iommufd(luo, token);
	else
		ksft_exit_fail_msg("Test can only run when LUO state is normal or updated");

	ret = setup_iommufd(iommufd, cdev_fd);
	ksft_assert(!ret);

	if (liveupdate_state_normal(luo)) {
		ret = liveupdate_preserve_iommufd(luo, iommufd, token);
		ksft_assert(!ret);

		ret = liveupdate_set_event(luo, LIVEUPDATE_PREPARE);
		ksft_assert(!ret);

		while (1)
			sleep(5);
	} else {
		ret = liveupdate_set_event(luo, LIVEUPDATE_FINISH);
		ksft_assert(!ret);
	}

	return 0;
}

