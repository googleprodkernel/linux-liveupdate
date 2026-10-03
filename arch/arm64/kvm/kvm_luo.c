// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * ARM64 KVM LUO preservation and retrieval handlers.
 */

#include <linux/kho/abi/kvm.h>
#include <linux/kvm_host.h>

#include <asm/kvm_mmu.h>

int kvm_arch_vm_luo_preserve(struct kvm *kvm, struct kvm_luo_ser *ser)
{
	ser->type = kvm_phys_shift(&kvm->arch.mmu);
	if (kvm_vm_is_protected(kvm))
		ser->type |= KVM_VM_TYPE_ARM_PROTECTED;

	return 0;
}
