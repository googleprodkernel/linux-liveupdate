// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * x86 KVM LUO architectural preservation and retrieval handlers.
 */

#include <linux/cpu.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/kvm.h>
#include <linux/kho/abi/kvm_x86.h>
#include <linux/kvm_host.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <asm/fpu/api.h>
#include <asm/fpu/xcr.h>
#include <linux/mem_encrypt.h>
#include <asm/virt.h>

#include "cpuid.h"
#include "fpu.h"
#include "lapic.h"
#include "mmu.h"
#include "msrs.h"
#include "pmu.h"
#include "regs.h"
#include "x86.h"

int kvm_arch_vm_luo_preserve(struct kvm *kvm, struct kvm_luo_ser *ser)
{
	if (kvm->arch.vm_type != KVM_X86_DEFAULT_VM &&
	    kvm->arch.vm_type != KVM_X86_SW_PROTECTED_VM)
		return -EOPNOTSUPP;

	ser->type = kvm->arch.vm_type;
	return 0;
}

static int kvm_arch_vcpu_luo_check_preconditions(struct kvm_vcpu *vcpu,
						 unsigned int *max_msrs_out)
{
	unsigned int max_msrs;
	int ret;

	/*
	 * Confidential guests keep their vCPU register and FPU state in
	 * encrypted/protected memory (e.g. SEV-ES VMSA or TDX TDVPS) that KVM
	 * cannot read or restore across live update.  The guest FPU state also
	 * travels in @xsave, which requires XSAVE support on the host.
	 */
	if (vcpu->arch.guest_state_protected ||
	    fpstate_is_confidential(&vcpu->arch.guest_fpu) ||
	    !boot_cpu_has(X86_FEATURE_XSAVE))
		return -EOPNOTSUPP;

	/* Nested virtualisation state does not survive the handover. */
	if (is_guest_mode(vcpu) || (vcpu->arch.efer & EFER_SVME) ||
	    kvm_is_cr4_bit_set(vcpu, X86_CR4_VMXE))
		return -EOPNOTSUPP;

	if (kvm_nested_ops.enabled && kvm_nested_ops.get_state) {
		int nested_size = kvm_nested_ops.get_state(vcpu, NULL, 0);

		if (nested_size < 0)
			return nested_size;
		if (nested_size > sizeof(struct kvm_nested_state))
			return -EOPNOTSUPP;
	}

	/*
	 * @xsave is a fixed-size struct kvm_xsave.  A guest whose XSAVE area
	 * outgrows it -- AMX today -- needs KVM_GET_XSAVE2 and would otherwise
	 * be truncated silently by fpu_copy_guest_fpstate_to_uabi() below.
	 * This mirrors the check kvm_arch_vcpu_ioctl() makes for KVM_GET_XSAVE.
	 */
	if (vcpu->arch.guest_fpu.uabi_size > sizeof(struct kvm_xsave))
		return -EOPNOTSUPP;

	if (unlikely(vcpu->arch.complete_userspace_io)) {
		int (*cui)(struct kvm_vcpu *) = vcpu->arch.complete_userspace_io;

		vcpu_load(vcpu);
		kvm_vcpu_srcu_read_lock(vcpu);
		vcpu->arch.complete_userspace_io = NULL;
		ret = cui(vcpu);
		kvm_vcpu_srcu_read_unlock(vcpu);
		vcpu_put(vcpu);
		if (ret <= 0)
			return ret < 0 ? ret : -EBUSY;
	}

	max_msrs = kvm_num_msrs_to_save();
	if (max_msrs > KVM_X86_SER_MAX_MSRS)
		return -E2BIG;

	*max_msrs_out = max_msrs;
	return 0;
}

