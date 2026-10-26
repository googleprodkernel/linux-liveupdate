// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2026, Google LLC.
 *
 * Test that PCI devices preserved across a kexec Live Update are left alone
 * by the next kernel, using the pci_lu_test module (test_modules/) to preserve
 * them.
 *
 * Usage: luo_pci [-s 1|2] [DEVICE...]
 *
 * DEVICE is a BDF (e.g. 0000:01:00.0) or VENDOR:DEVICE IDs (e.g. 8086:10c9),
 * which selects every device with those IDs. Without DEVICEs, every QEMU
 * device commonly used for testing (see default_ids[]) is selected, which is
 * how vmtest.sh runs this test. If a selected device is an SR-IOV PF, up to
 * DEFAULT_NUMVFS of its VFs are enabled (unless some already are) and
 * preserved too.
 *
 * Stage 1 binds each device to pci-lu-test, snapshots the configuration space
 * and resources of the device and of every bridge upstream of it, and
 * preserves each device in its own LUO session. Stage 2 (after kexec) checks
 * that every snapshotted device looks exactly like it did before kexec, then
 * binds each device to pci-lu-test, adopts the VFs of PFs, retrieves the
 * devices, and checks again. Stage 2 takes the list of devices from the
 * snapshot, so DEVICE arguments are ignored.
 *
 * With LUO_PCI_AUTO_PF (luo_pci_auto_pf), PFs with VFs are not preserved
 * explicitly, only their VFs, and the PCI core preserves the PFs automatically
 * since pci-lu-test supports Live Update as a PF driver.
 *
 * This test must build with nolibc (see vmtest.sh).
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/pci_regs.h>

#include <libliveupdate.h>

#include "pci_lu_test_uapi.h"

#define STATE_SESSION_NAME	"luo_pci_state"
#define STATE_MEMFD_TOKEN	999
#define SNAPSHOT_SESSION_NAME	"luo_pci_snapshot"
#define SNAPSHOT_MEMFD_TOKEN	1000
#define DEVICE_TOKEN		0x5043

#define DRIVER			"pci-lu-test"
#define SYSFS_PCI		"/sys/bus/pci"
#define LU_PATH_MAX		512
#define BDF_LEN			16
#define MAX_DEVICES		32
#define MAX_NODES		(MAX_DEVICES * 4)
#define CFG_SIZE		PCI_CFG_SPACE_EXP_SIZE
#define RESOURCE_SIZE		2048
#define DEFAULT_NUMVFS		2

/* QEMU devices selected when no DEVICE is given. */
static const char * const default_ids[] = {
	"1b36:0005",	/* pci-testdev */
	"8086:10c9",	/* igb (SR-IOV PF) */
	"8086:10d3",	/* e1000e */
};

/* Snapshot of one device: a preserved device or a bridge upstream of one. */
struct node {
	char bdf[BDF_LEN];
	int numvfs;
	int cfg_len;
	unsigned char cfg[CFG_SIZE];
	char resource[RESOURCE_SIZE];
};

struct snapshot {
	int nr_devices;
	int nr_nodes;
	char devices[MAX_DEVICES][BDF_LEN];
	/* PFs that are not preserved explicitly (LUO_PCI_AUTO_PF). */
	bool auto_pf[MAX_DEVICES];
	struct node nodes[MAX_NODES];
};

static char devices[MAX_DEVICES][BDF_LEN];
static int nr_devices;

static int read_file(const char *path, void *buf, int len)
{
	int fd, ret, total = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	while (total < len) {
		ret = read(fd, (char *)buf + total, len - total);
		if (ret < 0) {
			ret = -errno;
			close(fd);
			return ret;
		}
		if (!ret)
			break;
		total += ret;
	}

	close(fd);
	return total;
}

static void write_file(const char *path, const char *val)
{
	int fd;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		fail_exit("open(%s)", path);

	if (write(fd, val, strlen(val)) != strlen(val))
		fail_exit("write(%s, %s)", path, val);

	close(fd);
}

/* Copy the string @src into @dst of size @len, which it must fit in. */
static void copy_str(char *dst, size_t len, const char *src)
{
	size_t n = strlen(src);

	if (n >= len)
		fail_exit("String too long: %s", src);

	memcpy(dst, src, n + 1);
}

