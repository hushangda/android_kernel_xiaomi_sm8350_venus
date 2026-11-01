// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi MCTRL/XSWAPD policy on real memcg and ZRAM accounting.
 * ABI/equations: X-Ring technologies Inc. 2024 mctrl.c/xswapd.c (GPL-2.0).
 * Scoped reclaim, lifecycle and synchronization are native to this kernel.
 */
#include <linux/cgroup.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/mi_memcg.h>
#include <linux/mi_memcg_core.h>
#include <linux/mutex.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/workqueue.h>

#define MI_RECLAIM_WINDOW (4UL << (20 - PAGE_SHIFT))
#define MI_RECLAIM_CALLS 4096U

static atomic_t mi_backends = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(mi_reclaim_wait);
static struct task_struct *mi_reclaim_task;
static DEFINE_MUTEX(mi_control_lock);
static bool mi_enabled;
static bool mi_umr_enabled;
static unsigned long mi_stop_swap;
static atomic64_t mi_reclaimed = ATOMIC64_INIT(0);
static atomic64_t mi_attempts = ATOMIC64_INIT(0);
static atomic64_t mi_no_progress = ATOMIC64_INIT(0);

/* Public source defaults, in MiB. Configured state is snapshotted atomically. */
struct mi_reclaim_control {
	u32 low_mb, high_mb;
	u32 reclaim_mb, wake_ms;
};
static struct mi_reclaim_control mi_control = {2500, 3000, 100, 1000};

void mi_memcg_backend_get(void)
{
	atomic_inc(&mi_backends);
}
EXPORT_SYMBOL_GPL(mi_memcg_backend_get);

void mi_memcg_backend_put(void)
{
	WARN_ON_ONCE(atomic_dec_return(&mi_backends) < 0);
	wake_up(&mi_reclaim_wait);
}
EXPORT_SYMBOL_GPL(mi_memcg_backend_put);

static bool mi_backend_ready(void)
{
	return atomic_read(&mi_backends) > 0 && total_swap_pages > 0;
}

static bool mi_reclaim_allowed(void)
{
	return (READ_ONCE(mi_enabled) || READ_ONCE(mi_umr_enabled)) &&
	       mi_backend_ready();
}

static unsigned long mi_lru_pages(struct mem_cgroup *memcg, int type)
{
	/* Match enum xiaomi_reclaim_type / node_stat_item order (0..3). */
	return memcg_page_state(memcg, NR_INACTIVE_ANON + type);
}

struct mi_reclaim_context {
	struct mem_cgroup *memcg;
	unsigned long deadline;
	bool background;
};

static bool mi_reclaim_cancelled(void *ctx)
{
	struct mi_reclaim_context *work = ctx;

	if (fatal_signal_pending(current) || !mi_reclaim_allowed() ||
	    !READ_ONCE(mi_memcg_data(work->memcg)->online) ||
	    time_after_eq(jiffies, work->deadline) ||
	    test_and_clear_bit(0, &mi_stop_swap))
		return true;
	if (work->background && (kthread_should_stop() || freezing(current) ||
				!READ_ONCE(mi_enabled)))
		return true;
	return false;
}

static int mi_reclaim_one(void *ctx, enum vmc_lru_type type, u64 pages, u64 *done)
{
	struct mi_reclaim_context *work = ctx;

	/* A single VM call stays bounded even for the ROM's 4096MiB window. */
	pages = min_t(u64, pages, MI_RECLAIM_WINDOW);
	*done = xiaomi_reclaim_memcg(work->memcg, pages,
				   (enum xiaomi_reclaim_type)type);
	atomic64_inc(&mi_attempts);
	atomic64_add(*done, &mi_reclaimed);
	if (!*done)
		atomic64_inc(&mi_no_progress);
	cond_resched();
	return 0;
}

