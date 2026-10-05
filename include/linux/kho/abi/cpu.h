/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _LINUX_KHO_ABI_CPU_H
#define _LINUX_KHO_ABI_CPU_H

#include <linux/build_bug.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <asm/page.h>

/**
 * DOC: CPU Preservation Live Update ABI
 *
 * Physical CPU preservation uses the ABI defined below to serialize
 * and restore the state of preserved CPUs across a live update kexec
 * reboot using the LUO.
 *
 * Preserved CPUs are isolated from host scheduling and remain active
 * in a parking loop or running workload across the live update reboot.
 * This ABI provides the contract for communicating preserved core metadata
 * and per-file descriptor state to the incoming kernel.
 *
 * The state is serialized into the structures defined below
 * (struct cpupr_global_ser, struct cpupr_ser and the
 * structures they point to), which are handed over to the next kernel via
 * the KHO mechanism.  Their fields are naturally aligned, without implicit
 * padding.
 *
 * This interface is a contract. Any modification to the structure
 * fields, compatible strings, or the layout of the serialization
 * structures defined here constitutes a breaking change.
 * Such changes require incrementing the version number in the
 * CPUPR_LUO_FLB_COMPATIBLE or CPUPR_LUO_FH_COMPATIBLE
 * compatibility strings to prevent a new kernel from misinterpreting
 * data from an old kernel.
 *
 * Changes are allowed provided the compatibility version is
 * incremented; however, backward/forward compatibility is only
 * guaranteed for kernels supporting the same ABI version.
 */

/* The compatibility string for preserved CPU FLB */
#if defined(CONFIG_X86_64)
#include <linux/kho/abi/cpu_x86.h>
#define CPUPR_LUO_FLB_COMPATIBLE	"cpu_flb_x86_v1"
#elif defined(CONFIG_ARM64)
#include <linux/kho/abi/cpu_arm64.h>
#define CPUPR_LUO_FLB_COMPATIBLE	"cpu_flb_arm64_v1"
#else
#define CPUPR_LUO_FLB_COMPATIBLE	"cpu_flb_v1"
struct cpupr_arch_mode_ser {
	u8 reserved[8];
};
#endif

/* The compatibility string for preserved CPU file handler */
#define CPUPR_LUO_FH_COMPATIBLE		"cpu_fh_v1"

/**
 * enum cpupr_workload - Preserved CPU execution lifecycle states
 * @CPUPR_PARKING: CPU is being offlined and entering the preserved park loop.
 * @CPUPR_PARKED: CPU is parked in the preserved park loop.
 * @CPUPR_EXITING: Host requested the preserved CPU to exit the park loop.
 * @CPUPR_DEAD: Preserved CPU has exited the park loop and stopped cleanly.
 * @CPUPR_FAULTED: Preserved CPU stopped on an unexpected exception or fault.
 * @CPUPR_WORKLOAD: Preserved CPU is executing an attached workload.
 * @CPUPR_DETACHING: Host requested the running workload to return to the park loop.
 * @CPUPR_NR_STATES: Total number of preserved CPU states.
 */
enum cpupr_workload {
	CPUPR_PARKING = 0,
	CPUPR_PARKED = 1,
	CPUPR_EXITING = 2,
	CPUPR_DEAD = 3,
	CPUPR_FAULTED = 4,
	CPUPR_WORKLOAD = 5,
	CPUPR_DETACHING = 6,
	CPUPR_NR_STATES
};

/**
 * struct cpupr_global_ser - Global FLB serialization header
 * @flags: Reserved for future extension; must be zero.
 * @size: the size of the struct.
 * @text_runtime_pa: Physical address of preserved CPU runtime text.
 * @text_runtime_size: Size of preserved CPU runtime text in bytes.
 * @data_runtime_pa: Physical address of preserved CPU runtime data.
 * @data_runtime_size: Size of preserved CPU runtime data in bytes.
 * @cpus: Preservation pointer to the struct cpupr_ser of
 *        the first CPU handed over.
 * @arch_mode: Modes of the outgoing kernel that the parked CPUs
 *             depend on, as arch_cpupr_mode() returns them.
 *
 * The outgoing kernel hands over the CPUs whose files it froze, on a list
 * linked through cpupr_ser.next.  The list lets the incoming kernel
 * find each parked CPU's state at early boot, before any file is retrieved.
 * The incoming kernel refuses the handover unless it runs in @arch_mode.
 */
