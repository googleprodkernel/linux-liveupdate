/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef _LINUX_KHO_ABI_CPU_X86_H
#define _LINUX_KHO_ABI_CPU_X86_H

#ifdef CONFIG_X86_64

#include <linux/build_bug.h>
#include <linux/types.h>

/**
 * struct cpupr_arch_mode_ser - x86 modes that parked CPUs depend on
 * @x2apic:   Non-zero when x2APIC mode is active.
 * @la57:     Non-zero when 5-level paging (LA57) is active.
 * @reserved: Must be zero.
 */
struct cpupr_arch_mode_ser {
	u8 x2apic;
	u8 la57;
	u8 reserved[6];
};

static_assert(sizeof(struct cpupr_arch_mode_ser) == 8);

#endif /* CONFIG_X86_64 */

#endif /* _LINUX_KHO_ABI_CPU_X86_H */
