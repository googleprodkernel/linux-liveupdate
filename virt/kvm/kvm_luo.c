// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * Tarun Sahu <tarunsahu@google.com>
 *
 * KVM VM Preservation for Live Update Orchestrator (LUO)
 */

/**
 * DOC: KVM VM Preservation via LUO
 *
 * Overview
 * ========
 *
 * KVM virtual machines (VMs) can be preserved over a kexec reboot using the
 * Live Update Orchestrator (LUO) file preservation. This allows userspace
 * to preserve KVM VM state across kexec reboots.
 *
 * The preservation is not intended to be fully transparent. Only specific
 * VM configuration and state are preserved, while other aspects of the VM
 * must be re-established or re-configured by userspace after retrieval.
 *
 * Preserved Properties
 * ====================
 *
 * The following properties of the KVM VM are preserved across kexec:
 *
 * VM Type
 *   The VM type (e.g., on x86 architecture, the vm_type parameter) is
 *   preserved.
 *
 * Non-Preserved Properties
 * ========================
 *
 * The preservation does not cover:
 *
 * - Memspots / Memory slot layout (memslots)
 * - Interrupt controllers and IRQ routings
 * - Coalesced MMIO zones
 * - Device bindings (VFIO/Eventfds)
 * - Active paging or guest registers state
 * - etc
 */
#include <linux/liveupdate.h>
#include <linux/kvm_host.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/err.h>
#include <linux/anon_inodes.h>
#include <linux/magic.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/kho/abi/kvm.h>
#include "kvm_mm.h"

static bool kvm_luo_can_preserve(struct liveupdate_file_handler *handler,
				 struct file *file)
{
	return file_is_kvm(file);
}

static int kvm_luo_preserve(struct liveupdate_file_op_args *args)
{
	struct kvm *kvm = args->file->private_data;
	struct kvm_luo_ser *ser;
	int err;

	mutex_lock(&kvm->lock);
	if (kvm->luo_preserved) {
		mutex_unlock(&kvm->lock);
		return -EBUSY;
	}

	if (kvm->vm_dead || kvm->vm_bugged) {
		mutex_unlock(&kvm->lock);
		return -EINVAL;
	}

	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser)) {
		mutex_unlock(&kvm->lock);
		return PTR_ERR(ser);
	}

	/*
	 * @type is the argument the new kernel will pass to KVM_CREATE_VM, and
	 * only the architecture knows how to spell it.
	 */
	ser->type = 0;
	err = kvm_arch_vm_luo_preserve(kvm, ser);
	if (err) {
		kho_unpreserve_free(ser);
		return err;
	}

	kvm->luo_preserved = true;
	mutex_unlock(&kvm->lock);

	args->serialized_data = virt_to_phys(ser);
	return 0;
}

static atomic_t restored_vm_id = ATOMIC_INIT(0);

#ifdef CONFIG_KEXEC_HANDOVER_READONLY
static int kvm_luo_retrieve_ro(struct liveupdate_file_op_args *args)
{
	char fdname[ITOA_MAX_LEN + 1];
	struct kvm_luo_ser *ser;
	struct file *file;
	struct kvm *kvm;
	u64 type;

	if (!args->serialized_data)
		return -EINVAL;

	ser = kho_ro_map_folio(args->serialized_data, PAGE_KERNEL_RO);
	if (!ser)
		return -EINVAL;

	type = ser->type;

	snprintf(fdname, sizeof(fdname), "%d",
		 atomic_inc_return(&restored_vm_id));

	file = kvm_create_vm_file(type, fdname);
	if (IS_ERR(file))
		return PTR_ERR(file);

	kvm = file->private_data;
	args->file = file;
	kvm_uevent_notify_vm_create(kvm);
	return 0;
}
#else
static int kvm_luo_retrieve_ro(struct liveupdate_file_op_args *args)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_KEXEC_HANDOVER_READONLY */