struct cpupr_global_ser {
	u32 flags;
	u32 size;
	u64 text_runtime_pa;
	u64 text_runtime_size;
	u64 data_runtime_pa;
	u64 data_runtime_size;
	DECLARE_KHOSER_PTR(cpus, struct cpupr_ser *);
	struct cpupr_arch_mode_ser arch_mode;
};
static_assert(sizeof(struct cpupr_global_ser) == 56);

#define CPUPR_FOLIOS_PER_BLOCK	((PAGE_SIZE - 3 * sizeof(u64)) / sizeof(u64))

/**
 * struct cpupr_folio_block_ser - Page-sized block of preserved folio PAs
 * @flags: Reserved for future extension; must be zero.
 * @size: the size of the struct.
 * @next: Preservation pointer to the next block in the chain, or 0.
 * @count: Number of valid entries in @pa.
 * @pa: Physical addresses of KHO-preserved folios.
 */
struct cpupr_folio_block_ser {
	u32 flags;
	u32 size;
	DECLARE_KHOSER_PTR(next, struct cpupr_folio_block_ser *);
	u64 count;
	u64 pa[CPUPR_FOLIOS_PER_BLOCK];
};
static_assert(sizeof(struct cpupr_folio_block_ser) == PAGE_SIZE);

/**
 * struct cpupr_as_ser - Serialized preserved address space metadata
 * @flags: Reserved for future extension; must be zero.
 * @size: The size of the struct.
 * @pgd_pa: Physical address of the root page table (PGD).
 * @folios: Preservation pointer to the first struct cpupr_folio_block_ser
 *          of a linked list of blocks containing the physical addresses of
 *          all KHO-preserved folios owned by the isolated address space: its
 *          page-table pages and everything mapped into it except the
 *          preserved runtime text and data, such as the stacks and
 *          descriptors of its CPUs (struct cpupr_ser), session
 *          and workload runtime structures, and pages adopted from another
 *          owner.
 *
 * The CPUs of a session are the handed-over CPUs whose descriptors point to
 * its address space.  The incoming kernel frees the folios once every CPU of
 * the session has left the address space.
 */
struct cpupr_as_ser {
	u32 flags;
	u32 size;
	u64 pgd_pa;
	DECLARE_KHOSER_PTR(folios, struct cpupr_folio_block_ser *);
};
static_assert(sizeof(struct cpupr_as_ser) == 24);

/**
 * struct cpupr_ser - Serialized state for preserved CPU
 * @flags: Reserved for future extension; must be zero.
 * @size: The size of the struct.
 * @state: Preserved workload state (enum cpupr_workload).
 * @reserved: Reserved for future extension; must be zero.
 * @stack_pa: Physical address of this CPU's preserved stack.
 * @hwid: Hardware identifier of the CPU, as arch_cpupr_hwid() returns.
 * @next: Preservation pointer to the next CPU handed over, or 0.
 * @as: Preservation pointer to the isolated address space of the
 *      session of the CPU.
 *
 * The incoming kernel matches @hwid against its present CPUs and refuses the
 * handover unless an offline present CPU has hardware identifier @hwid.
 */
struct cpupr_ser {
	u32 flags;
	u32 size;
	u32 state;
	u32 reserved;
	u64 stack_pa;
	u64 hwid;
	DECLARE_KHOSER_PTR(next, struct cpupr_ser *);
	DECLARE_KHOSER_PTR(as, struct cpupr_as_ser *);
};
static_assert(sizeof(struct cpupr_ser) == 48);

#endif /* _LINUX_KHO_ABI_CPU_H */
