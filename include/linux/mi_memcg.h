/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MI_MEMCG_H
#define _LINUX_MI_MEMCG_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/list.h>

struct mem_cgroup;
struct page;

struct mi_memcg_stats {
	u64 compressed_pages;
	u64 compressed_bytes;
	u64 writeback_pages;
	u64 writeback_bytes;
	u64 faults;
};

#ifdef CONFIG_XIAOMI_MEMCG_RECLAIM
/* Allocation-tail extension: existing mem_cgroup layout is unchanged. */
struct mi_memcg {
	struct mem_cgroup *owner;
	/* Leaf lock; ZRAM slot -> lock. Never reclaim or take a slot under it. */
	spinlock_t lock;
	struct mi_memcg_stats stats;
	char name[32];
	u32 score;
	u32 comp_ratio;
	u32 wb_ratio;
	u32 refault_ratio;
	u64 last_faults;
	bool online;
	struct list_head policy_node;
#ifdef CONFIG_XIAOMI_MIMD_RECLAIM
	/* data->lock serializes request admission, foreground and result state. */
	bool mimd_active;
	bool mimd_foreground;
	u64 mimd_reclaimed_kb;
#endif
#ifdef CONFIG_XIAOMI_SMART_CACHE
	/* SmartCache's separate sc_lock; never take data->lock underneath it. */
	u32 sc_level;
	unsigned long sc_max_pages;
	bool sc_dying;
#endif
};

struct mi_memcg *mi_memcg_data(struct mem_cgroup *memcg);
void mi_memcg_init(struct mem_cgroup *memcg);
void mi_memcg_online(struct mem_cgroup *memcg);
void mi_memcg_offline(struct mem_cgroup *memcg);
struct mem_cgroup *mi_memcg_get_swap_page(struct page *page);
void mi_memcg_zram_add(struct mem_cgroup *memcg, u32 bytes, bool writeback);
void mi_memcg_zram_remove(struct mem_cgroup *memcg, u32 bytes, bool writeback);
void mi_memcg_zram_replace(struct mem_cgroup *memcg, u32 old_bytes,
			  u32 new_bytes, bool old_wb, bool new_wb);
void mi_memcg_zram_fault(struct mem_cgroup *memcg);
void mi_memcg_stats(struct mem_cgroup *memcg, struct mi_memcg_stats *stats);
struct mem_cgroup *mi_memcg_next(struct mem_cgroup *previous);
void mi_memcg_set_score(struct mem_cgroup *memcg, u32 score);
void mi_memcg_backend_get(void);
void mi_memcg_backend_put(void);
#ifdef CONFIG_ZRAM_WRITEBACK
int mi_zram_group_io(struct mem_cgroup *memcg, u64 bytes, bool write,
		     u64 *done, u32 ratio, bool (*cancelled)(void *), void *context);
#endif
#else
static inline void mi_memcg_init(struct mem_cgroup *memcg) { }
static inline void mi_memcg_online(struct mem_cgroup *memcg) { }
static inline void mi_memcg_offline(struct mem_cgroup *memcg) { }
static inline struct mem_cgroup *mi_memcg_get_swap_page(struct page *page)
{
	return NULL;
}
static inline void mi_memcg_zram_add(struct mem_cgroup *m, u32 b, bool w) { }
static inline void mi_memcg_zram_remove(struct mem_cgroup *m, u32 b, bool w) { }
static inline void mi_memcg_zram_replace(struct mem_cgroup *m, u32 o,
				       u32 n, bool ow, bool nw) { }
static inline void mi_memcg_zram_fault(struct mem_cgroup *memcg) { }
#endif
#endif