static void dev_path(char *buf, const char *bdf, const char *file)
{
	snprintf(buf, LU_PATH_MAX, SYSFS_PCI "/devices/%s/%s", bdf, file);
}

static int read_dev_int(const char *bdf, const char *file)
{
	char path[LU_PATH_MAX], buf[32] = {};

	dev_path(path, bdf, file);
	if (read_file(path, buf, sizeof(buf) - 1) < 0)
		return -1;

	return strtoul(buf, NULL, 0);
}

/* Return the last path component of the target of the symlink @path. */
static bool read_link_name(const char *path, char *name, size_t len)
{
	char link[LU_PATH_MAX];
	const char *base;
	ssize_t ret;

	ret = readlink(path, link, sizeof(link) - 1);
	if (ret <= 0)
		return false;

	link[ret] = '\0';
	base = strrchr(link, '/');
	copy_str(name, len, base ? base + 1 : link);
	return true;
}

static bool is_hex(const char *s, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
		      (s[i] >= 'A' && s[i] <= 'F')))
			return false;
	}

	return true;
}

/* Match "DDDD:BB:DD.F" exactly, e.g. 0000:01:00.0. */
static bool is_bdf(const char *s, size_t len)
{
	return len == 12 && is_hex(s, 4) && s[4] == ':' && is_hex(s + 5, 2) &&
	       s[7] == ':' && is_hex(s + 8, 2) && s[10] == '.' &&
	       s[11] >= '0' && s[11] <= '7';
}

/* Match "VVVV:DDDD", e.g. 8086:10c9. */
static bool is_id(const char *s)
{
	return strlen(s) == 9 && is_hex(s, 4) && s[4] == ':' && is_hex(s + 5, 4);
}

static void bind_device(const char *bdf)
{
	char path[LU_PATH_MAX], name[LU_PATH_MAX];

	dev_path(path, bdf, "driver");
	if (read_link_name(path, name, sizeof(name))) {
		if (!strcmp(name, DRIVER))
			return;

		dev_path(path, bdf, "driver/unbind");
		write_file(path, bdf);
	}

	dev_path(path, bdf, "driver_override");
	write_file(path, DRIVER);
	write_file(SYSFS_PCI "/drivers_probe", bdf);

	dev_path(path, bdf, "driver");
	if (!read_link_name(path, name, sizeof(name)))
		fail_exit("%s not bound to " DRIVER " after probe", bdf);

	if (strcmp(name, DRIVER))
		fail_exit("%s bound to %s instead of " DRIVER, bdf, name);
}

/*
 * Check that the kernel bound @bdf to pci-lu-test on its own, since the
 * previous kernel passed the name of the driver the device was bound to.
 */
static int check_bound(const char *bdf)
{
	char path[LU_PATH_MAX], name[LU_PATH_MAX];

	dev_path(path, bdf, "driver");
	if (read_link_name(path, name, sizeof(name)) && !strcmp(name, DRIVER))
		return 0;

	ksft_print_msg("%s: not bound to " DRIVER " automatically\n", bdf);
	return 1;
}

/* Check that driver_override of @bdf is cleared, i.e. reads "(null)". */
static int check_no_override(const char *bdf)
{
	char path[LU_PATH_MAX], buf[64] = {};

	dev_path(path, bdf, "driver_override");
	if (read_file(path, buf, sizeof(buf) - 1) < 0)
		fail_exit("Failed to read %s", path);

	if (!strncmp(buf, "(null)", 6))
		return 0;

	ksft_print_msg("%s: driver_override is still set (%s)\n", bdf, buf);
	return 1;
}

/* Check that writing @val to @path fails with @expected_errno. */
static int check_write_fails(const char *path, const char *val,
			     int expected_errno)
{
	ssize_t ret;
	int fd;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		fail_exit("open(%s)", path);

	ret = write(fd, val, strlen(val));
	close(fd);

	if (ret < 0 && errno == expected_errno)
		return 0;

	ksft_print_msg("Writing %s to %s returned %zd (errno %d), expected errno %d\n",
		       val, path, ret, ret < 0 ? errno : 0, expected_errno);
	return 1;
}

