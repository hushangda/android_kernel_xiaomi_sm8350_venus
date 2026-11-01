// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi memcg reclaim accounting for the Venus kernel.
 * Slot ownership uses CSS references, not recycled 16-bit memcg IDs.
 * Policy ABI reference: X-Ring 2024 mctrl/xswapd (GPL-2.0), pinned in
 * Documentation/admin-guide/mm/xiaomi-memcg.rst.
 */
#include <linux/memcontrol.h>
#include <linux/mi_memcg.h>
#include <linux/export.h>
#include <linux/pagemap.h>

/* policy list -> mi_reclaim.lock; no list acquisition from ZRAM accounting. */
static LIST_HEAD(mi_memcg_list);
static DEFINE_SPINLOCK(mi_memcg_list_lock);

/* mem_cgroup_alloc reserves this tail after the variable nodeinfo array. */
struct mi_memcg *mi_memcg_data(struct mem_cgroup *memcg)
{
	return (void *)memcg + sizeof(*memcg) +
		nr_node_ids * sizeof(memcg->nodeinfo[0]);
}

void mi_memcg_init(struct mem_cgroup *memcg)
{
	mi_memcg_data(memcg)->owner = memcg;
	spin_lock_init(&mi_memcg_data(memcg)->lock);
	mi_memcg_data(memcg)->refault_ratio = 50;
	INIT_LIST_HEAD(&mi_memcg_data(memcg)->policy_node);
}

void mi_memcg_online(struct mem_cgroup *memcg)
{
	unsigned long flags;

	spin_lock_irqsave(&mi_memcg_list_lock, flags);
	WRITE_ONCE(mi_memcg_data(memcg)->online, true);
	list_add_tail(&mi_memcg_data(memcg)->policy_node, &mi_memcg_list);
	spin_unlock_irqrestore(&mi_memcg_list_lock, flags);
}

void mi_memcg_offline(struct mem_cgroup *memcg)
{
	unsigned long flags;

	/* Outstanding ZRAM slot references keep the statistics storage alive. */
	spin_lock_irqsave(&mi_memcg_list_lock, flags);
	WRITE_ONCE(mi_memcg_data(memcg)->online, false);
	list_del_init(&mi_memcg_data(memcg)->policy_node);
	spin_unlock_irqrestore(&mi_memcg_list_lock, flags);
}

/* Consumes previous's CSS reference. Caller puts the last result on break. */
struct mem_cgroup *mi_memcg_next(struct mem_cgroup *previous)
{
	struct list_head *pos;
	struct mi_memcg *data;
	struct mem_cgroup *next = NULL;
	unsigned long flags;

	spin_lock_irqsave(&mi_memcg_list_lock, flags);
	pos = previous ? &mi_memcg_data(previous)->policy_node : &mi_memcg_list;
	if (list_empty(pos))
		goto out;
	for (pos = pos->next; pos != &mi_memcg_list; pos = pos->next) {
		data = list_entry(pos, struct mi_memcg, policy_node);
		next = data->owner;
		if (css_tryget_online(&next->css))
			break;
		next = NULL;
	}
out:
	spin_unlock_irqrestore(&mi_memcg_list_lock, flags);
	if (previous)
		css_put(&previous->css);
	return next;
}

void mi_memcg_set_score(struct mem_cgroup *memcg, u32 score)
{
	struct mi_memcg *data = mi_memcg_data(memcg);
	struct list_head *pos;
	struct mi_memcg *other;
	unsigned long flags;

	spin_lock_irqsave(&mi_memcg_list_lock, flags);
	spin_lock(&data->lock);
	data->score = score;
	spin_unlock(&data->lock);
	if (!data->online)
		goto out;
	list_del_init(&data->policy_node);
	list_for_each(pos, &mi_memcg_list) {
		other = list_entry(pos, struct mi_memcg, policy_node);
		if (score > other->score)
			break;
	}
	list_add_tail(&data->policy_node, pos);
out:
	spin_unlock_irqrestore(&mi_memcg_list_lock, flags);
}