static int kvm_luo_retrieve(struct liveupdate_file_op_args *args)
{
	char fdname[ITOA_MAX_LEN + 1];
	struct kvm_luo_ser *ser;
	struct file *file;
	struct kvm *kvm;
	int err = 0;

	if (kho_is_ro())
		return kvm_luo_retrieve_ro(args);

	if (!args->serialized_data)
		return -EINVAL;

	ser = phys_to_virt(args->serialized_data);

	snprintf(fdname, sizeof(fdname), "%d",
		 atomic_inc_return(&restored_vm_id));

	file = kvm_create_vm_file(ser->type, fdname);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_free_ser;
	}

	kvm = file->private_data;

	args->file = file;
	kho_restore_free(ser);

	kvm_uevent_notify_vm_create(kvm);
	return 0;

err_free_ser:
	kho_restore_free(ser);
	return err;
}

static void kvm_luo_unpreserve(struct liveupdate_file_op_args *args)
{
	struct kvm *kvm = args->file ? args->file->private_data : NULL;
	struct kvm_luo_ser *ser;

	/*
	 * in case preservation failed, args->serialized_data will
	 * be NULL and kvm_luo_preserve takes care of cleaning up.
	 * If preserve succeeds, this condition fails and unpreserve
	 * function takes care of cleaning up.
	 */
	if (WARN_ON_ONCE(!args->serialized_data))
		return;

	if (kvm) {
		mutex_lock(&kvm->lock);
		kvm->luo_preserved = false;
		mutex_unlock(&kvm->lock);
	}

	ser = phys_to_virt(args->serialized_data);

	kho_unpreserve_free(ser);
}

static void kvm_luo_finish(struct liveupdate_file_op_args *args)
{
	struct kvm_luo_ser *ser;

	/*
	 * If retrieve_status is true or set to error, nothing to do here.
	 * Already cleaned up in kvm_luo_retrieve().
	 */
	if (args->retrieve_status)
		return;

	if (!args->serialized_data)
		return;

	ser = phys_to_virt(args->serialized_data);

	kho_restore_free(ser);
}

static const struct liveupdate_file_ops kvm_luo_file_ops = {
	.can_preserve = kvm_luo_can_preserve,
	.preserve = kvm_luo_preserve,
	.retrieve = kvm_luo_retrieve,
	.unpreserve = kvm_luo_unpreserve,
	.finish = kvm_luo_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler kvm_luo_handler = {
	.ops = &kvm_luo_file_ops,
	.compatible = KVM_LUO_FH_COMPATIBLE,
	.flags = LIVEUPDATE_FH_RO_RETRIEVE,
};

static bool kvm_vcpu_luo_can_preserve(struct liveupdate_file_handler *handler,
				      struct file *file)
{
	return file_is_kvm_vcpu(file);
}

static int kvm_vcpu_luo_preserve(struct liveupdate_file_op_args *args)
{
	struct kvm_vcpu *vcpu = args->file->private_data;
	struct kvm_vcpu_ser *ser;
	struct file *kvm_file;
	u64 vm_token;
	int err;

	if (!mutex_trylock(&vcpu->mutex))
		return -EBUSY;

	if (vcpu->luo_preserved) {
		mutex_unlock(&vcpu->mutex);
		return -EBUSY;
	}

	kvm_file = get_file_active(&vcpu->kvm->vm_file);
	if (!kvm_file) {
		mutex_unlock(&vcpu->mutex);
		return -ENOENT;
	}

	err = liveupdate_get_token_outgoing(args->session, kvm_file, &vm_token);
	fput(kvm_file);
	if (err) {
		mutex_unlock(&vcpu->mutex);
		return err;
	}

	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser)) {
		mutex_unlock(&vcpu->mutex);
		return PTR_ERR(ser);
	}

	ser->vcpu_id = vcpu->vcpu_id;
	ser->reserved = 0;
	ser->vm_token = vm_token;
	ser->arch_state.phys = 0;

	err = kvm_arch_vcpu_luo_preserve(vcpu, ser);
	if (!err)
		vcpu->luo_preserved = true;
	mutex_unlock(&vcpu->mutex);
	if (err) {
		kho_unpreserve_free(ser);
		return err;
	}

	args->serialized_data = virt_to_phys(ser);
	return 0;
}