static int kvm_arch_vcpu_luo_save_state(struct kvm_vcpu *vcpu,
					struct kvm_vcpu_arch_ser *state,
					phys_addr_t state_phys)
{
	struct kvm_cpuid2 *cpuid;
	struct kvm_msrs *msrs;
	int ret;

	/*
	 * kvm_arch_vcpu_ioctl_{get,set}_mpstate() take the vCPU themselves, so
	 * they have to be called outside the vcpu_load() region below:
	 * vcpu_load() registers a preempt notifier and is not reentrant.
	 * Calling kvm_arch_vcpu_ioctl_get_mpstate() first also runs
	 * kvm_apic_accept_events() so any pending INIT/SIPI is folded into
	 * mp_state and architectural registers before they are saved below.
	 */
	ret = kvm_arch_vcpu_ioctl_get_mpstate(vcpu, &state->mp_state);
	if (ret)
		return ret;
	state->pad = 0;
	memset(state->pad_xsave, 0, sizeof(state->pad_xsave));

	vcpu_load(vcpu);

	__get_regs(vcpu, &state->regs);
	kvm_vcpu_ioctl_x86_get_sregs2(vcpu, &state->sregs);
	ret = kvm_vcpu_ioctl_x86_get_xcrs(vcpu, &state->xcrs);
	if (ret)
		goto err_put;

	if (boot_cpu_has(X86_FEATURE_XSAVE)) {
		ret = kvm_vcpu_ioctl_x86_get_xsave(vcpu, &state->xsave);
		if (ret)
			goto err_put;
	}

	ret = kvm_vcpu_ioctl_x86_get_debugregs(vcpu, &state->debugregs);
	if (ret)
		goto err_put;

	if (vcpu->vcpu_id == 0) {
		kvm_get_clock(vcpu->kvm, &state->clock);
		if (kho_is_ro()) {
			state->clock.flags &= ~KVM_CLOCK_REALTIME;
			state->clock.realtime = 0;
		}
	} else {
		memset(&state->clock, 0, sizeof(state->clock));
	}

	kvm_vcpu_ioctl_x86_get_vcpu_events(vcpu, &state->events);

	/*
	 * A hardware maskable interrupt can only be queued for injection when
	 * RFLAGS.IF is set.  If RFLAGS.IF is already clear, the interrupt gate
	 * delivery has already completed (e.g., under QEMU TCG where
	 * exit_int_info can remain set across the first instruction of the
	 * handler), so clear any stale injected flag in the saved state to
	 * avoid delivering the same interrupt a second time with RFLAGS.IF == 0
	 * before SWAPGS, without mutating the live vCPU if preservation is
	 * later cancelled.
	 */
	if (state->events.interrupt.injected && !state->events.interrupt.soft &&
	    !(state->regs.rflags & X86_EFLAGS_IF))
		state->events.interrupt.injected = 0;

	if (lapic_in_kernel(vcpu)) {
		ret = kvm_vcpu_ioctl_get_lapic(vcpu, &state->lapic);
		if (ret)
			goto err_put;
	}

	msrs = (void *)(state + 1);
	kvm_msrs_save(vcpu, msrs);
	state->msrs.phys = state_phys + ((void *)msrs - (void *)state);

	cpuid = (void *)&msrs->entries[msrs->nmsrs];
	kvm_get_cpuid(vcpu, cpuid);
	state->cpuid.phys = state_phys + ((void *)cpuid - (void *)state);

	vcpu_put(vcpu);
	return 0;

err_put:
	vcpu_put(vcpu);
	return ret;
}

int kvm_arch_vcpu_luo_preserve(struct kvm_vcpu *vcpu, struct kvm_vcpu_ser *ser)
{
	struct kvm_vcpu_arch_ser *state;
	struct kvm_cpuid2 *cpuid;
	struct kvm_msrs *msrs;
	unsigned int max_msrs;
	size_t size;
	int ret;

	ret = kvm_arch_vcpu_luo_check_preconditions(vcpu, &max_msrs);
	if (ret)
		return ret;

	size = sizeof(*state) + struct_size(msrs, entries, max_msrs) +
	       struct_size(cpuid, entries, KVM_MAX_CPUID_ENTRIES);
	state = kho_alloc_preserve(size);
	if (IS_ERR(state))
		return PTR_ERR(state);

	ret = kvm_arch_vcpu_luo_save_state(vcpu, state, virt_to_phys(state));
	if (ret) {
		kho_unpreserve_free(state);
		return ret;
	}

	KHOSER_STORE_PTR(ser->arch_state, state);
	return 0;
}

