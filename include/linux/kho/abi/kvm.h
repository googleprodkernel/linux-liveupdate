/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Tarun Sahu <tarunsahu@google.com>
 *
 * KVM Preservation ABI for Live Update Orchestrator (LUO)
 */
#ifndef _LINUX_KHO_ABI_KVM_H
#define _LINUX_KHO_ABI_KVM_H

#include <linux/types.h>
#include <linux/kho/abi/kexec_handover.h>

/**
 * DOC: KVM Live Update ABI
 *
 * KVM uses the ABI defined below for preserving its state
 * across a kexec reboot using the LUO.
 *
 * The state is serialized into a packed structure `struct kvm_luo_ser`
 * which is handed over to the next kernel via the KHO mechanism.
 *
 * This interface is a contract. Any modification to the structure layout
 * constitutes a breaking change. Such changes require incrementing the
 * version number in the KVM_LUO_FH_COMPATIBLE compatibility string.
 */

/**
 * struct kvm_luo_ser - Main serialization structure for a KVM VM.
 * @type:         The type of VM.
 */
struct kvm_luo_ser {
	u64 type;
} __packed;

/* The compatibility string for KVM VM file handler */
#define KVM_LUO_FH_COMPATIBLE	"kvm_vm_luo_v1"

#endif /* _LINUX_KHO_ABI_KVM_H */