struct mem_cgroup *mi_memcg_get_swap_page(struct page *page)
{
	struct mem_cgroup *memcg = NULL;

	page = compound_head(page);
	if (mem_cgroup_disabled() || !PageSwapCache(page))
		return NULL;
	/* The BIO pins the page. RCU protects concurrent charge migration. */
	rcu_read_lock();
	memcg = page_memcg_rcu(page);
	if (memcg && !css_tryget(&memcg->css))
		memcg = NULL;
	rcu_read_unlock();
	return memcg;
}
EXPORT_SYMBOL_GPL(mi_memcg_get_swap_page);

static void mi_memcg_account(struct mi_memcg_stats *s, u32 bytes,
			    bool writeback, bool add)
{
	u64 *pages = writeback ? &s->writeback_pages : &s->compressed_pages;
	u64 *size = writeback ? &s->writeback_bytes : &s->compressed_bytes;

	if (add) {
		(*pages)++;
		*size += bytes;
	} else {
		if (WARN_ON_ONCE(!*pages || *size < bytes))
			return;
		(*pages)--;
		*size -= bytes;
	}
}

void mi_memcg_zram_add(struct mem_cgroup *memcg, u32 bytes, bool writeback)
{
	unsigned long flags;

	/* Match hierarchical VM LRU counters used to calculate reclaim budgets. */
	for (; memcg; memcg = parent_mem_cgroup(memcg)) {
		spin_lock_irqsave(&mi_memcg_data(memcg)->lock, flags);
		mi_memcg_account(&mi_memcg_data(memcg)->stats, bytes, writeback, true);
		spin_unlock_irqrestore(&mi_memcg_data(memcg)->lock, flags);
	}
}
EXPORT_SYMBOL_GPL(mi_memcg_zram_add);

void mi_memcg_zram_remove(struct mem_cgroup *memcg, u32 bytes, bool writeback)
{
	unsigned long flags;

	for (; memcg; memcg = parent_mem_cgroup(memcg)) {
		spin_lock_irqsave(&mi_memcg_data(memcg)->lock, flags);
		mi_memcg_account(&mi_memcg_data(memcg)->stats, bytes, writeback, false);
		spin_unlock_irqrestore(&mi_memcg_data(memcg)->lock, flags);
	}
}
EXPORT_SYMBOL_GPL(mi_memcg_zram_remove);

void mi_memcg_zram_replace(struct mem_cgroup *memcg, u32 old_bytes,
			  u32 new_bytes, bool old_wb, bool new_wb)
{
	unsigned long flags;

	for (; memcg; memcg = parent_mem_cgroup(memcg)) {
		spin_lock_irqsave(&mi_memcg_data(memcg)->lock, flags);
		mi_memcg_account(&mi_memcg_data(memcg)->stats, old_bytes, old_wb, false);
		mi_memcg_account(&mi_memcg_data(memcg)->stats, new_bytes, new_wb, true);
		spin_unlock_irqrestore(&mi_memcg_data(memcg)->lock, flags);
	}
}
EXPORT_SYMBOL_GPL(mi_memcg_zram_replace);

void mi_memcg_zram_fault(struct mem_cgroup *memcg)
{
	unsigned long flags;

	for (; memcg; memcg = parent_mem_cgroup(memcg)) {
		spin_lock_irqsave(&mi_memcg_data(memcg)->lock, flags);
		if (mi_memcg_data(memcg)->stats.faults != U64_MAX)
			mi_memcg_data(memcg)->stats.faults++;
		spin_unlock_irqrestore(&mi_memcg_data(memcg)->lock, flags);
	}
}
EXPORT_SYMBOL_GPL(mi_memcg_zram_fault);

void mi_memcg_stats(struct mem_cgroup *memcg, struct mi_memcg_stats *stats)
{
	unsigned long flags;

	spin_lock_irqsave(&mi_memcg_data(memcg)->lock, flags);
	*stats = mi_memcg_data(memcg)->stats;
	spin_unlock_irqrestore(&mi_memcg_data(memcg)->lock, flags);
}
EXPORT_SYMBOL_GPL(mi_memcg_stats);
