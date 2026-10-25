// SPDX-License-Identifier: GPL-2.0

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <linux/kexec.h>

/* from arch/x86/include/asm/setup.h */
#define COMMAND_LINE_SIZE	2048

#define KERNEL_IMAGE "/kernel"
#define FDT_PHYS_PATH "/debugfs/kho/out/fdt_phys"

#if defined(__x86_64__)
#define KHO_RO_MEM_PARAM "memmap=792M$16M memmap=24M$1000M"
#elif defined(__aarch64__)
#define KHO_RO_MEM_PARAM "mem=128M"
#else
#define KHO_RO_MEM_PARAM ""
#endif

static int mount_filesystems(void)
{
	if (mount("debugfs", "/debugfs", "debugfs", 0, NULL) < 0)
		return -1;

	return mount("proc", "/proc", "proc", 0, NULL);
}

static long kexec_file_load(int kernel_fd, int initrd_fd,
			    unsigned long cmdline_len, const char *cmdline,
			    unsigned long flags)
{
	return syscall(__NR_kexec_file_load, kernel_fd, initrd_fd, cmdline_len,
		       cmdline, flags);
}

static int kexec_load(void)
{
	char cmdline[COMMAND_LINE_SIZE];
	ssize_t len, fdt_len;
	char fdt_phys[64];
	int fd, err;

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd < 0)
		return -1;

	len = read(fd, cmdline, sizeof(cmdline));
	close(fd);
	if (len <= 0)
		return -1;

	/* replace \n with \0 */
	cmdline[len - 1] = 0;

	if (strstr(cmdline, "kho_ro_test")) {
		fd = open(FDT_PHYS_PATH, O_RDONLY);
		if (fd < 0)
			return -1;

		fdt_len = read(fd, fdt_phys, sizeof(fdt_phys) - 1);
		close(fd);
		if (fdt_len <= 0)
			return -1;

		if (fdt_phys[fdt_len - 1] == '\n')
			fdt_len--;
		fdt_phys[fdt_len] = 0;

		len = (len - 1) + snprintf(cmdline + len - 1,
					   sizeof(cmdline) - (len - 1),
					   " kho_ro=%s " KHO_RO_MEM_PARAM,
					   fdt_phys) + 1;
		if (len <= 0 || (size_t)len > sizeof(cmdline))
			return -1;
	}

	fd = open(KERNEL_IMAGE, O_RDONLY);
	if (fd < 0)
		return -1;

	err = kexec_file_load(fd, -1, len, cmdline, KEXEC_FILE_NO_INITRAMFS);
	close(fd);

	return err ? : 0;
}

int main(int argc, char *argv[])
{
	if (mount_filesystems())
		goto err_reboot;

	if (kexec_load())
		goto err_reboot;

	if (reboot(RB_KEXEC))
		goto err_reboot;

	return 0;

err_reboot:
	reboot(RB_AUTOBOOT);
	return -1;
}