/*
 * Check that the kernel refuses to unbind, remove or reset the preserved
 * device @bdf via sysfs.
 */
static int check_guards(const char *bdf)
{
	char path[LU_PATH_MAX];
	int errors = 0;

	ksft_print_msg("Checking that %s can't be unbound, removed or reset\n", bdf);

	dev_path(path, bdf, "driver/unbind");
	errors += check_write_fails(path, bdf, EBUSY);

	/* VFs can't be removed on their own, so have no remove attribute. */
	dev_path(path, bdf, "remove");
	if (!access(path, F_OK))
		errors += check_write_fails(path, "1", EBUSY);

	/*
	 * Only devices that support reset have a reset attribute. PFs with
	 * VFs enabled refuse function resets regardless (-ENOTTY).
	 */
	dev_path(path, bdf, "reset");
	if (!access(path, F_OK))
		errors += check_write_fails(path, "1",
					    read_dev_int(bdf, "sriov_numvfs") > 0 ?
					    ENOTTY : EBUSY);

	return errors;
}

static bool is_selected(const char *bdf)
{
	int i;

	for (i = 0; i < nr_devices; i++) {
		if (!strcmp(devices[i], bdf))
			return true;
	}

	return false;
}

static void select_device(const char *bdf)
{
	if (is_selected(bdf))
		return;

	if (nr_devices == MAX_DEVICES)
		fail_exit("Too many devices");

	copy_str(devices[nr_devices++], BDF_LEN, bdf);
}

/* Select a device and, if it is an SR-IOV PF, its VFs. */
static void select_device_and_vfs(const char *bdf)
{
	char path[LU_PATH_MAX], vf[BDF_LEN], val[32];
	int i, numvfs, totalvfs;

	select_device(bdf);

	totalvfs = read_dev_int(bdf, "sriov_totalvfs");
	if (totalvfs <= 0)
		return;

	/* The VFs are enabled by the PF's driver. */
	bind_device(bdf);

	numvfs = read_dev_int(bdf, "sriov_numvfs");
	if (!numvfs) {
		numvfs = totalvfs < DEFAULT_NUMVFS ? totalvfs : DEFAULT_NUMVFS;
		ksft_print_msg("[STAGE 1] Enabling %d VFs on %s\n", numvfs, bdf);
		snprintf(val, sizeof(val), "%d", numvfs);
		dev_path(path, bdf, "sriov_numvfs");
		write_file(path, val);
	}

	for (i = 0; i < numvfs; i++) {
		snprintf(val, sizeof(val), "virtfn%d", i);
		dev_path(path, bdf, val);
		if (!read_link_name(path, vf, sizeof(vf)))
			fail_exit("No %s for %s", val, bdf);

		select_device(vf);
	}
}

/*
 * nolibc only has readdir_r(), which glibc deprecates in favor of readdir(),
 * which nolibc does not have.
 */
static int next_dirent(DIR *dir, struct dirent *entry, struct dirent **result)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	return readdir_r(dir, entry, result);
#pragma GCC diagnostic pop
}

/* Select every device with the VENDOR:DEVICE IDs @id, in BDF order. */
static void select_devices_by_id(const char *id)
{
	char found[MAX_DEVICES][BDF_LEN], tmp[BDF_LEN];
	char ids[32], path[LU_PATH_MAX];
	struct dirent entry, *result;
	int i, j, nr_found = 0;
	DIR *dir;

	dir = opendir(SYSFS_PCI "/devices");
	if (!dir)
		fail_exit("opendir(" SYSFS_PCI "/devices)");

	while (!next_dirent(dir, &entry, &result) && result) {
		if (!is_bdf(entry.d_name, strlen(entry.d_name)))
			continue;

		snprintf(ids, sizeof(ids), "%04x:%04x",
			 read_dev_int(entry.d_name, "vendor"),
			 read_dev_int(entry.d_name, "device"));
		if (strcmp(ids, id))
			continue;

		/* VFs are selected along with their PF. */
		dev_path(path, entry.d_name, "physfn");
		if (!access(path, F_OK))
			continue;

		if (nr_found == MAX_DEVICES)
			fail_exit("Too many devices");

		copy_str(found[nr_found++], BDF_LEN, entry.d_name);
	}

	closedir(dir);

	/* Insertion sort, nolibc has no qsort(). */
	for (i = 1; i < nr_found; i++) {
		memcpy(tmp, found[i], BDF_LEN);
		for (j = i; j > 0 && strcmp(found[j - 1], tmp) > 0; j--)
			memcpy(found[j], found[j - 1], BDF_LEN);
		memcpy(found[j], tmp, BDF_LEN);
	}

	for (i = 0; i < nr_found; i++)
		select_device_and_vfs(found[i]);
}