static int kvm_arch_vcpu_luo_restore_state(struct kvm_vcpu *vcpu,
					   struct kvm_vcpu_arch_ser *state,
					   phys_addr_t state_phys)
{
	struct kvm_cpuid2 *cpuid = NULL;
	struct kvm_msrs *msrs;
	int ret;

	if (state->pad ||
	    memchr_inv(state->pad_xsave, 0, sizeof(state->pad_xsave)))
		return -EINVAL;

	if (!state->msrs.phys ||
	    state->msrs.phys != state_phys + sizeof(*state))
		return -EINVAL;
	msrs = (struct kvm_msrs *)(state + 1);
	if (msrs->pad || msrs->nmsrs > KVM_X86_SER_MAX_MSRS)
		return -EINVAL;

	if (state->cpuid.phys) {
		size_t cpuid_off = (void *)&msrs->entries[msrs->nmsrs] -
				   (void *)state;

		if (state->cpuid.phys != state_phys + cpuid_off)
			return -EINVAL;
		cpuid = (struct kvm_cpuid2 *)&msrs->entries[msrs->nmsrs];
		if (cpuid->padding || cpuid->nent > KVM_MAX_CPUID_ENTRIES)
			return -EINVAL;
	}

	vcpu_load(vcpu);

	if (cpuid && cpuid->nent > 0) {
		struct kvm_cpuid_entry2 *entries;

		entries = kvmemdup(cpuid->entries,
				   flex_array_size(cpuid, entries, cpuid->nent),
				   GFP_KERNEL);
		if (!entries) {
			ret = -ENOMEM;
			goto out;
		}

		ret = kvm_set_cpuid(vcpu, entries, cpuid->nent);
		if (ret) {
			kvfree(entries);
			goto out;
		}
	}

	/*
	 * kvm_msrs_restore() intentionally ignores per-MSR errors from
	 * do_set_msr() because the discovery list (msrs_to_save +
	 * emulated_msrs) includes feature-dependent MSRs that succeed on
	 * host-initiated read with ignore_msrs=true but are rejected on write
	 * when the corresponding guest feature is not enabled.
	 */
	kvm_msrs_restore(vcpu, msrs, true);

	__set_regs(vcpu, &state->regs);

	ret = kvm_vcpu_ioctl_x86_set_sregs2(vcpu, &state->sregs);
	if (ret)
		goto out;

	if (boot_cpu_has(X86_FEATURE_XSAVE)) {
		ret = kvm_vcpu_ioctl_x86_set_xcrs(vcpu, &state->xcrs);
		if (ret)
			goto out;

		ret = kvm_vcpu_ioctl_x86_set_xsave(vcpu, &state->xsave);
		if (ret)
			goto out;
	}

	ret = kvm_vcpu_ioctl_x86_set_debugregs(vcpu, &state->debugregs);
	if (ret)
		goto out;

	if (lapic_in_kernel(vcpu)) {
		ret = kvm_vcpu_ioctl_set_lapic(vcpu, &state->lapic);
		if (ret)
			goto out;
	}

	kvm_msrs_restore(vcpu, msrs, false);

	kvm_vcpu_srcu_read_lock(vcpu);
	ret = kvm_vcpu_ioctl_x86_set_vcpu_events(vcpu, &state->events);
	kvm_vcpu_srcu_read_unlock(vcpu);
	if (ret)
		goto out;

	if (vcpu->vcpu_id == 0) {
		ret = kvm_set_clock(vcpu->kvm, &state->clock);
		if (ret)
			goto out;
	}

	ret = 0;
out:
	vcpu_put(vcpu);

	/* Takes the vCPU itself; see the comment in the preserve path. */
	if (!ret)
		ret = kvm_arch_vcpu_ioctl_set_mpstate(vcpu, &state->mp_state);

	return ret;
}

int kvm_arch_vcpu_luo_retrieve(struct kvm_vcpu *vcpu, struct kvm_vcpu_ser *ser)
{
	struct kvm_vcpu_arch_ser *state;

	if (!ser->arch_state.phys)
		return -EINVAL;

	state = KHOSER_LOAD_PTR(ser->arch_state);
	return kvm_arch_vcpu_luo_restore_state(vcpu, state,
					       ser->arch_state.phys);
}

#ifdef CONFIG_KEXEC_HANDOVER_READONLY
int kvm_arch_vcpu_luo_preserve_ro(struct kvm_vcpu *vcpu,
				  const struct kvm_vcpu_ser *ser)
{
	struct kvm_vcpu_arch_ser *state;
	unsigned int max_msrs;
	int ret;

	if (!ser->arch_state.phys)
		return -EINVAL;

	ret = kvm_arch_vcpu_luo_check_preconditions(vcpu, &max_msrs);
	if (ret)
		return ret;

	state = vcpu->luo_ro_arch_state ?: kho_ro_map_folio(ser->arch_state.phys,
							    PAGE_KERNEL);
	if (!state)
		return -EINVAL;
	vcpu->luo_ro_arch_state = state;

	return kvm_arch_vcpu_luo_save_state(vcpu, state, ser->arch_state.phys);
}

int kvm_arch_vcpu_luo_retrieve_ro(struct kvm_vcpu *vcpu,
				  const struct kvm_vcpu_ser *ser)
{
	struct kvm_vcpu_arch_ser *state;

	if (!ser->arch_state.phys)
		return -EINVAL;

	/*
	 * Map read-write because kvm_vcpu_ioctl_x86_set_sregs2() ->
	 * set_sregs_common() updates sregs->cs.unusable in place.
	 */
	state = vcpu->luo_ro_arch_state ?: kho_ro_map_folio(ser->arch_state.phys,
							    PAGE_KERNEL);
	if (!state)
		return -EINVAL;
	vcpu->luo_ro_arch_state = state;

	return kvm_arch_vcpu_luo_restore_state(vcpu, state,
					       ser->arch_state.phys);
}
#endif /* CONFIG_KEXEC_HANDOVER_READONLY */

void kvm_arch_vcpu_luo_unpreserve(struct kvm_vcpu_ser *ser)
{
	struct kvm_vcpu_arch_ser *state = KHOSER_LOAD_PTR(ser->arch_state);

	if (state) {
		kho_unpreserve_free(state);
		ser->arch_state.phys = 0;
	}
}

void kvm_arch_vcpu_luo_finish(struct kvm_vcpu_ser *ser)
{
	struct kvm_vcpu_arch_ser *state = KHOSER_LOAD_PTR(ser->arch_state);

	if (state) {
		kho_restore_free(state);
		ser->arch_state.phys = 0;
	}
}
