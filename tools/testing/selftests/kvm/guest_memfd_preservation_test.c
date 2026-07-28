// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Google LLC.
 *
 * Author: Tarun Sahu <tarunsahu@google.com>
 *
 * Test for VM and guest_memfd preservation across kexec (Live Update) via LUO.
 *
 * NOTE: This is a MANUAL test and is excluded from automated CI/testing
 * frameworks because Stage 1 daemonizes into the background to pin resources
 * and requires a human operator to manually trigger kexec before Stage 2
 * is executed. Running Stage 1 automatically would leak the background daemon
 * and cause CI runners to falsely interpret it as a passed test.
 *
 * Usage:
 * Stage 1: ./guest_memfd_preservation_test --s 1
 * Stage 2: ./guest_memfd_preservation_test --s 2
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/sizes.h>
#include <linux/falloc.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "ucall_common.h"
#include "../kselftest.h"
#include "../kselftest_harness.h"

#include <libliveupdate.h>

#define TEST_PASSED(fmt, ...) ksft_print_msg(fmt, ##__VA_ARGS__)

#define SESSION_NAME "gmem_vm_preservation_session"
#define VM_TOKEN 0x1001
#define GMEM_TOKEN 0x1002

#define STATE_SESSION_NAME "gmem_preservation_state"
#define STATE_TOKEN 0x999

#define UNSHARED_GMEM_TOKEN 0x9001
#define MMAP_ONLY_GMEM_TOKEN 0x9002
#define NON_GMEM_TOKEN 0x9003
#define FROZEN_GMEM_TOKEN 0x8888

#define GPA SZ_4G
#define GMEM_MEM_SLOT 1
#define GMEM_SIZE (16ULL * 1024 * 1024)
#define DATA_SIZE (5ULL * 1024 * 1024)

static size_t page_size;

/* Deterministic byte pattern generation based on offset */
static inline uint8_t get_pattern_byte(size_t offset)
{
	return (uint8_t)(offset ^ 0x5A);
}

static void guest_code_phase1(uint64_t gpa)
{
	uint8_t *mem = (uint8_t *)gpa;

	for (size_t i = 0; i < DATA_SIZE; i++)
		mem[i] = get_pattern_byte(i);

	GUEST_DONE();
}

static void guest_code_phase2(uint64_t gpa)
{
	uint8_t *mem = (uint8_t *)gpa;

	for (size_t i = 0; i < DATA_SIZE; i++) {
		uint8_t val = get_pattern_byte(i);

		__GUEST_ASSERT(mem[i] == val,
			       "Data mismatch at offset %lu! Expected 0x%x, got 0x%x",
			       i, val, mem[i]);
	}

	GUEST_DONE();
}

static void setup_guest_memfd_region(struct kvm_vm *vm, int gmem_fd)
{
	vm_set_user_memory_region2(vm, GMEM_MEM_SLOT, KVM_MEM_GUEST_MEMFD, GPA, GMEM_SIZE, NULL,
				   gmem_fd, 0);

	for (size_t i = 0; i < GMEM_SIZE; i += page_size)
		virt_pg_map(vm, GPA + i, GPA + i);
}

static void test_preserve_disallowed_no_init_shared(int session_fd,
						    struct kvm_vm *vm)
{
	int unshared_fd, ret;

	ksft_print_msg("[STAGE 1] TEST 1: Preserving guest_memfd without INIT_SHARED...\n");

	unshared_fd = __vm_create_guest_memfd(vm, GMEM_SIZE, 0);
	if (unshared_fd >= 0) {
		ret = luo_session_preserve_fd(session_fd, unshared_fd, UNSHARED_GMEM_TOKEN);
		TEST_ASSERT(ret < 0,
			    "Preservation without INIT_SHARED should fail");
		close(unshared_fd);
	}
	TEST_PASSED("[STAGE 1] TEST 1: PASSED\n");
}

static void test_preserve_disallowed_mmap_only(int session_fd,
						struct kvm_vm *vm)
{
	int mmap_only_fd, ret;

	if (!(vm_check_cap(vm, KVM_CAP_GUEST_MEMFD_FLAGS) &
	      GUEST_MEMFD_FLAG_MMAP))
		return;

	ksft_print_msg("[STAGE 1] TEST 2: Preserving guest_memfd with MMAP only...\n");

	mmap_only_fd = __vm_create_guest_memfd(vm, GMEM_SIZE,
					       GUEST_MEMFD_FLAG_MMAP);
	if (mmap_only_fd >= 0) {
		ret = luo_session_preserve_fd(session_fd, mmap_only_fd, MMAP_ONLY_GMEM_TOKEN);
		TEST_ASSERT(ret < 0,
			    "Preserving guest_memfd with MMAP only should fail");
		close(mmap_only_fd);
	}
	TEST_PASSED("[STAGE 1] TEST 2: PASSED\n");
}