static struct node *find_node(struct snapshot *snap, const char *bdf)
{
	int i;

	for (i = 0; i < snap->nr_nodes; i++) {
		if (!strcmp(snap->nodes[i].bdf, bdf))
			return &snap->nodes[i];
	}

	return NULL;
}

static void snapshot_node(struct node *node, const char *bdf)
{
	char path[LU_PATH_MAX];

	memset(node, 0, sizeof(*node));
	copy_str(node->bdf, sizeof(node->bdf), bdf);

	dev_path(path, bdf, "config");
	node->cfg_len = read_file(path, node->cfg, sizeof(node->cfg));
	if (node->cfg_len < PCI_STD_HEADER_SIZEOF)
		fail_exit("Failed to read config space of %s", bdf);

	dev_path(path, bdf, "resource");
	if (read_file(path, node->resource, sizeof(node->resource) - 1) < 0)
		fail_exit("Failed to read resources of %s", bdf);

	node->numvfs = read_dev_int(bdf, "sriov_numvfs");
}

/*
 * Snapshot @bdf and every bridge upstream of it, which are the BDFs in the
 * path of the device in sysfs, e.g. ../../../devices/pci0000:00/0000:00:02.0/
 * 0000:01:00.0.
 */
static void snapshot_device(struct snapshot *snap, const char *bdf)
{
	char path[LU_PATH_MAX], link[LU_PATH_MAX], name[BDF_LEN];
	const char *start, *end;
	ssize_t len;

	snprintf(path, sizeof(path), SYSFS_PCI "/devices/%s", bdf);
	len = readlink(path, link, sizeof(link) - 1);
	if (len <= 0)
		fail_exit("readlink(%s)", path);
	link[len] = '\0';

	for (start = link; *start; start = *end ? end + 1 : end) {
		end = strchr(start, '/');
		if (!end)
			end = start + strlen(start);

		if (!is_bdf(start, end - start))
			continue;

		memcpy(name, start, end - start);
		name[end - start] = '\0';
		if (find_node(snap, name))
			continue;

		if (snap->nr_nodes == MAX_NODES)
			fail_exit("Too many devices");

		snapshot_node(&snap->nodes[snap->nr_nodes++], name);
	}
}

/* Clear the mask for the @len bytes at @off that may change across kexec. */
static void ignore(unsigned char *mask, int off, int len)
{
	if (off <= 0 || off + len > CFG_SIZE)
		return;

	memset(mask + off, 0, len);
}

static unsigned int cfg_dword(const unsigned char *cfg, int off)
{
	return cfg[off] | cfg[off + 1] << 8 | cfg[off + 2] << 16 |
	       (unsigned int)cfg[off + 3] << 24;
}

/* Set the mask for the @len bytes at @off, i.e. compare them. */
static void compare(unsigned char *mask, int off, int len)
{
	if (off < 0 || off + len > CFG_SIZE)
		return;

	memset(mask + off, 0xff, len);
}

#define MAX_EXT_CAPS	64
#define MAX_EXT_CAP_LEN	0x80

/*
 * Build a mask of the configuration space bits that must not change across a
 * Live Update. Status registers (including RW1C error status bits) and link
 * status can change at any time, so they are excluded.
 *
 * Devices can keep device-private, live data in configuration space, e.g. in
 * vendor-specific capabilities or outside of any capability in the extended
 * configuration space, so only compare the configuration space header and
 * standard (extended) capabilities. The size of an extended capability is not
 * recorded anywhere, so assume it extends up to the next one, but at most
 * MAX_EXT_CAP_LEN bytes.
 */