static int mi_do_reclaim(struct mem_cgroup *memcg, struct vmc_request *request,
			 bool background, struct vmc_result *result)
{
	struct mi_reclaim_context work = {
		.memcg = memcg,
		.deadline = jiffies + msecs_to_jiffies(background ? 1000 : 5000),
		.background = background,
	};
	struct vmc_reclaimer ops = {mi_reclaim_one, mi_reclaim_cancelled, &work};

	return vmc_run_reclaim(request, &ops, MI_RECLAIM_CALLS, result);
}

static int mi_policy_budget(struct mem_cgroup *memcg, unsigned int priority,
			    struct vmc_budget *budget)
{
	struct mi_memcg *data = mi_memcg_data(memcg);
	struct vmc_policy policy;
	struct vmc_snapshot snapshot;
	unsigned long flags;

	snapshot.anon_pages = mi_lru_pages(memcg, VMC_INACTIVE_ANON) +
			      mi_lru_pages(memcg, VMC_ACTIVE_ANON);
	spin_lock_irqsave(&data->lock, flags);
	policy = (struct vmc_policy) {data->score, data->comp_ratio,
				    data->refault_ratio, priority};
	snapshot.compressed_pages = data->stats.compressed_pages;
	snapshot.writeback_pages = data->stats.writeback_pages;
	snapshot.faults = data->stats.faults;
	snapshot.previous_faults = data->last_faults;
	spin_unlock_irqrestore(&data->lock, flags);
	return vmc_policy_budget(&policy, &snapshot, budget);
}

static void mi_refault_snapshot(void)
{
	struct mem_cgroup *memcg = NULL;
	unsigned long flags;

	while ((memcg = mem_cgroup_iter(NULL, memcg, NULL))) {
		spin_lock_irqsave(&mi_memcg_data(memcg)->lock, flags);
		mi_memcg_data(memcg)->last_faults = mi_memcg_data(memcg)->stats.faults;
		spin_unlock_irqrestore(&mi_memcg_data(memcg)->lock, flags);
		cond_resched();
	}
}

static void mi_reclaim_round(const struct mi_reclaim_control *control)
{
	struct mem_cgroup *memcg = NULL;
	unsigned long target, reclaimed = 0;
	long available = si_mem_available();
	unsigned int visits = 0;
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	struct vmc_budget budget;
	struct vmc_result result;
	struct vmc_request request = {.type = VMC_ACTIVE_ANON};

	if (max(available, 0L) >= (unsigned long)control->low_mb << (20 - PAGE_SHIFT))
		return;
	target = min_t(unsigned long, control->reclaim_mb << (20 - PAGE_SHIFT),
		 (control->high_mb << (20 - PAGE_SHIFT)) - max(available, 0L));
	while ((memcg = mi_memcg_next(memcg))) {
		/* Bound a pass even if userspace continuously reorders the score list. */
		if (++visits > 65536 || time_after_eq(jiffies, deadline) || !READ_ONCE(mi_enabled) ||
		    kthread_should_stop() || freezing(current) || reclaimed >= target)
			break;
		if (mem_cgroup_is_root(memcg) || mi_policy_budget(memcg, 0, &budget) ||
		    !budget.pages)
			continue;
		request.target_pages = min_t(u64, budget.pages, target - reclaimed);
		request.window_pages = min_t(u64, request.target_pages, MI_RECLAIM_WINDOW);
		if (!mi_do_reclaim(memcg, &request, true, &result))
			reclaimed += result.reclaimed_pages;
		cond_resched();
	}
	if (memcg)
		css_put(&memcg->css);
}

#ifdef CONFIG_ZRAM_WRITEBACK
static void mi_writeback_round(const struct mi_reclaim_control *control);
#endif

