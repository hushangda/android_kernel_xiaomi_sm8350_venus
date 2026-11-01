// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIMD2 synchronous per-memcg reclaim. Protocol independently established from
 * pandora stock perf_helper.ko: reclaim_once decimal retry/type/page encoding,
 * last-result KB readback and process_fg positive-value cancellation.
 * No global scan_type or vendor-private mem_cgroup offsets are used.
 */
#include <linux/cgroup.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/memcontrol.h>
#include <linux/mi_memcg.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/swap.h>

#define MI_MIMD_BASE 10000000ULL
#define MI_MIMD_CHUNK (4UL << (20 - PAGE_SHIFT))

struct mi_mimd_request {
	unsigned long pages;
	unsigned int type, retries;
};

static void mi_mimd_decode(u64 value, struct mi_mimd_request *request)
{
	u64 retries = value / (10 * MI_MIMD_BASE);
	u32 type = (value / MI_MIMD_BASE) % 10;

	request->retries = retries > 0 && retries < 10 ? retries : 10;
	request->type = type == 2 || type == 3 ? type : 0;
	request->pages = value % MI_MIMD_BASE;
}

static bool mi_mimd_cancelled(struct mi_memcg *data)
{
	return READ_ONCE(data->mimd_foreground) || !READ_ONCE(data->online) ||
		fatal_signal_pending(current);
}

static unsigned long mi_mimd_run(struct mi_memcg *data,
				const struct mi_mimd_request *request)
{
	unsigned long reclaimed = 0, nr, target;
	unsigned long deadline = jiffies + msecs_to_jiffies(5000);
	unsigned int retries = 0, calls = 0, idle = 0;
	bool anon = request->type != 3;

	while (reclaimed < request->pages && retries < request->retries &&
	       calls++ < 4096 && !mi_mimd_cancelled(data) &&
	       time_before(jiffies, deadline)) {
		target = min(request->pages - reclaimed, MI_MIMD_CHUNK);
		nr = xiaomi_reclaim_memcg(data->owner, target,
			anon ? XIAOMI_RECLAIM_ANON : XIAOMI_RECLAIM_FILE);
		reclaimed += nr;
		idle = nr ? 0 : idle + 1;
		if (idle >= (request->type ? 1 : 2))
			break;
		/* Subdivide a full attempt for cancellation; only short progress
		 * consumes a retry. Mixed requests must try the other LRU on zero.
		 */
		if (nr && nr < target)
			retries++;
		if (!request->type)
			anon = !anon;
		cond_resched();
	}
	return reclaimed;
}

static int mi_mimd_reclaim_write(struct cgroup_subsys_state *css,
				 struct cftype *cft, u64 value)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct mi_memcg *data = mi_memcg_data(memcg);
	struct mi_mimd_request request;
	unsigned long flags, reclaimed;
	int ret = 0;

	if (!css_tryget_online(css))
		return -ENODEV;
	mi_mimd_decode(value, &request);
	spin_lock_irqsave(&data->lock, flags);
	if (data->mimd_active) {
		ret = -EBUSY;
		spin_unlock_irqrestore(&data->lock, flags);
		goto put;
	}
	WRITE_ONCE(data->mimd_foreground, false);
	data->mimd_active = true;
	spin_unlock_irqrestore(&data->lock, flags);

	reclaimed = mi_mimd_run(data, &request);
	spin_lock_irqsave(&data->lock, flags);
	data->mimd_reclaimed_kb = (u64)reclaimed << (PAGE_SHIFT - 10);
	data->mimd_active = false;
	spin_unlock_irqrestore(&data->lock, flags);
put:
	css_put(css);
	return ret;
}

static u64 mi_mimd_reclaim_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct mi_memcg *data = mi_memcg_data(mem_cgroup_from_css(css));
	unsigned long flags;
	u64 reclaimed;

	spin_lock_irqsave(&data->lock, flags);
	reclaimed = data->mimd_reclaimed_kb;
	spin_unlock_irqrestore(&data->lock, flags);
	return reclaimed;
}

static int mi_mimd_foreground_write(struct cgroup_subsys_state *css,
				    struct cftype *cft, s64 value)
{
	struct mi_memcg *data = mi_memcg_data(mem_cgroup_from_css(css));
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);
	WRITE_ONCE(data->mimd_foreground, value > 0);
	spin_unlock_irqrestore(&data->lock, flags);
	return 0;
}

static struct cftype mi_mimd_files[] = {
	{ .name = "reclaim_once", .write_u64 = mi_mimd_reclaim_write,
	  .read_u64 = mi_mimd_reclaim_read },
	{ .name = "process_fg", .write_s64 = mi_mimd_foreground_write },
	{ }
};

static int __init mi_mimd_init(void)
{
	struct cftype *dfl;
	int ret;

	if (mem_cgroup_disabled())
		return 0;
	dfl = kmemdup(mi_mimd_files, sizeof(mi_mimd_files), GFP_KERNEL);
	if (!dfl)
		return -ENOMEM;
	ret = cgroup_add_legacy_cftypes(&memory_cgrp_subsys, mi_mimd_files);
	if (ret)
		goto free;
	ret = cgroup_add_dfl_cftypes(&memory_cgrp_subsys, dfl);
	if (!ret)
		return 0;
	cgroup_rm_cftypes(mi_mimd_files);
free:
	kfree(dfl);
	return ret;
}
late_initcall(mi_mimd_init);