static void build_mask(const struct node *node, unsigned char *mask)
{
	const unsigned char *cfg = node->cfg;
	int ext_pos[MAX_EXT_CAPS], ext_id[MAX_EXT_CAPS];
	int pos, ttl, i, j, end, nr_ext = 0;

	memset(mask, 0, sizeof(node->cfg));

	/* Standard configuration space, except vendor-specific capabilities. */
	compare(mask, 0, PCI_CFG_SPACE_SIZE);

	ignore(mask, PCI_STATUS, 2);
	if ((cfg[PCI_HEADER_TYPE] & 0x7f) == PCI_HEADER_TYPE_BRIDGE)
		ignore(mask, PCI_SEC_STATUS, 2);

	pos = cfg[PCI_CAPABILITY_LIST] & ~3;
	for (ttl = 48; pos >= PCI_STD_HEADER_SIZEOF && ttl; ttl--) {
		switch (cfg[pos + PCI_CAP_LIST_ID]) {
		case PCI_CAP_ID_PM:
			/* PME_Status */
			mask[pos + PCI_PM_CTRL + 1] &= ~(PCI_PM_CTRL_PME_STATUS >> 8);
			break;
		case PCI_CAP_ID_EXP:
			ignore(mask, pos + PCI_EXP_DEVSTA, 2);
			ignore(mask, pos + PCI_EXP_LNKSTA, 2);
			ignore(mask, pos + PCI_EXP_SLTSTA, 2);
			ignore(mask, pos + PCI_EXP_RTSTA, 4);
			ignore(mask, pos + PCI_EXP_LNKSTA2, 2);
			ignore(mask, pos + PCI_EXP_SLTSTA2, 2);
			break;
		case PCI_CAP_ID_VNDR:
			/* Keep the header (ID and next pointer). */
			if (cfg[pos + PCI_CAP_FLAGS] > 2)
				ignore(mask, pos + 2, cfg[pos + PCI_CAP_FLAGS] - 2);
			break;
		}
		pos = cfg[pos + PCI_CAP_LIST_NEXT] & ~3;
	}

	if (node->cfg_len <= PCI_CFG_SPACE_SIZE)
		return;

	pos = PCI_CFG_SPACE_SIZE;
	for (ttl = 960; pos >= PCI_CFG_SPACE_SIZE && ttl && nr_ext < MAX_EXT_CAPS; ttl--) {
		unsigned int hdr = cfg_dword(cfg, pos);

		if (!hdr || hdr == 0xffffffff)
			break;

		ext_pos[nr_ext] = pos;
		ext_id[nr_ext++] = PCI_EXT_CAP_ID(hdr);
		pos = PCI_EXT_CAP_NEXT(hdr);
	}

	for (i = 0; i < nr_ext; i++) {
		pos = ext_pos[i];

		/* Always compare the header, i.e. the capability list itself. */
		compare(mask, pos, 4);
		if (ext_id[i] == PCI_EXT_CAP_ID_VNDR || ext_id[i] == PCI_EXT_CAP_ID_DVSEC)
			continue;

		end = pos + MAX_EXT_CAP_LEN;
		for (j = 0; j < nr_ext; j++) {
			if (ext_pos[j] > pos && ext_pos[j] < end)
				end = ext_pos[j];
		}
		compare(mask, pos, end - pos);

		switch (ext_id[i]) {
		case PCI_EXT_CAP_ID_ERR:
			ignore(mask, pos + PCI_ERR_UNCOR_STATUS, 4);
			ignore(mask, pos + PCI_ERR_COR_STATUS, 4);
			ignore(mask, pos + PCI_ERR_HEADER_LOG, 16);
			ignore(mask, pos + PCI_ERR_ROOT_STATUS, 4);
			ignore(mask, pos + PCI_ERR_ROOT_ERR_SRC, 4);
			break;
		case PCI_EXT_CAP_ID_SRIOV:
			ignore(mask, pos + PCI_SRIOV_STATUS, 2);
			break;
		}
	}
}