static void test_preserve_disallowed_non_gmem(int session_fd)
{
	int null_fd, ret;

	ksft_print_msg("[STAGE 1] TEST 3: Preserving non-guest_memfd FD (/dev/null)...\n");

	null_fd = open("/dev/null", O_RDWR);
	if (null_fd >= 0) {
		ret = luo_session_preserve_fd(session_fd, null_fd, NON_GMEM_TOKEN);
		TEST_ASSERT(ret < 0,
			    "Preserving /dev/null should fail");
		close(null_fd);
	}
	TEST_PASSED("[STAGE 1] TEST 3: PASSED\n");
}

static void test_allocate_while_frozen(int luo_fd, struct kvm_vm *vm)
{
	int frozen_session, frozen_fd, ret;
	uint64_t flags;
	char *mem;

	flags = GUEST_MEMFD_FLAG_MMAP | GUEST_MEMFD_FLAG_INIT_SHARED;
	if ((vm_check_cap(vm, KVM_CAP_GUEST_MEMFD_FLAGS) & flags) != flags)
		return;

	ksft_print_msg("[STAGE 1] TEST 4: Operations on frozen guest_memfd...\n");

	frozen_session = luo_create_session(luo_fd, "session_frozen_ops");
	TEST_ASSERT(frozen_session >= 0, "Failed to create frozen test session");

	frozen_fd = __vm_create_guest_memfd(vm, GMEM_SIZE, flags);
	TEST_ASSERT(frozen_fd >= 0, "Failed to create guest_memfd for frozen test");

	mem = kvm_mmap(GMEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, frozen_fd);
	mem[0] = get_pattern_byte(0);
	kvm_munmap(mem, GMEM_SIZE);

	ret = luo_session_preserve_fd(frozen_session, frozen_fd, FROZEN_GMEM_TOKEN);
	TEST_ASSERT(ret == 0, "Failed to preserve guest_memfd for frozen test");

	ret = fallocate(frozen_fd, FALLOC_FL_KEEP_SIZE, DATA_SIZE, page_size);
	TEST_ASSERT(ret == -1 && errno == EPERM,
		    "fallocate on frozen guest_memfd failed ret=%d errno=%d",
		    ret, errno);

	ret = fallocate(frozen_fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
			0, page_size);
	TEST_ASSERT(ret == -1 && errno == EPERM,
		    "fallocate punch hole on frozen guest_memfd failed ret=%d errno=%d",
		    ret, errno);

	mem = kvm_mmap(GMEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, frozen_fd);

	TEST_ASSERT_EQ(READ_ONCE(mem[0]), get_pattern_byte(0));
	TEST_EXPECT_SIGBUS((void)READ_ONCE(mem[DATA_SIZE]));

	kvm_munmap(mem, GMEM_SIZE);

	luo_session_finish(frozen_session);
	close(frozen_session);
	close(frozen_fd);

	TEST_PASSED("[STAGE 1] TEST 4: PASSED\n");
}

static void test_gmem_preservation_stage1(int session_fd, struct kvm_vm *vm,
					   struct kvm_vcpu *vcpu, int *gmem_fd)
{
	uint64_t flags = GUEST_MEMFD_FLAG_MMAP | GUEST_MEMFD_FLAG_INIT_SHARED;
	int fd, ret;

	ksft_print_msg("[STAGE 1] TEST 5: Preserving VM and guest_memfd...\n");

	fd = vm_create_guest_memfd(vm, GMEM_SIZE, flags);
	setup_guest_memfd_region(vm, fd);

	vcpu_args_set(vcpu, 1, GPA);
	vcpu_run(vcpu);
	TEST_ASSERT_EQ(get_ucall(vcpu, NULL), UCALL_DONE);

	ret = luo_session_preserve_fd(session_fd, vm->fd, VM_TOKEN);
	TEST_ASSERT(ret == 0, "Failed to preserve VM file descriptor");

	ret = luo_session_preserve_fd(session_fd, fd, GMEM_TOKEN);
	TEST_ASSERT(ret == 0, "Failed to preserve guest_memfd file descriptor");

	*gmem_fd = fd;
}

