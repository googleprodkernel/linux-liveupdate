/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Low-level assembly routines for ARM64 physical CPU preservation.
 *
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#include <linux/linkage.h>
#include <asm/assembler.h>

	.text

/*
 * __arch_cpupr_park_on_stack - Switch stack and enter park loop
 * @cpu: Logical CPU identifier (x0)
 * @stack_top: Top of runtime allocated stack (x1)
 */
SYM_FUNC_START(__arch_cpupr_park_on_stack)
	mov	sp, x1
	mov	x19, x0
	bl	cpupr_park_loop
	mov	x0, x19
	bl	arch_cpupr_park_finish
	b	.
SYM_FUNC_END(__arch_cpupr_park_on_stack)

/*
 * arch_cpupr_dcache_clean - Clean and invalidate data cache to PoC
 * @start: Virtual start address (x0)
 * @end: Virtual end address (x1)
 */
SYM_FUNC_START(arch_cpupr_dcache_clean)
	dsb	sy
	raw_dcache_line_size x2, x3
	dcache_by_myline_op_nosync civac, x0, x1, x2, x3
	dsb	sy
	ret
SYM_FUNC_END(arch_cpupr_dcache_clean)