/*
 * Compare @before to the device's current state. sriov_numvfs reads as 0 until
 * the PF adopts its VFs (the SR-IOV capability itself must not change), so it
 * is only compared once @adopted.
 */
static int compare_node(const struct node *before, bool adopted)
{
	unsigned char mask[CFG_SIZE];
	struct node *after;
	int i, errors = 0;

	after = calloc(1, sizeof(*after));
	if (!after)
		fail_exit("calloc");

	snapshot_node(after, before->bdf);
	build_mask(before, mask);

	if (after->cfg_len != before->cfg_len) {
		ksft_print_msg("%s: config space size changed from %d to %d\n",
			       before->bdf, before->cfg_len, after->cfg_len);
		errors++;
	}

	for (i = 0; i < before->cfg_len && i < after->cfg_len; i++) {
		if ((before->cfg[i] ^ after->cfg[i]) & mask[i]) {
			ksft_print_msg("%s: config[0x%03x] changed from 0x%02x to 0x%02x\n",
				       before->bdf, i, before->cfg[i], after->cfg[i]);
			errors++;
		}
	}

	if (strcmp(before->resource, after->resource)) {
		ksft_print_msg("%s: resources changed from:\n%s\nto:\n%s\n",
			       before->bdf, before->resource, after->resource);
		errors++;
	}

	if (adopted && before->numvfs != after->numvfs) {
		ksft_print_msg("%s: sriov_numvfs changed from %d to %d\n",
			       before->bdf, before->numvfs, after->numvfs);
		errors++;
	}

	free(after);
	return errors;
}

static void check_enabled(const struct node *node)
{
	unsigned short cmd = node->cfg[PCI_COMMAND] | node->cfg[PCI_COMMAND + 1] << 8;

	if (!(cmd & PCI_COMMAND_MASTER))
		fail_exit("%s: Bus Master Enable is not set (cmd 0x%04x)",
			  node->bdf, cmd);
}

static int memfd_preserve_buf(int session_fd, int token, const void *buf,
			      size_t len)
{
	size_t done = 0;
	ssize_t ret;
	int fd;

	fd = memfd_create("luo_pci_snapshot", 0);
	if (fd < 0)
		return -errno;

	while (done < len) {
		ret = write(fd, (const char *)buf + done, len - done);
		if (ret <= 0)
			return -errno;
		done += ret;
	}

	return luo_session_preserve_fd(session_fd, fd, token);
}

static void memfd_retrieve_buf(int session_fd, int token, void *buf, size_t len)
{
	size_t done = 0;
	ssize_t ret;
	int fd;

	fd = luo_session_retrieve_fd(session_fd, token);
	if (fd < 0)
		fail_exit("Failed to retrieve snapshot memfd");

	if (lseek(fd, 0, SEEK_SET) < 0)
		fail_exit("lseek");

	while (done < len) {
		ret = read(fd, (char *)buf + done, len - done);
		if (ret <= 0)
			fail_exit("Failed to read snapshot");
		done += ret;
	}

	close(fd);
}

static int open_device(const char *bdf)
{
	struct pci_lu_test_set_device arg = { .argsz = sizeof(arg) };
	char hex[8];
	int fd;

	/* bdf is "DDDD:BB:DD.F", see is_bdf(). */
	memcpy(hex, bdf, 4);
	hex[4] = '\0';
	arg.domain = strtoul(hex, NULL, 16);
	memcpy(hex, bdf + 5, 2);
	hex[2] = '\0';
	arg.bus = strtoul(hex, NULL, 16);
	memcpy(hex, bdf + 8, 2);
	hex[2] = '\0';
	arg.devfn = strtoul(hex, NULL, 16) << 3 | (bdf[11] - '0');

	fd = open("/dev/" PCI_LU_TEST_DEV_NAME, O_RDWR);
	if (fd < 0)
		ksft_exit_skip("Failed to open /dev/" PCI_LU_TEST_DEV_NAME
			       ". Is the pci_lu_test module loaded?\n");

	if (ioctl(fd, PCI_LU_TEST_IOCTL_SET_DEVICE, &arg))
		fail_exit("PCI_LU_TEST_IOCTL_SET_DEVICE(%s)", bdf);

	return fd;
}

