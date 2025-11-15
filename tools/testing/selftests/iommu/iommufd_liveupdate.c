// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Samiullah Khawaja <skhawaja@google.com>
 */

#include "asm-generic/errno-base.h"
#include "linux/liveupdate.h"
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

int setup_iommufd(int iommufd, int cdev_fd, int hwpt_token)
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
	struct iommu_hwpt_lu_set_preserved set_preserved = {
		.size = sizeof(set_preserved),
		.hwpt_token = hwpt_token,
		.preserved = 1,
	};

	bind.iommufd = iommufd;
	ret = ioctl(cdev_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
	ksft_assert(!ret);

	ret = ioctl(iommufd, IOMMU_IOAS_ALLOC, &alloc_data);
	ksft_assert(!ret);

	attach_data.pt_id = alloc_data.out_ioas_id;
	ret = ioctl(cdev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data);
	ksft_assert(!ret);

	set_preserved.hwpt_id = attach_data.pt_id;
	ret = ioctl(iommufd, IOMMU_HWPT_LU_SET_PRESERVED, &set_preserved);
	ksft_assert(!ret);

	return ret;
}

int restore_iommufd(int iommufd, int cdev_fd, int hwpt_token)
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
	struct iommu_hwpt_lu_restore restore = {
		.size = sizeof(restore),
		.hwpt_token = hwpt_token,
		.hwpt_alloc_flags = 0,
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

	restore.pt_id = alloc_data.out_ioas_id;
	ret = ioctl(iommufd, IOMMU_HWPT_LU_RESTORE, &restore);
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

int luo_session_finish(int session_fd)
{
	struct liveupdate_session_finish arg = { .size = sizeof(arg) };

	if (ioctl(session_fd, LIVEUPDATE_SESSION_FINISH, &arg) < 0)
		return -errno;

	return 0;
}

int luo_create_session(int luo_fd, const char *name)
{
	struct liveupdate_ioctl_create_session arg = { .size = sizeof(arg) };
	int ret;

	snprintf((char *)arg.name, LIVEUPDATE_SESSION_NAME_LENGTH, "%.*s",
		 LIVEUPDATE_SESSION_NAME_LENGTH - 1, name);
	ret = ioctl(luo_fd, LIVEUPDATE_IOCTL_CREATE_SESSION, &arg);
	ksft_assert(!ret);
	ksft_assert(arg.fd > 0);

	return arg.fd;
}

int luo_retrieve_session(int luo_fd, const char *name)
{
	struct liveupdate_ioctl_retrieve_session arg = { .size = sizeof(arg) };
	int ret;

	snprintf((char *)arg.name, LIVEUPDATE_SESSION_NAME_LENGTH, "%.*s",
		 LIVEUPDATE_SESSION_NAME_LENGTH - 1, name);
	ret = ioctl(luo_fd, LIVEUPDATE_IOCTL_RETRIEVE_SESSION, &arg);
	ksft_assert(!ret || errno == ENOENT);

	if (ret && errno == ENOENT)
		return -errno;

	return arg.fd;
}

int liveupdate_preserve_iommufd(int session_fd, int iommufd, int token)
{
	struct liveupdate_session_preserve_fd preserve;
	int ret;

	preserve.fd = iommufd;
	preserve.token = token;
	preserve.size = sizeof(preserve);

	ret = ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, &preserve);
	perror ("pre\n");
	ksft_assert(!ret);

	return ret;
}

int liveupdate_restore_iommufd(int session_fd, int token)
{
	struct liveupdate_session_retrieve_fd arg = { .size = sizeof(arg) };
	int ret;

	arg.token = token;

	ret = ioctl(session_fd, LIVEUPDATE_SESSION_RETRIEVE_FD, &arg);
	ksft_assert(!ret);
	ksft_assert(arg.fd > 0);

	return arg.fd;
}

int main(int argc, char *argv[])
{
	int iommufd, cdev_fd, luo, session, ret;
	const int token = 0x123456;
	const int hwpt_token = 0x789012;
	bool updated = false;

	if (argc < 2) {
		printf("Usage: ./iommufd_liveupdate <vfio_cdev_path>\n");
		return 1;
	}

	cdev_fd = setup_cdev(argv[1]);

	luo = open_liveupdate_orchestrator();
	ksft_assert(luo > 0);

	session = luo_retrieve_session(luo, "iommufd-test");
	if (session == -ENOENT) {
		session = luo_create_session(luo, "iommufd-test");
		iommufd = open_iommufd();
	} else {
		updated = true;
		iommufd = liveupdate_restore_iommufd(session, token);
	}

	if (!updated) {
		ret = setup_iommufd(iommufd, cdev_fd, hwpt_token);
		ksft_assert(!ret);
	} else {
		ret = restore_iommufd(iommufd, cdev_fd, hwpt_token);
		ksft_assert(!ret);
	}

	if (!updated) {
		ret = liveupdate_preserve_iommufd(session, iommufd, token);
		ksft_assert(!ret);

		while (1)
			sleep(5);
	} else {
		luo_session_finish(session);
	}

	return 0;
}
