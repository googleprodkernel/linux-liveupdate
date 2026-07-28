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
#include <linux/bits.h>
#include <linux/kho/abi/kexec_handover.h>

/**
 * DOC: KVM and guest_memfd Live Update ABI
 *
 * KVM and guest_memfd use the ABI defined below for preserving their states
 * across a kexec reboot using the LUO.
 *
 * The state is serialized into packed structures (struct kvm_luo_ser and
 * struct guest_memfd_luo_ser) which are handed over to the next kernel via
 * the KHO mechanism.
 *
 * This interface is a contract. Any modification to the structure layouts
 * constitutes a breaking change. Such changes require incrementing the
 * version number in the KVM_LUO_FH_COMPATIBLE or
 * GUEST_MEMFD_LUO_FH_COMPATIBLE compatibility strings.
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

/**
 * struct guest_memfd_luo_folio_ser - Serialization layout for a single folio in guest_memfd.
 * @pfn:   Page Frame Number of the folio.
 * @index: Page offset of the folio within the file.
 * @flags: State flags associated with the folio.
 */
struct guest_memfd_luo_folio_ser {
	u64 pfn:52;
	u64 flags:12;
	u64 index;
} __packed;

/**
 * GUEST_MEMFD_LUO_FOLIO_UPTODATE - The folio is up-to-date.
 *
 * This flag is per folio to check if the folio is uptodate.
 */
#define GUEST_MEMFD_LUO_FOLIO_UPTODATE	BIT(0)


/**
 * GUEST_MEMFD_LUO_FLAG_MMAP - The guest_memfd supports mmap.
 *
 * This flag indicates that the guest_memfd supports host-side mmap.
 */
#define GUEST_MEMFD_LUO_FLAG_MMAP		BIT(0)

/**
 * GUEST_MEMFD_LUO_FLAG_INIT_SHARED - Initialize memory as shared.
 *
 * This flag indicates that the guest_memfd has been initialized as shared
 * memory.
 */
#define GUEST_MEMFD_LUO_FLAG_INIT_SHARED	BIT(1)

/**
 * GUEST_MEMFD_LUO_SUPPORTED_FLAGS - Supported guest_memfd LUO flags mask.
 *
 * A mask of all guest_memfd preservation flags supported by this version
 * of the KVM LUO ABI.
 */
#define GUEST_MEMFD_LUO_SUPPORTED_FLAGS	(GUEST_MEMFD_LUO_FLAG_MMAP | \
						 GUEST_MEMFD_LUO_FLAG_INIT_SHARED)

/**
 * struct guest_memfd_luo_ser - Main serialization structure for guest_memfd.
 * @size:      The size of the file in bytes.
 * @flags:     File-level flags.
 * @nr_folios: Number of folios in the folios array.
 * @vm_token:  Token of the associated KVM VM instance.
 * @folios:    KHO vmalloc descriptor pointing to the array of
 *             struct guest_memfd_luo_folio_ser.
 */
struct guest_memfd_luo_ser {
	u64 size;
	u64 flags;
	u64 nr_folios;
	u64 vm_token;
	struct kho_vmalloc folios;
} __packed;

/* The compatibility string for GUEST_MEMFD file handler */
#define GUEST_MEMFD_LUO_FH_COMPATIBLE	"guest_memfd_luo_v1"

#endif /* _LINUX_KHO_ABI_KVM_H */
