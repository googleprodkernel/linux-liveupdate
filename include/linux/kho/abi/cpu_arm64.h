/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef _LINUX_KHO_ABI_CPU_ARM64_H
#define _LINUX_KHO_ABI_CPU_ARM64_H

#ifdef CONFIG_ARM64

#include <linux/build_bug.h>
#include <linux/types.h>

/**
 * struct cpupr_arch_mode_ser - arm64 modes that parked CPUs depend on
 * @page_shift:     Page size shift (PAGE_SHIFT).
 * @va_bits:        Virtual address width in bits (VA_BITS).
 * @pgtable_levels: Number of page-table levels (CONFIG_PGTABLE_LEVELS).
 * @lpa2:           Non-zero when FEAT_LPA2 52-bit addressing is active.
 * @vhe:            Non-zero when the kernel runs at EL2 with VHE.
 * @be:             Non-zero when the kernel runs big-endian.
 * @reserved:       Must be zero.
 */
struct cpupr_arch_mode_ser {
	u8 page_shift;
	u8 va_bits;
	u8 pgtable_levels;
	u8 lpa2;
	u8 vhe;
	u8 be;
	u8 reserved[2];
};

static_assert(sizeof(struct cpupr_arch_mode_ser) == 8);

#endif /* CONFIG_ARM64 */

#endif /* _LINUX_KHO_ABI_CPU_ARM64_H */