static int mi_reclaim_thread(void *unused)
{
	struct mi_reclaim_control control;

	set_freezable();
	while (!kthread_should_stop()) {
		wait_event_freezable(mi_reclaim_wait,
			kthread_should_stop() || READ_ONCE(mi_enabled));
		if (kthread_should_stop())
			break;
		mutex_lock(&mi_control_lock);
		control = mi_control;
		mutex_unlock(&mi_control_lock);
		if (READ_ONCE(mi_enabled) && mi_backend_ready()) {
			mi_reclaim_round(&control);
#ifdef CONFIG_ZRAM_WRITEBACK
			mi_writeback_round(&control);
#endif
			mi_refault_snapshot();
		}
		wait_event_freezable_timeout(mi_reclaim_wait,
			kthread_should_stop() || !READ_ONCE(mi_enabled),
			msecs_to_jiffies(control.wake_ms));
	}
	return 0;
}

enum mi_policy_field {MI_SCORE, MI_COMP_RATIO, MI_REFAULT_RATIO, MI_WB_RATIO};

static int mi_policy_write(struct cgroup_subsys_state *css, struct cftype *cft, u64 val)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct mi_memcg *data = mi_memcg_data(memcg);
	unsigned long flags;

	if (val > (cft->private == MI_SCORE ? 1000 : 100))
		return -EINVAL;
	if (cft->private == MI_SCORE) {
		mi_memcg_set_score(memcg, val);
		return 0;
	}
	spin_lock_irqsave(&data->lock, flags);
	if (cft->private == MI_COMP_RATIO)
		data->comp_ratio = val;
	else if (cft->private == MI_WB_RATIO)
		data->wb_ratio = val;
	else
		data->refault_ratio = val;
	spin_unlock_irqrestore(&data->lock, flags);
	return 0;
}

static u64 mi_policy_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct mi_memcg *data = mi_memcg_data(mem_cgroup_from_css(css));
	unsigned long flags;
	u64 value;

	spin_lock_irqsave(&data->lock, flags);
	value = cft->private == MI_SCORE ? data->score :
		cft->private == MI_COMP_RATIO ? data->comp_ratio :
		cft->private == MI_WB_RATIO ? data->wb_ratio : data->refault_ratio;
	spin_unlock_irqrestore(&data->lock, flags);
	return value;
}

static ssize_t mi_name_write(struct kernfs_open_file *of, char *buf,
			    size_t nbytes, loff_t off)
{
	struct mi_memcg *data = mi_memcg_data(mem_cgroup_from_css(of_css(of)));
	unsigned long flags;
	char name[sizeof(data->name)] = {0};

	if (memchr(buf, '\0', nbytes))
		return -EINVAL;
	strlcpy(name, strstrip(buf), sizeof(name));
	spin_lock_irqsave(&data->lock, flags);
	memcpy(data->name, name, sizeof(name));
	spin_unlock_irqrestore(&data->lock, flags);
	return nbytes;
}

static int mi_name_show(struct seq_file *m, void *v)
{
	struct mi_memcg *data = mi_memcg_data(mem_cgroup_from_seq(m));
	unsigned long flags;
	char name[sizeof(data->name)];

	spin_lock_irqsave(&data->lock, flags);
	memcpy(name, data->name, sizeof(name));
	spin_unlock_irqrestore(&data->lock, flags);
	seq_printf(m, "%s\n", name);
	return 0;
}

static int mi_stat_show(struct seq_file *m, void *v)
{
	struct mi_memcg_stats stats;

	mi_memcg_stats(mem_cgroup_from_seq(m), &stats);
	seq_printf(m, "comp_pages: %llu\ncomp_size: %llu\ncomp_fault: %llu\n",
		   stats.compressed_pages, stats.compressed_bytes, stats.faults);
	seq_printf(m, "wb_pages: %llu\nwb_size: %llu\n",
		   stats.writeback_pages, stats.writeback_bytes);
	return 0;
}

