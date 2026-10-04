// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Isolated runtime execution loop for preserved physical CPUs across Live Update.
 */

#include <linux/atomic.h>
#include <linux/cpu_preserve.h>
#include <linux/kho/abi/cpu.h>
#include <linux/types.h>

/**
 * cpupr_set_dead - Mark the current preserved CPU as fully stopped
 */
void cpupr_set_dead(void)
{
	struct cpupr_stack_context *sctx = cpupr_get_stack_context();
	u32 old_state;

	cpupr_clean(sctx->ser);
	old_state = READ_ONCE(sctx->ser->state);
	while (!cpupr_state_is_stopped(old_state)) {
		if (try_cmpxchg_release(&sctx->ser->state, &old_state, CPUPR_DEAD))
			break;
	}
	cpupr_clean(sctx->ser);
}

/**
 * cpupr_park_loop - Generic execution loop for a parked preserved CPU
 * @cpu: Logical CPU identifier.
 */
void cpupr_park_loop(int cpu)
{
	struct cpupr_stack_context *sctx = cpupr_get_stack_context();
	struct cpupr_ser *ser = sctx->ser;

	arch_cpupr_park_init(cpu);
	if (cmpxchg_release(&ser->state, CPUPR_PARKING,
			    CPUPR_PARKED) == CPUPR_PARKING) {
		cpupr_clean(ser);
	}

	for (;;) {
		/* Pairs with the fully ordered cmpxchg in cpupr_signal_exit() */
		switch (smp_load_acquire(&ser->state)) {
		case CPUPR_EXITING:
			return;
		default:
			arch_cpupr_park_wait();
			break;
		}
	}
}
