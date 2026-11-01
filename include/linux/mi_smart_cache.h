/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MI_SMART_CACHE_H
#define _LINUX_MI_SMART_CACHE_H

#include <linux/types.h>
#include <linux/err.h>
#include <linux/kconfig.h>

struct inode;
struct file;
struct page;
struct mem_cgroup;

#ifdef CONFIG_XIAOMI_SMART_CACHE
void mi_smart_cache_inode_free(struct inode *inode);
void mi_smart_cache_offline(struct mem_cgroup *memcg);
struct mem_cgroup *mi_smart_cache_charge_target(struct page *page);
bool mi_smart_cache_protect(struct page *page, int priority);
/* Returns migrated pages (zero if already there), or -errno. */
/* Sysfs migration context retains one CSS and moving_account reference. */
int mi_smart_cache_move_page(struct page *page, struct mem_cgroup *to,
			     struct mem_cgroup **source);
void mi_smart_cache_move_end(struct mem_cgroup **source);
#if IS_REACHABLE(CONFIG_OVERLAY_FS)
struct file *ovl_smart_cache_real_file(struct file *file);
#else
static inline struct file *ovl_smart_cache_real_file(struct file *file)
{
	return ERR_PTR(-EOPNOTSUPP);
}
#endif
#else
static inline void mi_smart_cache_inode_free(struct inode *inode) { }
static inline void mi_smart_cache_offline(struct mem_cgroup *memcg) { }
static inline struct mem_cgroup *mi_smart_cache_charge_target(struct page *page)
{
	return NULL;
}
static inline bool mi_smart_cache_protect(struct page *page, int priority)
{
	return false;
}
#endif
#endif