static ssize_t mi_reclaim_write(struct kernfs_open_file *of, char *buf,
			       size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	struct vmc_request request;
	struct vmc_result result;
	u64 pages[4];
	int i, ret;

	if (!mi_reclaim_allowed())
		return -EPERM;
	if (!READ_ONCE(mi_umr_enabled) && READ_ONCE(mi_memcg_data(memcg)->score) <= 100)
		return -EPERM;
	ret = vmc_parse_reclaim(buf, nbytes, PAGE_SHIFT, &request);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		pages[i] = mi_lru_pages(memcg, i);
	ret = vmc_cap_request(&request, pages);
	if (ret)
		return ret;
	ret = mi_do_reclaim(memcg, &request, false, &result);
	return ret ? ret : nbytes;
}

#ifdef CONFIG_ZRAM_WRITEBACK
struct mi_io_context {
	struct mem_cgroup *memcg;
	unsigned long deadline;
	bool write;
	bool background;
};
struct mi_io_job {
	struct work_struct work;
	struct mem_cgroup *memcg;
	u64 bytes;
};
static struct workqueue_struct *mi_io_wq;
static atomic_t mi_io_queued = ATOMIC_INIT(0);
static atomic64_t mi_wb_bytes = ATOMIC64_INIT(0);
static atomic64_t mi_rb_bytes = ATOMIC64_INIT(0);
static atomic64_t mi_io_errors = ATOMIC64_INIT(0);

static u64 mi_percent(u64 total, u32 ratio)
{
	return total / 100 * ratio + total % 100 * ratio / 100;
}

static bool mi_io_cancelled(void *context)
{
	struct mi_io_context *io = context;

	return fatal_signal_pending(current) || freezing(current) ||
	       !READ_ONCE(mi_memcg_data(io->memcg)->online) ||
	       time_after_eq(jiffies, io->deadline) ||
	       (io->background && !READ_ONCE(mi_enabled)) ||
	       (io->write && (!mi_reclaim_allowed() || test_and_clear_bit(1, &mi_stop_swap)));
}

static int mi_do_io(struct mem_cgroup *memcg, u64 bytes, bool write,
		    u32 ratio, bool background, u64 *transferred)
{
	struct mi_io_context io = {
		.memcg = memcg, .write = write, .background = background,
		.deadline = jiffies + msecs_to_jiffies(background ? 1000 : 5000),
	};
	u64 done;
	int ret;

	ret = mi_zram_group_io(memcg, bytes, write, &done, ratio, mi_io_cancelled, &io);
	if (transferred)
		*transferred = done;
	atomic64_add(done, write ? &mi_wb_bytes : &mi_rb_bytes);
	if (ret)
		atomic64_inc(&mi_io_errors);
	return ret;
}

static void mi_writeback_round(const struct mi_reclaim_control *control)
{
	struct mem_cgroup *memcg = NULL;
	struct vmc_budget budget;
	struct mi_memcg_stats stats;
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	u64 remaining = (u64)control->reclaim_mb << 20, done;
	u32 ratio;
	unsigned int visits = 0;
	int ret;

	if (max(si_mem_available(), 0L) >= (unsigned long)control->low_mb << (20 - PAGE_SHIFT))
		return;
	while ((memcg = mi_memcg_next(memcg))) {
		if (++visits > 65536 || !remaining || !READ_ONCE(mi_enabled) ||
		    kthread_should_stop() || freezing(current) || time_after_eq(jiffies, deadline))
			break;
		ratio = READ_ONCE(mi_memcg_data(memcg)->wb_ratio);
		if (!ratio || mem_cgroup_is_root(memcg) || mi_policy_budget(memcg, 0, &budget) ||
		    budget.gate == VMC_SCORE_PROTECTED || budget.gate == VMC_REFAULT)
			continue;
		mi_memcg_stats(memcg, &stats);
		if (!stats.compressed_bytes)
			continue;
		ret = mi_do_io(memcg, min(stats.compressed_bytes, remaining), true, ratio, true, &done);
		remaining -= min(done, remaining);
		if (ret == -ENODEV)
			break;
		cond_resched();
	}
	if (memcg)
		css_put(&memcg->css);
}