static void session_name(char *buf, size_t len, int i)
{
	snprintf(buf, len, "luo_pci_%d", i);
}

static void run_stage_1(int luo_fd)
{
	struct snapshot *snap;
	char name[64];
	int i, session_fd, fd;

	if (!nr_devices)
		ksft_exit_skip("No devices to preserve\n");

	snap = calloc(1, sizeof(*snap));
	if (!snap)
		fail_exit("calloc");

	for (i = 0; i < nr_devices; i++) {
		ksft_print_msg("[STAGE 1] Binding %s to " DRIVER "\n", devices[i]);
		bind_device(devices[i]);
	}

	/* Snapshot after binding, since binding enables the devices. */
	snap->nr_devices = nr_devices;
	for (i = 0; i < nr_devices; i++) {
		copy_str(snap->devices[i], BDF_LEN, devices[i]);
		snapshot_device(snap, devices[i]);
		check_enabled(find_node(snap, devices[i]));
	}

	ksft_print_msg("[STAGE 1] Snapshot of %d devices (incl. upstream bridges)\n",
		       snap->nr_nodes);

	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_MEMFD_TOKEN, 2);

#ifdef LUO_PCI_AUTO_PF
	for (i = 0; i < nr_devices; i++)
		snap->auto_pf[i] = find_node(snap, devices[i])->numvfs > 0;
#endif

	session_fd = luo_create_session(luo_fd, SNAPSHOT_SESSION_NAME);
	if (session_fd < 0 ||
	    memfd_preserve_buf(session_fd, SNAPSHOT_MEMFD_TOKEN, snap,
			       sizeof(*snap)) < 0)
		fail_exit("Failed to preserve snapshot");

	/* One session per device, in order, so that PFs are frozen before VFs. */
	for (i = 0; i < nr_devices; i++) {
		if (snap->auto_pf[i]) {
			ksft_print_msg("[STAGE 1] Not preserving PF %s explicitly\n",
				       devices[i]);
			continue;
		}

		session_name(name, sizeof(name), i);
		session_fd = luo_create_session(luo_fd, name);
		if (session_fd < 0)
			fail_exit("luo_create_session(%s)", name);

		fd = open_device(devices[i]);
		if (luo_session_preserve_fd(session_fd, fd, DEVICE_TOKEN) < 0)
			fail_exit("Failed to preserve %s", devices[i]);

		ksft_print_msg("[STAGE 1] Preserved %s in session %s\n",
			       devices[i], name);
	}

	for (i = 0; i < nr_devices; i++) {
		if (check_guards(devices[i]))
			fail_exit("Preserved device %s is not protected", devices[i]);
	}

	close(luo_fd);
	daemonize_and_wait();
}

