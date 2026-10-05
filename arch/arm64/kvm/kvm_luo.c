// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * ARM64 KVM LUO preservation and retrieval handlers.
 */

#include <linux/kexec_handover.h>
#include <linux/kho/abi/kvm.h>
#include <linux/kho/abi/kvm_arm64.h>
#include <linux/kvm_host.h>
#include <linux/slab.h>

#include <asm/kvm_emulate.h>
#include <asm/kvm_mmu.h>
#include <kvm/arm_vgic.h>

#include "sys_regs.h"
#include "vgic/vgic.h"

int kvm_arch_vm_luo_preserve(struct kvm *kvm, struct kvm_luo_ser *ser)
{
	if (kvm_vm_is_protected(kvm))
		return -EOPNOTSUPP;

	ser->type = kvm_phys_shift(&kvm->arch.mmu);

	return 0;
}

static void kvm_arm_luo_get_regs(struct kvm_vcpu *vcpu, struct kvm_regs *regs)
{
	regs->regs = vcpu->arch.ctxt.regs;
	regs->sp_el1 = ctxt_sys_reg(&vcpu->arch.ctxt, SP_EL1);
	regs->elr_el1 = ctxt_sys_reg(&vcpu->arch.ctxt, ELR_EL1);
	regs->spsr[KVM_SPSR_EL1] = ctxt_sys_reg(&vcpu->arch.ctxt, SPSR_EL1);
	regs->spsr[KVM_SPSR_ABT] = vcpu->arch.ctxt.spsr_abt;
	regs->spsr[KVM_SPSR_UND] = vcpu->arch.ctxt.spsr_und;
	regs->spsr[KVM_SPSR_IRQ] = vcpu->arch.ctxt.spsr_irq;
	regs->spsr[KVM_SPSR_FIQ] = vcpu->arch.ctxt.spsr_fiq;
	regs->fp_regs = vcpu->arch.ctxt.fp_regs;
}

static void kvm_arm_luo_set_regs(struct kvm_vcpu *vcpu, const struct kvm_regs *regs)
{
	vcpu->arch.ctxt.regs = regs->regs;
	ctxt_sys_reg(&vcpu->arch.ctxt, SP_EL1) = regs->sp_el1;
	ctxt_sys_reg(&vcpu->arch.ctxt, ELR_EL1) = regs->elr_el1;
	ctxt_sys_reg(&vcpu->arch.ctxt, SPSR_EL1) = regs->spsr[KVM_SPSR_EL1];
	vcpu->arch.ctxt.spsr_abt = regs->spsr[KVM_SPSR_ABT];
	vcpu->arch.ctxt.spsr_und = regs->spsr[KVM_SPSR_UND];
	vcpu->arch.ctxt.spsr_irq = regs->spsr[KVM_SPSR_IRQ];
	vcpu->arch.ctxt.spsr_fiq = regs->spsr[KVM_SPSR_FIQ];
	vcpu->arch.ctxt.fp_regs = regs->fp_regs;
}

int kvm_arch_vcpu_luo_preserve(struct kvm_vcpu *vcpu, struct kvm_vcpu_ser *ser)
{
	struct kvm_arm64_sysregs_ser *sysregs;
	struct kvm_vcpu_arch_ser *state;
	u64 *indices;
	int num_sysregs;
	size_t size;
	int ret;
	int i;

	if (!kvm_vcpu_initialized(vcpu))
		return -EINVAL;

	if (vcpu_has_nv(vcpu) || kvm_vm_is_protected(vcpu->kvm))
		return -EOPNOTSUPP;

	if (vcpu->mmio_needed) {
		ret = kvm_handle_mmio_return(vcpu);
		if (ret <= 0)
			return ret < 0 ? ret : -EBUSY;
	}

	if (kvm_check_request(KVM_REQ_VCPU_RESET, vcpu))
		kvm_reset_vcpu(vcpu);

	if (unlikely(vcpu_get_flag(vcpu, PENDING_EXCEPTION) ||
		     vcpu_get_flag(vcpu, INCREMENT_PC))) {
		vcpu_load(vcpu);
		kvm_call_hyp(__kvm_adjust_pc, vcpu);
		vcpu_put(vcpu);
	}

	num_sysregs = kvm_arm_get_sys_reg_indices(vcpu, NULL, 0);
	if (num_sysregs < 0)
		return num_sysregs;
	if (num_sysregs > KVM_ARM64_SER_MAX_SYSREGS)
		return -E2BIG;

	indices = kmalloc_array(num_sysregs, sizeof(*indices), GFP_KERNEL);
	if (!indices)
		return -ENOMEM;

	num_sysregs = kvm_arm_get_sys_reg_indices(vcpu, indices, num_sysregs);
	if (num_sysregs < 0) {
		kfree(indices);
		return num_sysregs;
	}

	size = sizeof(*state) + struct_size(sysregs, sysregs, num_sysregs);
	state = kho_alloc_preserve(size);
	if (IS_ERR(state)) {
		kfree(indices);
		return PTR_ERR(state);
	}

	/* Core general-purpose and FP registers (uAPI struct kvm_regs) */
	kvm_arm_luo_get_regs(vcpu, &state->regs);

	/* Multiprocessor state (uAPI struct kvm_mp_state) */
	kvm_arch_vcpu_ioctl_get_mpstate(vcpu, &state->mp_state);

	/* Exception / SError injection events (uAPI struct kvm_vcpu_events) */
	__kvm_arm_vcpu_get_events(vcpu, &state->events);

	/* CPU target and feature configuration (uAPI struct kvm_vcpu_init) */
	state->init.target = KVM_ARM_TARGET_GENERIC_V8;
	bitmap_to_arr32(state->init.features, vcpu->kvm->arch.vcpu_features,
			KVM_VCPU_MAX_FEATURES);

	/* System registers (uAPI struct kvm_one_reg array) */
	sysregs = (void *)(state + 1);
	for (i = 0; i < num_sysregs; i++) {
		u64 val;

		if (kvm_arm_sys_reg_read(vcpu, indices[i], &val) == 0) {
			sysregs->sysregs[sysregs->num_sysregs].id = indices[i];
			sysregs->sysregs[sysregs->num_sysregs].addr = val;
			sysregs->num_sysregs++;
		}
	}
	kfree(indices);
	KHOSER_STORE_PTR(state->sysregs, sysregs);

	KHOSER_STORE_PTR(ser->arch_state, state);

	return 0;
}

