// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * A selftest to validate the lifecycle of preserved CPUs across Live Update
 * and within a local session.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libliveupdate.h>

#define TEST_SESSION_NAME "cpu-preserve-session"
#define STATE_SESSION_NAME "cpu_preserve_state"
#define STATE_MEMFD_TOKEN 999
#define TEST_CPU_TOKEN 0x100
#define TEST_MEM_TOKEN 0x101
#define JOB_MAGIC 0x52505550435f4b4fULL

struct cpu_job_data {
	uint64_t input;
	uint64_t output;
	uint64_t counter;
	uint64_t stop;
};

extern const char __start_cpu_job_test[];
extern const char __stop_cpu_job_test[];

static void __attribute__((noinline, used, section("cpu_job_test")))
cpu_job_entry_fn(void *arg)
{
	struct cpu_job_data *data = arg;

	__atomic_store_n(&data->output, ~data->input, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&data->stop, __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&data->counter, data->counter + 1,
				 __ATOMIC_RELAXED);
#if defined(__x86_64__)
		asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
		asm volatile("yield" ::: "memory");
#endif
	}
}

static int find_target_cpu(void)
{
	char path[128];
	int cpu;

	for (cpu = 1; cpu < 256; cpu++) {
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/preserve", cpu);
		if (access(path, R_OK) == 0) {
			snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpu);
			if (access(path, R_OK) == 0)
				return cpu;
		}
	}
	return -1;
}

static int read_cpu_attr(int cpu, const char *attr)
{
	char path[128], buf[16];
	int fd, val = -1;

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/%s", cpu, attr);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	if (read(fd, buf, sizeof(buf) - 1) > 0)
		val = atoi(buf);
	close(fd);
	return val;
}

static int preserve_cpu_fd(int session_fd, int cpu, int token,
			   struct liveupdate_cpu_preserve_arg *job_arg)
{
	char path[128];
	int cpu_fd, ret;

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/preserve", cpu);
	cpu_fd = open(path, O_RDONLY);
	if (cpu_fd < 0)
		return -1;

	ret = luo_session_preserve_fd_arg(session_fd, cpu_fd, token,
					  (uintptr_t)job_arg);
	close(cpu_fd);
	return ret;
}

/* Stage 1: Executed before kexec */
static void run_stage_1(int luo_fd)
{
	size_t code_size = __stop_cpu_job_test - __start_cpu_job_test;
	size_t data_offset = (code_size + 63) & ~63UL;
	struct liveupdate_cpu_preserve_arg job_arg = {};
	long page_size = getpagesize();
	int target_cpu, session_fd, mem_fd;
	struct cpu_job_data *job_data;
	void *map;

	target_cpu = find_target_cpu();
	if (target_cpu < 0)
		ksft_exit_skip("No hotpluggable CPU with preserve attribute found\n");

	ksft_print_msg("[STAGE 1] Target CPU for preservation: %d\n", target_cpu);

	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_MEMFD_TOKEN, target_cpu);

	session_fd = luo_create_session(luo_fd, TEST_SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_create_session for '%s'", TEST_SESSION_NAME);

	mem_fd = memfd_create("cpu-job-mem", 0);
	if (mem_fd < 0)
		fail_exit("memfd_create");
	if (ftruncate(mem_fd, page_size) < 0)
		fail_exit("ftruncate");

	map = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, 0);
	if (map == MAP_FAILED)
		fail_exit("mmap");

	memcpy(map, __start_cpu_job_test, code_size);
	job_data = (struct cpu_job_data *)((char *)map + data_offset);
	job_data->input = JOB_MAGIC;
	job_data->output = 0;
	job_data->counter = 0;
	job_data->stop = 0;

	if (luo_session_preserve_fd(session_fd, mem_fd, TEST_MEM_TOKEN) < 0)
		fail_exit("luo_session_preserve_fd for job memfd");

	job_arg.size = sizeof(job_arg);
	job_arg.mem_token = TEST_MEM_TOKEN;
	job_arg.entry_offset = (uintptr_t)cpu_job_entry_fn -
			       (uintptr_t)__start_cpu_job_test;
	job_arg.arg_offset = data_offset;

	if (preserve_cpu_fd(session_fd, target_cpu, TEST_CPU_TOKEN, &job_arg) < 0)
		fail_exit("preserve_cpu_fd for cpu %d", target_cpu);

	if (read_cpu_attr(target_cpu, "preserve") != 1 ||
	    read_cpu_attr(target_cpu, "online") != 0)
		fail_exit("CPU %d was expected to be preserved and offline", target_cpu);

	if (__atomic_load_n(&job_data->output, __ATOMIC_ACQUIRE) != 0)
		fail_exit("Job started before session freeze");

	ksft_print_msg("[STAGE 1] CPU %d preserved with job; waiting for kexec\n", target_cpu);

	munmap(map, page_size);
	close(mem_fd);
	close(luo_fd);
	daemonize_and_wait();
}