static void run_stage_2(int luo_fd, int state_session_fd)
{
	char path[LU_PATH_MAX], val[32], name[64];
	int session_fds[MAX_DEVICES];
	struct snapshot *snap;
	struct node *node;
	int i, fd, stage, snapshot_fd, errors = 0;

	restore_and_read_stage(state_session_fd, STATE_MEMFD_TOKEN, &stage);
	if (stage != 2)
		fail_exit("Expected stage 2, but state file contains %d", stage);

	snap = calloc(1, sizeof(*snap));
	if (!snap)
		fail_exit("calloc");

	snapshot_fd = luo_retrieve_session(luo_fd, SNAPSHOT_SESSION_NAME);
	if (snapshot_fd < 0)
		fail_exit("luo_retrieve_session(" SNAPSHOT_SESSION_NAME ")");

	memfd_retrieve_buf(snapshot_fd, SNAPSHOT_MEMFD_TOKEN, snap, sizeof(*snap));

	for (i = 0; i < snap->nr_devices; i++) {
		session_fds[i] = -1;
		if (snap->auto_pf[i])
			continue;

		session_name(name, sizeof(name), i);
		session_fds[i] = luo_retrieve_session(luo_fd, name);
		if (session_fds[i] < 0)
			fail_exit("luo_retrieve_session(%s)", name);
	}

	/*
	 * Check everything before the test touches anything. VFs are not
	 * enumerated until their PF adopts them.
	 */
	for (i = 0; i < snap->nr_nodes; i++) {
		node = &snap->nodes[i];
		dev_path(path, node->bdf, "");
		if (access(path, F_OK)) {
			ksft_print_msg("[STAGE 2] %s not enumerated yet\n", node->bdf);
			continue;
		}

		ksft_print_msg("[STAGE 2] Checking %s before binding\n", node->bdf);
		errors += compare_node(node, false);
	}

	for (i = 0; i < snap->nr_devices; i++) {
		const char *bdf = snap->devices[i];

		/* VFs show up when their PF adopts them, below. */
		errors += check_bound(bdf);

		node = find_node(snap, bdf);
		if (node->numvfs > 0) {
			ksft_print_msg("[STAGE 2] Adopting %d VFs of %s\n",
				       node->numvfs, bdf);
			snprintf(val, sizeof(val), "%d", node->numvfs);
			dev_path(path, bdf, "sriov_numvfs");
			write_file(path, val);
		}

		if (snap->auto_pf[i])
			continue;

		fd = luo_session_retrieve_fd(session_fds[i], DEVICE_TOKEN);
		if (fd < 0)
			fail_exit("Failed to retrieve %s", bdf);

		ksft_print_msg("[STAGE 2] Retrieved %s\n", bdf);
	}

	/* VFs first, so that removing a PF doesn't take its VFs with it. */
	for (i = snap->nr_devices - 1; i >= 0; i--)
		errors += check_guards(snap->devices[i]);

	/* And again after binding and retrieving. */
	for (i = 0; i < snap->nr_nodes; i++) {
		node = &snap->nodes[i];
		ksft_print_msg("[STAGE 2] Checking %s after binding\n", node->bdf);
		errors += compare_node(node, true);
	}

	if (errors)
		fail_exit("%d unexpected changes across Live Update", errors);

	/*
	 * Finish VFs before their PFs. PFs that were preserved automatically
	 * finish along with their last VF.
	 */
	for (i = snap->nr_devices - 1; i >= 0; i--) {
		if (session_fds[i] < 0)
			continue;

		if (luo_session_finish(session_fds[i]) < 0)
			fail_exit("luo_session_finish for %s", snap->devices[i]);
		close(session_fds[i]);
	}

	if (luo_session_finish(snapshot_fd) < 0)
		fail_exit("luo_session_finish for snapshot session");
	close(snapshot_fd);

	if (luo_session_finish(state_session_fd) < 0)
		fail_exit("luo_session_finish for state session");
	close(state_session_fd);

	/* The driver_override set for Live Update is gone once finished. */
	for (i = 0; i < snap->nr_devices; i++)
		errors += check_no_override(snap->devices[i]);

	if (errors)
		fail_exit("driver_override not cleared after finishing");

	ksft_print_msg("\n--- PCI LIVE UPDATE TEST PASSED ---\n");
}

int main(int argc, char *argv[])
{
	bool device_args = false;
	int stage = 1;
	size_t i;
	int arg;

	for (arg = 1; arg < argc; arg++) {
		if (!strcmp(argv[arg], "-s") && arg + 1 < argc)
			stage = atoi(argv[arg + 1]);
	}

	/* Stage 2 takes the devices from the snapshot. */
	for (arg = 1; stage == 1 && arg < argc; arg++) {
		if (!strcmp(argv[arg], "-s")) {
			arg++;
			continue;
		}

		device_args = true;
		if (is_bdf(argv[arg], strlen(argv[arg])))
			select_device_and_vfs(argv[arg]);
		else if (is_id(argv[arg]))
			select_devices_by_id(argv[arg]);
		else
			ksft_exit_fail_msg("Invalid device %s\n", argv[arg]);
	}

	if (stage == 1 && !device_args) {
		for (i = 0; i < sizeof(default_ids) / sizeof(default_ids[0]); i++)
			select_devices_by_id(default_ids[i]);
	}

	/* luo_test() parses -s again and ignores the DEVICE arguments. */
	return luo_test(argc, argv, STATE_SESSION_NAME, run_stage_1, run_stage_2);
}
