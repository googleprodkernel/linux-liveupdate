/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef _LINUX_KHO_ABI_KVM_ARM64_H
#define _LINUX_KHO_ABI_KVM_ARM64_H

#ifdef CONFIG_ARM64

#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/kho/abi/kvm.h>
#include <uapi/linux/kvm.h>
#include <uapi/asm/kvm.h>

/**
 * DOC: arm64 KVM vCPU Live Update ABI
 *
 * arm64 KVM uses the ABI defined below for preserving architectural vCPU state
 * across a kexec reboot using LUO.
 *
 * The state is serialized into a packed structure `struct kvm_vcpu_arch_ser`
 * which is handed over to the next kernel via KHO.
 *
 * All sub-structures (struct kvm_regs, struct kvm_mp_state,
 * struct kvm_vcpu_events, struct kvm_vcpu_init, and struct kvm_one_reg)
 * are uAPI contracts.
 *
 * This interface is a contract. Any modification to the structure layout
 * constitutes a breaking change. Such changes require incrementing the version
 * number in the KVM_VCPU_LUO_FH_COMPATIBLE string.
 */

#define KVM_ARM64_SER_MAX_SYSREGS	2048

/**
 * struct kvm_arm64_sysregs_ser - Serialized arm64 guest system registers.
 * @num_sysregs: Number of serialized system registers in @sysregs.
 * @reserved:    Reserved padding for 64-bit alignment; must be zero.
 * @sysregs:     Guest architectural system registers (uAPI struct kvm_one_reg array).
 */
struct kvm_arm64_sysregs_ser {
	u32 num_sysregs;
	u32 reserved;
	struct kvm_one_reg sysregs[];
} __packed;

static_assert(offsetof(struct kvm_arm64_sysregs_ser, sysregs) % sizeof(u64) == 0,
	      "sysregs must be 64-bit aligned");

/**
 * struct kvm_vcpu_arch_ser - Preserved arm64 architectural vCPU state in RAM.
 * @regs:     Core general-purpose and floating-point registers (uAPI struct kvm_regs).
 * @mp_state: Multiprocessor execution state (uAPI struct kvm_mp_state).
 * @pad:      Padding to maintain 64-bit alignment after mp_state.
 * @events:   Exception and SError injection state (uAPI struct kvm_vcpu_events).
 * @init:     Target CPU and vCPU feature bitmap (uAPI struct kvm_vcpu_init).
 * @sysregs:  Preservation pointer to guest architectural system registers.
 */
struct kvm_vcpu_arch_ser {
	struct kvm_regs regs;
	struct kvm_mp_state mp_state;
	u32 pad;
	struct kvm_vcpu_events events;
	struct kvm_vcpu_init init;
	DECLARE_KHOSER_PTR(sysregs, struct kvm_arm64_sysregs_ser *);
} __packed;

static_assert(offsetof(struct kvm_vcpu_arch_ser, sysregs) % sizeof(u64) == 0,
	      "sysregs pointer must be 64-bit aligned");

#endif /* CONFIG_ARM64 */

#endif /* _LINUX_KHO_ABI_KVM_ARM64_H */
