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
 * cpupr_should_exit - Check if the running preserved workload should exit
 *
 * Return: %true if the workload on the current CPU must exit back to the park
 *         loop, %false otherwise.
 */
bool cpupr_should_exit(void)
{
	struct cpupr_stack_context *sctx = cpupr_get_stack_context();

	if (!sctx->ser)
		return false;

	/* Pairs with the release cmpxchg in cpupr_attach_workload() */
	return smp_load_acquire(&sctx->ser->state) != CPUPR_WORKLOAD;
}

static void cpupr_run_workload(struct cpupr_stack_context *sctx)
{
	struct cpupr_ser *ser = sctx->ser;
	void (*fn)(void *data);
	void *arg;
	u32 old;

	fn = READ_ONCE(sctx->entry_fn);
	arg = (void *)(uintptr_t)READ_ONCE(sctx->workload_context);
	/* Pairs with the release cmpxchg in cpupr_attach_workload() */
	if (fn && smp_load_acquire(&ser->state) == CPUPR_WORKLOAD)
		fn(arg);

	/*
	 * Pairs with the fully ordered cmpxchg()es in cpupr_signal_exit() and
	 * cpupr_detach_workload()
	 */
	old = smp_load_acquire(&ser->state);
	while (old == CPUPR_WORKLOAD ||
	       old == CPUPR_DETACHING) {
		if (try_cmpxchg_release(&ser->state, &old,
					CPUPR_PARKED)) {
			cpupr_clean(ser);
			break;
		}
	}
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
		/*
		 * Pairs with the fully ordered cmpxchg in cpupr_signal_exit()
		 * and with the release cmpxchg in cpupr_attach_workload()
		 */
		switch (smp_load_acquire(&ser->state)) {
		case CPUPR_EXITING:
			return;
		case CPUPR_WORKLOAD:
		case CPUPR_DETACHING:
			cpupr_run_workload(sctx);
			break;
		default:
			arch_cpupr_park_wait();
			break;
		}
	}
}