int kvm_arch_vcpu_luo_retrieve(struct kvm_vcpu *vcpu, struct kvm_vcpu_ser *ser)
{
	struct kvm_arm64_sysregs_ser *sysregs;
	struct kvm_vcpu_arch_ser *state;
	int ret;
	int i;

	if (!ser->arch_state.phys)
		return -EINVAL;

	state = KHOSER_LOAD_PTR(ser->arch_state);
	if (state->pad || !state->sysregs.phys)
		return -EINVAL;

	sysregs = KHOSER_LOAD_PTR(state->sysregs);
	if (!sysregs || sysregs != (struct kvm_arm64_sysregs_ser *)(state + 1) ||
	    sysregs->reserved || sysregs->num_sysregs > KVM_ARM64_SER_MAX_SYSREGS)
		return -EINVAL;

	/* Restore vCPU target and features */
	ret = kvm_arm_vcpu_init(vcpu, &state->init);
	if (ret)
		return ret;

	/* Restore core registers */
	kvm_arm_luo_set_regs(vcpu, &state->regs);

	if (irqchip_in_kernel(vcpu->kvm) &&
	    vcpu->kvm->arch.vgic.vgic_model == KVM_DEV_TYPE_ARM_VGIC_V3) {
		vgic_v3_reset(vcpu);
	}

	/* Restore system registers */
	for (i = 0; i < sysregs->num_sysregs; i++) {
		ret = kvm_arm_sys_reg_write(vcpu, sysregs->sysregs[i].id,
					    sysregs->sysregs[i].addr);
		if (ret)
			return ret;
	}

	/* Restore multiprocessor execution state */
	ret = kvm_arch_vcpu_ioctl_set_mpstate(vcpu, &state->mp_state);
	if (ret)
		return ret;

	/* Restore exception / SError injection events */
	ret = __kvm_arm_vcpu_set_events(vcpu, &state->events);
	if (ret)
		return ret;

	if (irqchip_in_kernel(vcpu->kvm)) {
		struct vgic_irq *irq;
		unsigned long flags;

		vcpu->arch.vgic_cpu.vgic_cpu_luo_restored = true;
		vcpu->kvm->arch.vgic.enabled = true;

		irq = vgic_get_vcpu_irq(vcpu, timer_irq(vcpu_vtimer(vcpu)));
		if (irq) {
			raw_spin_lock_irqsave(&irq->irq_lock, flags);
			irq->enabled = true;
			raw_spin_unlock_irqrestore(&irq->irq_lock, flags);
			vgic_put_irq(vcpu->kvm, irq);
		}
		irq = vgic_get_vcpu_irq(vcpu, timer_irq(vcpu_ptimer(vcpu)));
		if (irq) {
			raw_spin_lock_irqsave(&irq->irq_lock, flags);
			irq->enabled = true;
			raw_spin_unlock_irqrestore(&irq->irq_lock, flags);
			vgic_put_irq(vcpu->kvm, irq);
		}
	}

	return 0;
}

void kvm_arch_vcpu_luo_unpreserve(struct kvm_vcpu_ser *ser)
{
	if (ser->arch_state.phys) {
		kho_unpreserve_free(KHOSER_LOAD_PTR(ser->arch_state));
		ser->arch_state.phys = 0;
	}
}

void kvm_arch_vcpu_luo_finish(struct kvm_vcpu_ser *ser)
{
	if (ser->arch_state.phys) {
		kho_restore_free(KHOSER_LOAD_PTR(ser->arch_state));
		ser->arch_state.phys = 0;
	}
}
