/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_KEXEC_HANDOVER_INTERNAL_H
#define LINUX_KEXEC_HANDOVER_INTERNAL_H

#include <linux/init.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/list.h>
#include <linux/types.h>

/* KHO internal flags for vmalloc preservations */
#define KHO_VMALLOC_ALLOC	0x0001
#define KHO_VMALLOC_HUGE_VMAP	0x0002

struct kho_vmalloc_walk_ops {
	const struct kho_vmalloc_chunk *(*get_chunk)(phys_addr_t pa);
	void (*put_chunk)(const struct kho_vmalloc_chunk *chunk);
	int (*entry)(phys_addr_t pa, void *priv);
};

#ifdef CONFIG_KEXEC_HANDOVER
const void *kho_get_fdt(void);
#else
static inline const void *kho_get_fdt(void) { return NULL; }
#endif

#ifdef CONFIG_KEXEC_HANDOVER_READONLY
const void *kho_ro_get_fdt(void);
bool kho_ro_cmdline_enabled(void);
#else
static inline const void *kho_ro_get_fdt(void) { return NULL; }
static inline bool kho_ro_cmdline_enabled(void) { return false; }
#endif

bool kho_vmalloc_validate(const struct kho_vmalloc *preservation,
			  unsigned int *vm_flags);
int kho_vmalloc_walk(const struct kho_vmalloc *preservation,
		     const struct kho_vmalloc_walk_ops *ops, void *priv);
unsigned long kho_encode_radix_key(phys_addr_t phys, unsigned int order);
phys_addr_t kho_decode_radix_key(unsigned long key, unsigned int *order);
unsigned long kho_radix_get_bitmap_index(unsigned long key);
unsigned long kho_radix_get_table_index(unsigned long key, unsigned int level);
struct kho_radix_leaf *kho_radix_find_leaf(phys_addr_t root_pa,
					   unsigned long key,
					   void *(*map)(phys_addr_t pa));
int __init kho_check_fdt(const void *fdt, phys_addr_t fdt_phys);
phys_addr_t __init kho_get_mem_map_phys(const void *fdt);

#ifdef CONFIG_KEXEC_HANDOVER_DEBUGFS
#include <linux/debugfs.h>

struct kho_debugfs {
	struct dentry *dir;
	struct dentry *sub_fdt_dir;
	struct list_head fdt_list;
};

#else
struct kho_debugfs {};
#endif

extern struct kho_scratch *kho_scratch;
extern unsigned int kho_scratch_cnt;

#ifdef CONFIG_KEXEC_HANDOVER_DEBUGFS
int kho_debugfs_init(void);
void kho_debugfs_cleanup(void);
void kho_in_debugfs_init(struct kho_debugfs *dbg, const void *fdt);
int kho_out_debugfs_init(struct kho_debugfs *dbg, const void *fdt);
int kho_debugfs_blob_add(struct kho_debugfs *dbg, const char *name,
			 const void *blob, size_t size, bool root);
void kho_debugfs_blob_remove(struct kho_debugfs *dbg, void *blob);
#else
static inline int kho_debugfs_init(void) { return 0; }
static inline void kho_debugfs_cleanup(void) { }
static inline void kho_in_debugfs_init(struct kho_debugfs *dbg,
				       const void *fdt) { }
static inline int kho_out_debugfs_init(struct kho_debugfs *dbg,
				       const void *fdt) { return 0; }
static inline int kho_debugfs_blob_add(struct kho_debugfs *dbg,
				       const char *name, const void *blob,
				       size_t size, bool root) { return 0; }
static inline void kho_debugfs_blob_remove(struct kho_debugfs *dbg,
					   void *blob) { }
#endif /* CONFIG_KEXEC_HANDOVER_DEBUGFS */

#endif /* LINUX_KEXEC_HANDOVER_INTERNAL_H */