static void test_gmem_preservation_stage2(int retrieved_vm_fd,
					   int retrieved_gmem_fd)
{
	struct userspace_mem_region *slot0;
	struct vm_shape shape = VM_SHAPE_DEFAULT;
	u64 nr_pages = 2048; /* 8MB for slot0 pages */
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	int i;

	ksft_print_msg("[STAGE 2] TEST 5 (cont): Verifying VM & guest_memfd data...\n");

	vm = vm_create_from_fd(retrieved_vm_fd, shape);

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, 0, 0,
				    nr_pages, 0);
	kvm_vm_elf_load(vm, program_invocation_name);

	for (i = 0; i < NR_MEM_REGIONS; i++)
		vm->memslots[i] = 0;

	slot0 = memslot2region(vm, 0);
	ucall_init(vm, slot0->region.guest_phys_addr +
		       slot0->region.memory_size);

	setup_guest_memfd_region(vm, retrieved_gmem_fd);

	vcpu = vm_vcpu_add(vm, 0, guest_code_phase2);
	kvm_arch_vm_finalize_vcpus(vm);

	vcpu_args_set(vcpu, 1, GPA);

	printf("Resuming / Running VM in Phase 2...\n");
	vcpu_run(vcpu);
	TEST_ASSERT_EQ(get_ucall(vcpu, NULL), UCALL_DONE);

	/* kvm_vm_free will also close retrieved_vm_fd */
	kvm_vm_free(vm);
	TEST_PASSED("[STAGE 2] TEST 5: PASSED\n");
}

static void run_stage_1(int luo_fd)
{
	int gmem_fd, session_fd;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	ksft_print_msg("[STAGE 1] Starting pre-kexec setup...\n");

	ksft_print_msg("[STAGE 1] Creating state file for next stage (2)...\n");
	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_TOKEN, 2);

	vm = __vm_create_shape_with_one_vcpu(VM_SHAPE_DEFAULT, &vcpu, 1,
					guest_code_phase1);

	ksft_print_msg("[STAGE 1] Creating session '%s' and preserving FDs...\n",
		       SESSION_NAME);
	session_fd = luo_create_session(luo_fd, SESSION_NAME);
	TEST_ASSERT(session_fd >= 0, "Failed to create LUO session");

	/* Run negative tests for disallowed preservation conditions */
	test_preserve_disallowed_no_init_shared(session_fd, vm);
	test_preserve_disallowed_mmap_only(session_fd, vm);
	test_preserve_disallowed_non_gmem(session_fd);

	/* Run operations on frozen guest_memfd test independently */
	test_allocate_while_frozen(luo_fd, vm);

	/* Run primary VM & guest_memfd preservation test */
	test_gmem_preservation_stage1(session_fd, vm, vcpu, &gmem_fd);

	printf("\n============================================================\n");
	printf("Phase 1 Complete Successfully!\n");
	printf("VM file and guest_memfd file have been preserved via LUO.\n");
	printf("Tokens: VM_TOKEN=0x%x, GMEM_TOKEN=0x%x\n", VM_TOKEN, GMEM_TOKEN);
	printf("Machine Size: %llu MB, Data Size: %llu MB\n", GMEM_SIZE / SZ_1M,
				 DATA_SIZE / SZ_1M);
	printf("------------------------------------------------------------\n");

	close(luo_fd);
	daemonize_and_wait();
}

static void run_stage_2(int luo_fd, int state_session_fd)
{
	int retrieved_vm_fd, retrieved_gmem_fd, session_fd, stage;

	ksft_print_msg("[STAGE 2] Starting post-kexec verification...\n");

	restore_and_read_stage(state_session_fd, STATE_TOKEN, &stage);
	if (stage != 2)
		fail_exit("Expected stage 2, but state file contains %d", stage);

	ksft_print_msg("[STAGE 2] Retrieving session '%s'...\n", SESSION_NAME);
	session_fd = luo_retrieve_session(luo_fd, SESSION_NAME);
	TEST_ASSERT(session_fd >= 0, "Failed to retrieve LUO session");

	retrieved_vm_fd = luo_session_retrieve_fd(session_fd, VM_TOKEN);
	TEST_ASSERT(retrieved_vm_fd >= 0, "Failed to retrieve VM file descriptor");

	retrieved_gmem_fd = luo_session_retrieve_fd(session_fd, GMEM_TOKEN);
	TEST_ASSERT(retrieved_gmem_fd >= 0, "Failed to retrieve guest_memfd file descriptor");

	/* Run primary VM & guest_memfd post-kexec data verification */
	test_gmem_preservation_stage2(retrieved_vm_fd, retrieved_gmem_fd);

	printf("\nSUCCESS: Phase 2 Complete! All 5MB complex data verified intact!\n");

	luo_session_finish(session_fd);
	close(session_fd);

	ksft_print_msg("[STAGE 2] Finalizing state session...\n");
	if (luo_session_finish(state_session_fd) < 0)
		fail_exit("luo_session_finish for state session");
	close(state_session_fd);

	close(retrieved_gmem_fd);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_GUEST_MEMFD));
	page_size = getpagesize();

	return luo_test(argc, argv, STATE_SESSION_NAME,
			run_stage_1, run_stage_2);
}