static int kvm_vcpu_luo_restore(struct kvm_vcpu *vcpu, void *data)
{
	return kvm_arch_vcpu_luo_retrieve(vcpu, data);
}

static int kvm_vcpu_luo_retrieve(struct liveupdate_file_op_args *args)
{
	struct kvm_vcpu_ser *ser;
	struct file *vm_file, *file;
	struct kvm *kvm;
	int err;

	if (!args->serialized_data)
		return -EINVAL;

	ser = phys_to_virt(args->serialized_data);
	if (ser->reserved)
		return -EINVAL;

	err = liveupdate_get_file_incoming(args->session, ser->vm_token, &vm_file);
	if (err)
		return err;

	if (!file_is_kvm(vm_file)) {
		fput(vm_file);
		return -EINVAL;
	}

	kvm = vm_file->private_data;
	if (kvm->mm != current->mm || kvm->vm_dead) {
		fput(vm_file);
		return -EIO;
	}

	file = kvm_create_vcpu_file(kvm, ser->vcpu_id, kvm_vcpu_luo_restore,
				    ser);
	fput(vm_file);
	if (IS_ERR(file))
		return PTR_ERR(file);

	args->file = file;
	return 0;
}

static void kvm_vcpu_luo_unpreserve(struct liveupdate_file_op_args *args)
{
	struct kvm_vcpu *vcpu = args->file ? args->file->private_data : NULL;
	struct kvm_vcpu_ser *ser;

	if (WARN_ON_ONCE(!args->serialized_data))
		return;

	ser = phys_to_virt(args->serialized_data);

	if (vcpu) {
		mutex_lock(&vcpu->mutex);
		vcpu->luo_preserved = false;
		mutex_unlock(&vcpu->mutex);
	}

	kvm_arch_vcpu_luo_unpreserve(ser);
	kho_unpreserve_free(ser);
}

static void kvm_vcpu_luo_finish(struct liveupdate_file_op_args *args)
{
	struct kvm_vcpu_ser *ser;

	if (!args->serialized_data)
		return;

	ser = phys_to_virt(args->serialized_data);

	kvm_arch_vcpu_luo_finish(ser);
	kho_restore_free(ser);
}

static const struct liveupdate_file_ops kvm_vcpu_luo_file_ops = {
	.can_preserve = kvm_vcpu_luo_can_preserve,
	.preserve = kvm_vcpu_luo_preserve,
	.retrieve = kvm_vcpu_luo_retrieve,
	.unpreserve = kvm_vcpu_luo_unpreserve,
	.finish = kvm_vcpu_luo_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler kvm_vcpu_luo_handler = {
	.ops = &kvm_vcpu_luo_file_ops,
	.compatible = KVM_VCPU_LUO_FH_COMPATIBLE,
};

int kvm_luo_init(void)
{
	int err = liveupdate_register_file_handler(&kvm_luo_handler);

	if (err && err != -EOPNOTSUPP) {
		pr_err("Could not register kvm_vm_luo handler: %pe\n", ERR_PTR(err));
		return err;
	}

	err = liveupdate_register_file_handler(&kvm_vcpu_luo_handler);
	if (err && err != -EOPNOTSUPP) {
		pr_err("Could not register kvm_vcpu_luo handler: %pe\n", ERR_PTR(err));
		liveupdate_unregister_file_handler(&kvm_luo_handler);
		return err;
	}

	return 0;
}

void kvm_luo_exit(void)
{
	liveupdate_unregister_file_handler(&kvm_vcpu_luo_handler);
	liveupdate_unregister_file_handler(&kvm_luo_handler);
}
