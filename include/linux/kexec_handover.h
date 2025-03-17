/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_KEXEC_HANDOVER_H
#define LINUX_KEXEC_HANDOVER_H

#include <linux/types.h>
#include <linux/hashtable.h>
#include <linux/notifier.h>

struct kho_scratch {
	phys_addr_t addr;
	phys_addr_t size;
};

/* KHO Notifier index */
enum kho_event {
	KEXEC_KHO_FINALIZE = 0,
	KEXEC_KHO_UNFREEZE = 1,
};

#define KHO_HASHTABLE_BITS 3
#define KHO_NODE_INIT                                        \
	{                                                    \
		.props = HASHTABLE_INIT(KHO_HASHTABLE_BITS), \
		.nodes = HASHTABLE_INIT(KHO_HASHTABLE_BITS), \
	}

struct kho_node {
	struct hlist_node hlist;

	const char *name;
	DECLARE_HASHTABLE(props, KHO_HASHTABLE_BITS);
	DECLARE_HASHTABLE(nodes, KHO_HASHTABLE_BITS);

	struct list_head list;
	bool visited;
};

#ifdef CONFIG_KEXEC_HANDOVER
bool kho_is_enabled(void);
void kho_init_node(struct kho_node *node);
int kho_add_node(struct kho_node *parent, const char *name,
		 struct kho_node *child);
struct kho_node *kho_remove_node(struct kho_node *parent, const char *name);
int kho_add_prop(struct kho_node *node, const char *key, const void *val,
		 u32 size);
void *kho_remove_prop(struct kho_node *node, const char *key, u32 *size);
int kho_add_string_prop(struct kho_node *node, const char *key,
			const char *val);

int register_kho_notifier(struct notifier_block *nb);
int unregister_kho_notifier(struct notifier_block *nb);

void kho_memory_init(void);
#else
static inline bool kho_is_enabled(void)
{
	return false;
}

static inline void kho_init_node(struct kho_node *node)
{
}

static inline int kho_add_node(struct kho_node *parent, const char *name,
			       struct kho_node *child)
{
	return -EOPNOTSUPP;
}

static inline struct kho_node *kho_remove_node(struct kho_node *parent,
					       const char *name)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline int kho_add_prop(struct kho_node *node, const char *key,
			       const void *val, u32 size)
{
	return -EOPNOTSUPP;
}

static inline void *kho_remove_prop(struct kho_node *node, const char *key,
				    u32 *size)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline int kho_add_string_prop(struct kho_node *node, const char *key,
				      const char *val)
{
	return -EOPNOTSUPP;
}

static inline int register_kho_notifier(struct notifier_block *nb)
{
	return -EOPNOTSUPP;
}

static inline int unregister_kho_notifier(struct notifier_block *nb)
{
	return -EOPNOTSUPP;
}

static inline void kho_memory_init(void)
{
}
#endif /* CONFIG_KEXEC_HANDOVER */

#endif /* LINUX_KEXEC_HANDOVER_H */
