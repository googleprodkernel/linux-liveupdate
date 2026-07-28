/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __KVM_GUEST_MEMFD_H__
#define __KVM_GUEST_MEMFD_H__

#include <linux/kvm_host.h>
#include <linux/kvm_types.h>
#include <linux/fs.h>
#include <linux/mempolicy.h>

#ifdef CONFIG_KVM_GUEST_MEMFD
int kvm_gmem_init(struct module *module);
void kvm_gmem_exit(void);
int kvm_gmem_create(struct kvm *kvm, struct kvm_create_guest_memfd *args);
int kvm_gmem_bind(struct kvm *kvm, struct kvm_memory_slot *slot,
		  unsigned int fd, uoff_t offset);
void kvm_gmem_unbind(struct kvm_memory_slot *slot);
#else
static inline int kvm_gmem_init(struct module *module)
{
	return 0;
}
static inline void kvm_gmem_exit(void) {};
static inline int kvm_gmem_bind(struct kvm *kvm,
					 struct kvm_memory_slot *slot,
					 unsigned int fd, uoff_t offset)
{
	WARN_ON_ONCE(1);
	return -EIO;
}

static inline void kvm_gmem_unbind(struct kvm_memory_slot *slot)
{
	WARN_ON_ONCE(1);
}
#endif /* CONFIG_KVM_GUEST_MEMFD */

/*
 * A guest_memfd instance can be associated multiple VMs, each with its own
 * "view" of the underlying physical memory.
 *
 * The gmem's inode is effectively the raw underlying physical storage, and is
 * used to track properties of the physical memory, while each gmem file is
 * effectively a single VM's view of that storage, and is used to track assets
 * specific to its associated VM, e.g. memslots=>gmem bindings.
 */
struct gmem_file {
	struct kvm *kvm;
	struct xarray bindings;
	struct list_head entry;
};

struct gmem_inode {
	struct shared_policy policy;
	struct inode vfs_inode;
	struct list_head gmem_file_list;

	u64 flags;
};

static inline struct gmem_inode *GMEM_I(struct inode *inode)
{
	return container_of(inode, struct gmem_inode, vfs_inode);
}

struct file *__kvm_gmem_create_file(struct kvm *kvm, loff_t size, u64 flags);

#endif /* __KVM_GUEST_MEMFD_H__ */
