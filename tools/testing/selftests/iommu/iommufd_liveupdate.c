// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025, Google LLC.
 * Samiullah Khawaja <skhawaja@google.com>
 */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#define __EXPORTED_HEADERS__
#include <linux/liveupdate.h>
#include <linux/iommufd.h>
#include <linux/types.h>
#include <linux/vfio.h>

#include "../kselftest.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE getpagesize()
#endif

#define ksft_assert(condition) \
	do { if (!(condition)) \
	ksft_exit_fail_msg("Failed: %s at %s %d: %s\n", \
	#condition, __FILE__, __LINE__, strerror(errno)); } while (0)

static int setup_cdev(const char *vfio_cdev_path)
{
	int cdev_fd;

	cdev_fd = open(vfio_cdev_path, O_RDWR);
	if (cdev_fd < 0)
		ksft_exit_skip("Failed to open VFIO cdev: %s\n", vfio_cdev_path);

	return cdev_fd;
}

static int open_iommufd(void)
{
	int iommufd;

	iommufd = open("/dev/iommu", O_RDWR);
	if (iommufd < 0)
		ksft_exit_skip("Failed to open /dev/iommu. IOMMUFD support not enabled.\n");

	return iommufd;
}

static void setup_iommufd(int iommufd, int cdev_fd, int memfd, int hwpt_token,
			  __u32 *ioas_id)
{
	void *va;
	int ret;

	struct vfio_device_bind_iommufd bind = {
		.argsz = sizeof(bind),
		.flags = 0,
	};
	struct iommu_ioas_alloc alloc_data = {
		.size = sizeof(alloc_data),
		.flags = 0,
	};
	struct iommu_hwpt_alloc hwpt_alloc = {
		.size = sizeof(hwpt_alloc),
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
	struct iommu_ioas_map map = {
		.size = sizeof(map),
		.flags = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE |
			 IOMMU_IOAS_MAP_FIXED_IOVA,
		.iova = 0,
		.length = PAGE_SIZE,
	};

	bind.iommufd = iommufd;
	ret = ioctl(cdev_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
	ksft_assert(!ret);

	ret = ioctl(iommufd, IOMMU_IOAS_ALLOC, &alloc_data);
	ksft_assert(!ret);

	hwpt_alloc.dev_id = bind.out_devid;
	hwpt_alloc.pt_id = alloc_data.out_ioas_id;
	ret = ioctl(iommufd, IOMMU_HWPT_ALLOC, &hwpt_alloc);
	ksft_assert(!ret);

	attach_data.pt_id = hwpt_alloc.pt_id;
	ret = ioctl(cdev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data);
	ksft_assert(!ret);

	set_preserved.hwpt_id = attach_data.pt_id;
	ret = ioctl(iommufd, IOMMU_HWPT_LU_SET_PRESERVED, &set_preserved);
	ksft_assert(!ret);

	va = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
	ksft_assert(va != MAP_FAILED);

	map.user_va = (uintptr_t)va;
	map.ioas_id = alloc_data.out_ioas_id;
	ret = ioctl(iommufd, IOMMU_IOAS_MAP, &map);
	ksft_assert(!ret);

	*ioas_id = alloc_data.out_ioas_id;
}

static void verify_immutable(int iommufd, int memfd, __u32 *ioas_id)
{
	int ret;

	struct iommu_ioas_unmap unmap = {
		.size = sizeof(unmap),
		.iova = 0,
		.length = PAGE_SIZE,
	};

	if (ioas_id) {
		/* IOAS mappings are immutable after preserve */
		unmap.ioas_id = *ioas_id;
		ret = ioctl(iommufd, IOMMU_IOAS_UNMAP, &unmap);
		ksft_assert(ret);
	}

	/* memfd size is immutable after preserve and before finish */
	ret = ftruncate(memfd, PAGE_SIZE * 2);
	ksft_assert(ret);
}

static int luo_session_finish(int session_fd)
{
	struct liveupdate_session_finish arg = { .size = sizeof(arg) };

	if (ioctl(session_fd, LIVEUPDATE_SESSION_FINISH, &arg) < 0)
		return -errno;

	return 0;
}

static void restore_iommufd(int session, int iommufd, int cdev_fd, int hwpt_token)
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
	struct iommu_hwpt_alloc hwpt_alloc = {
		.size = sizeof(hwpt_alloc),
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

	ret = ioctl(iommufd, IOMMU_HWPT_LU_RESTORE, &restore);
	ksft_assert(!ret);

	/* Should fail */
	ret = luo_session_finish(session);
	ksft_assert(ret);

	hwpt_alloc.dev_id = bind.out_devid;
	hwpt_alloc.pt_id = alloc_data.out_ioas_id;
	ret = ioctl(iommufd, IOMMU_HWPT_ALLOC, &hwpt_alloc);
	ksft_assert(!ret);

	attach_data.pt_id = hwpt_alloc.pt_id;
	ret = ioctl(cdev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data);
	ksft_assert(!ret);
	attach_data.pt_id = alloc_data.out_ioas_id;
	ret = ioctl(cdev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data);
	ksft_assert(!ret);
}

static int open_liveupdate_orchestrator(void)
{
	int luo;

	luo = open("/dev/liveupdate", O_RDWR);
	ksft_assert(luo > 0);

	return luo;
}

static int luo_create_session(int luo_fd, const char *name)
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

static int liveupdate_preserve_fd(int session_fd, int fd, int token)
{
	struct liveupdate_session_preserve_fd preserve;
	int ret;

	preserve.fd = fd;
	preserve.token = token;
	preserve.size = sizeof(preserve);

	ret = ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, &preserve);
	ksft_assert(!ret);

	return ret;
}

static int liveupdate_restore_fd(int session_fd, int token)
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
	int iommufd, cdev_fd, memfd, luo, session, ret;
	const int token = 0x123456;
	const int cdev_token = 0x654321;
	const int hwpt_token = 0x789012;
	const int memfd_token = 0x210987;
	bool updated = false;
	__u32 ioas_id;

	if (argc < 2) {
		printf("Usage: ./iommufd_liveupdate <vfio_cdev_path>\n");
		return 1;
	}

	luo = open_liveupdate_orchestrator();
	ksft_assert(luo > 0);

	session = luo_retrieve_session(luo, "iommufd-test");
	if (session == -ENOENT) {
		session = luo_create_session(luo, "iommufd-test");

		iommufd = open_iommufd();
		cdev_fd = setup_cdev(argv[1]);
		memfd = memfd_create("iommufd-test", MFD_CLOEXEC);

		ret = ftruncate(memfd, PAGE_SIZE);
		ksft_assert(!ret);
	} else {
		updated = true;

		/* Finish cannot happen without iommufd retrieved */
		ret = luo_session_finish(session);
		ksft_assert(ret);

		iommufd = liveupdate_restore_fd(session, token);
		cdev_fd = liveupdate_restore_fd(session, cdev_token);
		memfd = liveupdate_restore_fd(session, memfd_token);
	}

	if (!updated) {
		setup_iommufd(iommufd, cdev_fd, memfd, hwpt_token, &ioas_id);
	} else {
		/* Finish cannot happen without HWPT reclaimed */
		ret = luo_session_finish(session);
		ksft_assert(ret);

		restore_iommufd(session, iommufd, cdev_fd, hwpt_token);
	}

	if (!updated) {
		ret = liveupdate_preserve_fd(session, iommufd, token);
		ksft_assert(!ret);

		ret = liveupdate_preserve_fd(session, cdev_fd, cdev_token);
		ksft_assert(!ret);

		ret = liveupdate_preserve_fd(session, memfd, memfd_token);
		ksft_assert(!ret);

		verify_immutable(iommufd, memfd, &ioas_id);

		while (1)
			sleep(5);
	} else {
		verify_immutable(iommufd, memfd, NULL);

		ret = luo_session_finish(session);
		ksft_assert(!ret);
	}

	return 0;
}