static void mi_io_work(struct work_struct *work)
{
	struct mi_io_job *job = container_of(work, struct mi_io_job, work);

	mi_do_io(job->memcg, job->bytes, true, 100, false, NULL);
	css_put(&job->memcg->css);
	atomic_dec(&mi_io_queued);
	kfree(job);
}

static int mi_swapin_write(struct cgroup_subsys_state *css, struct cftype *cft, u64 val)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct mi_memcg_stats stats;

	if (val > 100)
		return -EINVAL;
	mi_memcg_stats(memcg, &stats);
	/* No written-out slots is genuinely no work, not a synthetic success. */
	return mi_do_io(memcg, mi_percent(stats.writeback_bytes, val), false, 100, false, NULL);
}

static ssize_t mi_swapout_write(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	struct mi_memcg_stats stats;
	struct mi_io_job *job;
	u64 values[2], bytes;
	unsigned int count;
	int ret;

	ret = vmc_parse_values(buf, nbytes, values, 2, &count);
	if (ret)
		return ret;
	if (values[0] > 100 || (count == 2 && values[1] > 1))
		return -EINVAL;
	if (!mi_reclaim_allowed() || (!READ_ONCE(mi_umr_enabled) &&
	    READ_ONCE(mi_memcg_data(memcg)->score) <= 100))
		return -EPERM;
	mi_memcg_stats(memcg, &stats);
	bytes = mi_percent(stats.compressed_bytes, values[0]);
	/* Current ROM writes just a percentage. Its operation completes here. */
	if (count == 1 || values[1]) {
		ret = mi_do_io(memcg, bytes, true, 100, false, NULL);
		return ret ? ret : nbytes;
	}
	if (!bytes)
		return nbytes;
	if (atomic_inc_return(&mi_io_queued) > 32) {
		atomic_dec(&mi_io_queued);
		return -EBUSY;
	}
	job = kmalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		atomic_dec(&mi_io_queued);
		return -ENOMEM;
	}
	if (!css_tryget_online(&memcg->css)) {
		atomic_dec(&mi_io_queued);
		kfree(job);
		return -ENODEV;
	}
	job->memcg = memcg;
	job->bytes = bytes;
	INIT_WORK(&job->work, mi_io_work);
	queue_work(mi_io_wq, &job->work);
	return nbytes;
}
#endif

static int mi_enable_write(struct cgroup_subsys_state *css, struct cftype *cft, u64 val)
{
	if (val > 1)
		return -EINVAL;
	if (val && !mi_backend_ready())
		return -EPERM;
	if (cft->private)
		WRITE_ONCE(mi_umr_enabled, val);
	else
		WRITE_ONCE(mi_enabled, val);
	wake_up(&mi_reclaim_wait);
	return 0;
}

static u64 mi_enable_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return cft->private ? READ_ONCE(mi_umr_enabled) : READ_ONCE(mi_enabled);
}

static int mi_stop_write(struct cgroup_subsys_state *css, struct cftype *cft, u64 val)
{
#ifdef CONFIG_ZRAM_WRITEBACK
	if (val > 3)
		return -EINVAL;
	if (val & 2)
		set_bit(1, &mi_stop_swap);
	else
		clear_bit(1, &mi_stop_swap);
#else
	if (val > 1)
		return -EINVAL;
#endif
	if (val & 1)
		set_bit(0, &mi_stop_swap);
	else
		clear_bit(0, &mi_stop_swap);
	return 0;
}

static u64 mi_stop_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return READ_ONCE(mi_stop_swap);
}

static int mi_worker_stat_show(struct seq_file *m, void *v)
{
	seq_printf(m, "reclaim_calls: %lld\nreclaimed_pages: %lld\nno_progress: %lld\n",
		   atomic64_read(&mi_attempts), atomic64_read(&mi_reclaimed),
		   atomic64_read(&mi_no_progress));
	seq_printf(m, "tracked_devices: %d\n", atomic_read(&mi_backends));
#ifdef CONFIG_ZRAM_WRITEBACK
	seq_printf(m, "writeback_bytes: %lld\nreadback_bytes: %lld\nio_errors: %lld\nqueued_io: %d\n",
		   atomic64_read(&mi_wb_bytes), atomic64_read(&mi_rb_bytes),
		   atomic64_read(&mi_io_errors), atomic_read(&mi_io_queued));
#endif
	return 0;
}

