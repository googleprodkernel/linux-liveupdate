/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_KEXEC_HANDOVER_INTERNAL_H
#define LINUX_KEXEC_HANDOVER_INTERNAL_H

#include <linux/kexec_handover.h>
#include <linux/list.h>
#include <linux/types.h>

#ifdef CONFIG_KEXEC_HANDOVER_DEBUG
#include <linux/debugfs.h>
#endif

struct kho_mem_track {
	/* Points to kho_mem_phys, each order gets its own bitmap tree */
	struct xarray orders;
};

struct kho_serialization {
	struct page *fdt;
	struct list_head fdt_list;
	struct kho_mem_track track;
	/* First chunk of serialized preserved memory map */
	struct khoser_mem_chunk *preserved_mem_map;
#ifdef CONFIG_KEXEC_HANDOVER_DEBUG
	struct dentry *sub_fdt_dir;
#endif
};

struct kho_in {
	phys_addr_t fdt_phys;
	phys_addr_t scratch_phys;
	struct list_head fdt_list;
#ifdef CONFIG_KEXEC_HANDOVER_DEBUG
	struct dentry *dir;
#endif
};

struct kho_out {
	struct blocking_notifier_head chain_head;
	struct mutex lock; /* protects KHO FDT finalization */
	struct kho_serialization ser;
	bool finalized;
#ifdef CONFIG_KEXEC_HANDOVER_DEBUG
	struct dentry *dir;
#endif
};

extern struct kho_in kho_in;
extern struct kho_out kho_out;

extern struct kho_scratch *kho_scratch;
extern unsigned int kho_scratch_cnt;

int __kho_finalize(void);
int __kho_abort(void);

#ifdef CONFIG_KEXEC_HANDOVER_DEBUG
int kho_debugfs_init(void);
void kho_in_debugfs_init(const void *fdt);
int kho_out_debugfs_init(void);
int kho_debugfs_fdt_add(struct kho_serialization *ser, const char *name,
			const void *fdt);
#else
static inline int kho_debugfs_init(void) { return 0; }
static inline void kho_in_debugfs_init(const void *fdt) { }
static inline int kho_out_debugfs_init(void) { return 0; }
static inline int kho_debugfs_fdt_add(struct kho_serialization *ser,
				      const char *name,
				      const void *fdt) { return 0; }
#endif /* CONFIG_KEXEC_HANDOVER_DEBUG */

#endif /* LINUX_KEXEC_HANDOVER_INTERNAL_H */
