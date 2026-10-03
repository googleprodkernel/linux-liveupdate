// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * x86 KVM LUO architectural preservation and retrieval handlers.
 */

#include <linux/kho/abi/kvm.h>
#include <linux/kvm_host.h>

int kvm_arch_vm_luo_preserve(struct kvm *kvm, struct kvm_luo_ser *ser)
{
	ser->type = kvm->arch.vm_type;
	return 0;
}