/* Only controls with a complete backend are exposed. Writeback/cache follow. */
static struct cftype mi_policy_files[] = {
	{ .name = "mctrl.name", .write = mi_name_write, .seq_show = mi_name_show },
	{ .name = "mctrl.score", .private = MI_SCORE,
	  .write_u64 = mi_policy_write, .read_u64 = mi_policy_read },
	{ .name = "mctrl.comp_ratio", .private = MI_COMP_RATIO,
	  .write_u64 = mi_policy_write, .read_u64 = mi_policy_read },
	{ .name = "mctrl.refault_ratio", .private = MI_REFAULT_RATIO,
	  .write_u64 = mi_policy_write, .read_u64 = mi_policy_read },
#ifdef CONFIG_ZRAM_WRITEBACK
	{ .name = "mctrl.wb_ratio", .private = MI_WB_RATIO,
	  .write_u64 = mi_policy_write, .read_u64 = mi_policy_read },
	{ .name = "xswapd.swapin", .write_u64 = mi_swapin_write },
	{ .name = "xswapd.swapout", .write = mi_swapout_write },
#endif
	{ .name = "mctrl.stat", .seq_show = mi_stat_show },
	{ .name = "xswapd.reclaim", .write = mi_reclaim_write },
	{ .name = "xswapd.enable", .flags = CFTYPE_ONLY_ON_ROOT,
	  .write_u64 = mi_enable_write, .read_u64 = mi_enable_read },
	{ .name = "xswapd.umrenable", .flags = CFTYPE_ONLY_ON_ROOT, .private = 1,
	  .write_u64 = mi_enable_write, .read_u64 = mi_enable_read },
	{ .name = "xswapd.stop_swap", .flags = CFTYPE_ONLY_ON_ROOT,
	  .write_u64 = mi_stop_write, .read_u64 = mi_stop_read },
	{ .name = "xswapd.stat", .flags = CFTYPE_ONLY_ON_ROOT,
	  .seq_show = mi_worker_stat_show },
	{ }
};

static int __init mi_policy_init(void)
{
	struct cftype *dfl_files;
	int ret;

	if (mem_cgroup_disabled())
		return 0;
	dfl_files = kmemdup(mi_policy_files, sizeof(mi_policy_files), GFP_KERNEL);
	if (!dfl_files)
		return -ENOMEM;
#ifdef CONFIG_ZRAM_WRITEBACK
	mi_io_wq = alloc_workqueue("xiaomi_zram", WQ_FREEZABLE | WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!mi_io_wq) {
		ret = -ENOMEM;
		goto free_dfl;
	}
#endif
	mi_reclaim_task = kthread_run(mi_reclaim_thread, NULL, "xiaomi_reclaim");
	if (IS_ERR(mi_reclaim_task)) {
		ret = PTR_ERR(mi_reclaim_task);
		goto free_dfl;
	}
	ret = cgroup_add_legacy_cftypes(&memory_cgrp_subsys, mi_policy_files);
	if (ret)
		goto stop;
	ret = cgroup_add_dfl_cftypes(&memory_cgrp_subsys, dfl_files);
	if (ret)
		goto remove_legacy;
	return 0;
remove_legacy:
	cgroup_rm_cftypes(mi_policy_files);
stop:
	kthread_stop(mi_reclaim_task);
free_dfl:
#ifdef CONFIG_ZRAM_WRITEBACK
	if (mi_io_wq)
		destroy_workqueue(mi_io_wq);
#endif
	kfree(dfl_files);
	return ret;
}
late_initcall(mi_policy_init);