/* Stage 2: Executed after kexec */
static void run_stage_2(int luo_fd, int state_session_fd)
{
	size_t code_size = __stop_cpu_job_test - __start_cpu_job_test;
	size_t data_offset = (code_size + 63) & ~63UL;
	int target_cpu, session_fd, cpu_fd, mem_fd, i;
	long page_size = getpagesize();
	struct cpu_job_data *job_data;
	uint64_t c1 = 0, c2 = 0;
	void *map;

	restore_and_read_stage(state_session_fd, STATE_MEMFD_TOKEN, &target_cpu);
	ksft_print_msg("[STAGE 2] Restored target CPU: %d\n", target_cpu);

	session_fd = luo_retrieve_session(luo_fd, TEST_SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", TEST_SESSION_NAME);

	cpu_fd = luo_session_retrieve_fd(session_fd, TEST_CPU_TOKEN);
	if (cpu_fd < 0)
		fail_exit("luo_session_retrieve_fd for cpu token %#x", TEST_CPU_TOKEN);
	close(cpu_fd);

	/* A retrieved CPU stays preserved and parked until the session finishes */
	if (read_cpu_attr(target_cpu, "preserve") != 1 ||
	    read_cpu_attr(target_cpu, "online") != 0)
		fail_exit("CPU %d was expected to stay preserved after retrieval", target_cpu);

	ksft_print_msg("[STAGE 2] CPU %d successfully retrieved and still parked\n", target_cpu);

	mem_fd = luo_session_retrieve_fd(session_fd, TEST_MEM_TOKEN);
	if (mem_fd < 0)
		fail_exit("luo_session_retrieve_fd for mem token %#x", TEST_MEM_TOKEN);

	map = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, 0);
	if (map == MAP_FAILED)
		fail_exit("mmap retrieved job memfd");

	job_data = (struct cpu_job_data *)((char *)map + data_offset);

	for (i = 0; i < 1000 && !__atomic_load_n(&job_data->output, __ATOMIC_ACQUIRE); i++)
		usleep(1000);
	if (__atomic_load_n(&job_data->output, __ATOMIC_ACQUIRE) != ~JOB_MAGIC)
		fail_exit("CPU %d job output mismatch: %#llx", target_cpu,
			  (unsigned long long)__atomic_load_n(&job_data->output, __ATOMIC_ACQUIRE));

	for (i = 0; i < 1000 && !(c1 = __atomic_load_n(&job_data->counter, __ATOMIC_ACQUIRE)); i++)
		usleep(1000);
	if (!c1)
		fail_exit("CPU %d job counter did not start", target_cpu);

	for (i = 0; i < 1000; i++) {
		c2 = __atomic_load_n(&job_data->counter, __ATOMIC_ACQUIRE);
		if (c2 > c1)
			break;
		usleep(1000);
	}
	if (c2 <= c1)
		fail_exit("CPU %d job did not progress after kexec (counter=%llu)",
			  target_cpu, (unsigned long long)c2);

	ksft_print_msg("[STAGE 2] CPU %d job survived kexec and progressed (%llu -> %llu)\n",
		       target_cpu, (unsigned long long)c1, (unsigned long long)c2);

	__atomic_store_n(&job_data->stop, 1, __ATOMIC_RELEASE);
	munmap(map, page_size);
	close(mem_fd);

	if (luo_session_finish(session_fd) < 0)
		fail_exit("luo_session_finish for test session");
	close(session_fd);

	if (read_cpu_attr(target_cpu, "preserve") != 0 ||
	    read_cpu_attr(target_cpu, "online") != 1)
		fail_exit("CPU %d was expected to be online after finish", target_cpu);

	ksft_print_msg("[STAGE 2] CPU %d online after finish\n", target_cpu);

	if (luo_session_finish(state_session_fd) < 0)
		fail_exit("luo_session_finish for state session");
	close(state_session_fd);

	ksft_print_msg("\n--- CPU PRESERVATION KEXEC TEST PASSED ---\n");
}

int main(int argc, char *argv[])
{
	return luo_test(argc, argv, STATE_SESSION_NAME,
			run_stage_1, run_stage_2);
}
