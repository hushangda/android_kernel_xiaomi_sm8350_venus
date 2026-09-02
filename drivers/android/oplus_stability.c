// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPlus framework-stability compatibility backend.
 *
 * Android userspace opens the generic-netlink family "oplus_frk_nl" and
 * enables it with command 1, attribute 1, payload byte 1. Kernel reports use
 * command 2, attribute 1 and an array of native-endian ints:
 *
 *     event, number_of_values, values[]
 *
 * Event numbers and thresholds follow OPlus' published stability helper.
 */

#define pr_fmt(fmt) "oplus_stability: " fmt

#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/dma-buf.h>
#include <linux/errno.h>
#include <linux/fdtable.h>
#include <linux/genetlink.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/oplus_stability.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/user_namespace.h>
#include <linux/workqueue.h>
#include <net/genetlink.h>
#include <trace/events/vmscan.h>

#include "binder_internal.h"

#define FRK_FAMILY_NAME			"oplus_frk_nl"
#define FRK_FAMILY_VERSION		1
#define FRK_DYNAMIC_FAMILY_ID		0
#define FRK_SYSTEM_UID			1000
#define FRK_MAX_DATA_INTS		128
#define FRK_MESSAGE_HEADER_INTS		2

#define FRK_THREAD_HIGH_WATERMARK	30000
#define FRK_PROCESS_THREAD_THRESHOLD	5000
#define FRK_THREAD_REPORT_INTERVAL	(60 * HZ)

#define FRK_LOWMEM_REPORT_INTERVAL	(10 * HZ)
#define FRK_BINDER_PENDING_MS		50000
#define FRK_BINDER_PENDING_COUNT		200

#define FRK_PAGES(size)			((size) >> PAGE_SHIFT)
#define FRK_KIB(pages)			((pages) << (PAGE_SHIFT - 10))

enum frk_command {
	FRK_CMD_UNSPEC,
	FRK_CMD_RECV,
	FRK_CMD_SEND,
};

enum frk_attribute {
	FRK_ATTR_UNSPEC,
	FRK_ATTR_MESSAGE,
};

enum frk_event {
	FRK_THREAD_WATCHER_EVENT = 1,
	FRK_LOWMEM_WATCHER_EVENT = 2,
	FRK_PENDING_TRANSACTION_WATCHER_EVENT = 3,
};

enum frk_memory_type {
	FRK_ANON_LEAK = 1,
	FRK_DMABUF_LEAK = 2,
	FRK_OTHER_LEAK = 3,
};

struct frk_event_work {
	struct work_struct work;
	int event;
	size_t count;
	int values[6];
};

struct frk_memory_report {
	int type;
	int size_kib;
	int pid;
	int total_kib;
	int free_kib;
	int available_kib;
};

struct frk_dmabuf_total {
	u64 bytes;
};

struct frk_dmabuf_process {
	u64 bytes;
};

static atomic_t frk_client_port = ATOMIC_INIT(0);
static atomic_t frk_enabled = ATOMIC_INIT(0);
static atomic_t frk_ready = ATOMIC_INIT(0);
static atomic_t frk_last_binder_node = ATOMIC_INIT(-1);
static atomic_t frk_lowmem_work_pending = ATOMIC_INIT(0);
static atomic64_t frk_last_lowmem_report = ATOMIC64_INIT(0);
static DEFINE_SPINLOCK(frk_thread_report_lock);
static u64 frk_last_thread_report;
static struct workqueue_struct *frk_workqueue;
static struct work_struct frk_lowmem_work;

static int frk_netlink_receive(struct sk_buff *skb, struct genl_info *info);

static const struct genl_ops frk_genl_ops[] = {
	{
		.cmd = FRK_CMD_RECV,
		.doit = frk_netlink_receive,
	},
};

static struct genl_family frk_genl_family = {
	.id = FRK_DYNAMIC_FAMILY_ID,
	.hdrsize = 0,
	.name = FRK_FAMILY_NAME,
	.version = FRK_FAMILY_VERSION,
	.maxattr = FRK_ATTR_MESSAGE,
	.ops = frk_genl_ops,
	.n_ops = ARRAY_SIZE(frk_genl_ops),
	.module = THIS_MODULE,
};

static bool frk_active(void)
{
	return atomic_read(&frk_ready) && atomic_read(&frk_enabled) &&
	       atomic_read(&frk_client_port) > 0;
}

static bool frk_is_system_server(struct task_struct *task)
{
	return task && __kuid_val(task_uid(task)) == FRK_SYSTEM_UID &&
	       !strcmp(task->comm, "system_server");
}

static int frk_send_event(int event, size_t count, const int *values)
{
	int message[FRK_MAX_DATA_INTS];
	struct sk_buff *skb;
	void *header;
	size_t message_count;
	size_t message_size;
	int port;
	int ret;

	if (!frk_active())
		return 0;
	if (count > FRK_MAX_DATA_INTS - FRK_MESSAGE_HEADER_INTS)
		return -EMSGSIZE;
	if (count && !values)
		return -EINVAL;

	port = atomic_read(&frk_client_port);
	if (port <= 0)
		return -ENOTCONN;

	message[0] = event;
	message[1] = count;
	if (count)
		memcpy(&message[FRK_MESSAGE_HEADER_INTS], values,
		       count * sizeof(*values));
	message_count = count + FRK_MESSAGE_HEADER_INTS;
	message_size = message_count * sizeof(*message);

	skb = genlmsg_new(nla_total_size(message_size), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	header = genlmsg_put(skb, 0, 0, &frk_genl_family, 0, FRK_CMD_SEND);
	if (!header) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}
	if (nla_put(skb, FRK_ATTR_MESSAGE, message_size, message)) {
		genlmsg_cancel(skb, header);
		kfree_skb(skb);
		return -EMSGSIZE;
	}
	genlmsg_end(skb, header);

	ret = genlmsg_unicast(&init_net, skb, port);
	if (ret < 0) {
		if (atomic_cmpxchg(&frk_client_port, port, 0) == port)
			atomic_set(&frk_enabled, 0);
		pr_warn_ratelimited("event %d send failed: %d\n", event, ret);
	}
	return ret;
}

static int frk_netlink_receive(struct sk_buff *skb, struct genl_info *info)
{
	struct genlmsghdr *genlhdr;
	struct nlattr *attribute;
	kuid_t sender;
	uid_t sender_uid;
	u8 enable;

	if (!skb || !info || !info->snd_portid)
		return -EINVAL;
	genlhdr = nlmsg_data(nlmsg_hdr(skb));
	if (genlmsg_len(genlhdr) < NLA_HDRLEN)
		return -EINVAL;
	attribute = genlmsg_data(genlhdr);
	if (!nla_ok(attribute, genlmsg_len(genlhdr)) ||
	    nla_type(attribute) != FRK_ATTR_MESSAGE ||
	    nla_len(attribute) < sizeof(enable))
		return -EINVAL;

	sender = NETLINK_CREDS(skb)->uid;
	sender_uid = from_kuid_munged(&init_user_ns, sender);
	if (sender_uid != 0 && sender_uid != FRK_SYSTEM_UID)
		return -EPERM;

	enable = *(u8 *)nla_data(attribute);
	if (enable != 0 && enable != 1)
		return -EINVAL;
	if (!enable) {
		atomic_set(&frk_enabled, 0);
		atomic_set(&frk_client_port, 0);
		return 0;
	}

	atomic_set(&frk_client_port, info->snd_portid);
	atomic_set(&frk_last_binder_node, -1);
	atomic_set(&frk_enabled, 1);
	pr_info("daemon handshake complete (uid=%u, port=%u)\n",
		sender_uid, info->snd_portid);
	return 0;
}

static void frk_event_worker(struct work_struct *work)
{
	struct frk_event_work *event_work =
		container_of(work, struct frk_event_work, work);

	frk_send_event(event_work->event, event_work->count,
		       event_work->values);
	kfree(event_work);
}

static void frk_queue_event(int event, size_t count, const int *values,
			    gfp_t gfp)
{
	struct frk_event_work *event_work;

	if (!frk_active() || !frk_workqueue || count > 6)
		return;
	event_work = kmalloc(sizeof(*event_work), gfp);
	if (!event_work)
		return;
	event_work->event = event;
	event_work->count = count;
	memcpy(event_work->values, values, count * sizeof(*values));
	INIT_WORK(&event_work->work, frk_event_worker);
	if (!queue_work(frk_workqueue, &event_work->work))
		kfree(event_work);
}

void oplus_stability_thread_created(struct task_struct *task,
				    int system_threads)
{
	unsigned long flags;
	int process_threads;
	int values[3];
	u64 now;
	bool force_report = false;

	if (!frk_active() || !task || !current->signal)
		return;
	process_threads = current->signal->nr_threads;
	if (system_threads <= FRK_THREAD_HIGH_WATERMARK &&
	    process_threads < FRK_PROCESS_THREAD_THRESHOLD)
		return;

	if (system_threads > FRK_THREAD_HIGH_WATERMARK &&
	    system_threads % 100 == 0)
		force_report = true;
	if (process_threads == FRK_PROCESS_THREAD_THRESHOLD ||
	    (process_threads > FRK_PROCESS_THREAD_THRESHOLD &&
	     process_threads % 1000 == 0))
		force_report = true;

	now = get_jiffies_64();
	spin_lock_irqsave(&frk_thread_report_lock, flags);
	if (!force_report && frk_last_thread_report &&
	    time_before64(now, frk_last_thread_report +
			     FRK_THREAD_REPORT_INTERVAL)) {
		spin_unlock_irqrestore(&frk_thread_report_lock, flags);
		return;
	}
	frk_last_thread_report = now;
	spin_unlock_irqrestore(&frk_thread_report_lock, flags);

	values[0] = system_threads;
	values[1] = task_tgid_nr(current);
	values[2] = process_threads;
	frk_queue_event(FRK_THREAD_WATCHER_EVENT, ARRAY_SIZE(values), values,
			GFP_ATOMIC);
}

void oplus_stability_binder_transaction_init(struct binder_transaction *t)
{
	if (t && (t->flags & TF_ONE_WAY) && t->to_proc &&
	    frk_is_system_server(t->to_proc->tsk) && frk_active())
		t->oplus_stability_start_ns = ktime_get_ns();
}

void oplus_stability_binder_transaction_queued_locked(
		struct binder_proc *proc, struct binder_transaction *transaction,
		bool pending_async)
{
	struct binder_transaction *first_transaction;
	struct binder_node *node;
	struct binder_work *work;
	struct frk_event_work *event_work;
	u64 now_ns;
	u64 elapsed_ms;
	unsigned int pending_count = 0;

	if (!frk_active() || !pending_async || !proc || !transaction ||
	    !transaction->buffer || !frk_is_system_server(proc->tsk))
		return;
	if (proc->alloc.free_async_space > proc->alloc.buffer_size / 3)
		return;

	node = transaction->buffer->target_node;
	if (!node)
		return;
	work = list_first_entry_or_null(&node->async_todo,
					struct binder_work, entry);
	if (!work || work->type != BINDER_WORK_TRANSACTION)
		return;
	first_transaction = container_of(work, struct binder_transaction, work);
	now_ns = ktime_get_ns();
	if (!first_transaction->oplus_stability_start_ns ||
	    now_ns <= first_transaction->oplus_stability_start_ns)
		return;
	elapsed_ms = div_u64(now_ns - first_transaction->oplus_stability_start_ns,
			     NSEC_PER_MSEC);
	if (elapsed_ms <= FRK_BINDER_PENDING_MS)
		return;

	list_for_each_entry(work, &node->async_todo, entry) {
		if (work->type == BINDER_WORK_TRANSACTION)
			pending_count++;
	}
	if (pending_count <= FRK_BINDER_PENDING_COUNT ||
	    atomic_read(&frk_last_binder_node) == node->debug_id)
		return;

	event_work = kmalloc(sizeof(*event_work), GFP_ATOMIC);
	if (!event_work)
		return;
	event_work->event = FRK_PENDING_TRANSACTION_WATCHER_EVENT;
	event_work->count = 2;
	event_work->values[0] = node->debug_id;
	event_work->values[1] = min_t(u64, elapsed_ms / 1000, INT_MAX);
	INIT_WORK(&event_work->work, frk_event_worker);
	atomic_set(&frk_last_binder_node, node->debug_id);
	if (!queue_work(frk_workqueue, &event_work->work))
		kfree(event_work);
}

static unsigned long frk_free_pages(void)
{
	long free = global_zone_page_state(NR_FREE_PAGES) -
		    global_zone_page_state(NR_FREE_CMA_PAGES);

	return free > 0 ? free : 0;
}

static unsigned long frk_file_pages(void)
{
	return global_node_page_state(NR_ACTIVE_FILE) +
	       global_node_page_state(NR_INACTIVE_FILE);
}

static unsigned long frk_anon_pages(void)
{
	return global_node_page_state(NR_ANON_MAPPED);
}

static int frk_sum_dmabuf(const struct dma_buf *dmabuf, void *private)
{
	struct frk_dmabuf_total *total = private;

	if (dmabuf)
		total->bytes += dmabuf->size;
	return 0;
}

static unsigned long frk_dmabuf_pages(void)
{
	struct frk_dmabuf_total total = { };

	if (dma_buf_get_each(frk_sum_dmabuf, &total))
		return 0;
	return total.bytes >> PAGE_SHIFT;
}

static int frk_sum_process_dmabuf(const void *private, struct file *file,
				  unsigned int fd)
{
	struct frk_dmabuf_process *usage = (void *)private;
	struct dma_buf *dmabuf;

	if (!file || !is_dma_buf_file(file))
		return 0;
	dmabuf = file->private_data;
	if (dmabuf)
		usage->bytes += dmabuf->size;
	return 0;
}

static void frk_largest_anon_process(int *pid, unsigned long *pages)
{
	struct task_struct *task;

	*pid = 0;
	*pages = 0;
	rcu_read_lock();
	for_each_process(task) {
		unsigned long task_pages = 0;

		task_lock(task);
		if (task->mm)
			task_pages = get_mm_counter(task->mm, MM_ANONPAGES);
		task_unlock(task);
		if (task_pages > *pages) {
			*pages = task_pages;
			*pid = task_pid_nr(task);
		}
	}
	rcu_read_unlock();
}

static void frk_largest_dmabuf_process(int *pid, u64 *bytes)
{
	struct task_struct *task;

	*pid = 0;
	*bytes = 0;
	rcu_read_lock();
	for_each_process(task) {
		struct frk_dmabuf_process usage = { };

		if (task->flags & PF_KTHREAD)
			continue;
		task_lock(task);
		iterate_fd(task->files, 0, frk_sum_process_dmabuf, &usage);
		task_unlock(task);
		if (usage.bytes > *bytes) {
			*bytes = usage.bytes;
			*pid = task_pid_nr(task);
		}
	}
	rcu_read_unlock();
}

static unsigned long frk_lowmem_watermark(unsigned long total_pages)
{
	if (total_pages >= FRK_PAGES(SZ_4G + SZ_4G + SZ_4G))
		return FRK_PAGES(SZ_1G + SZ_512M);
	if (total_pages >= FRK_PAGES(SZ_2G + SZ_2G))
		return FRK_PAGES(SZ_1G);
	return FRK_PAGES(SZ_512M);
}

static void frk_lowmem_worker(struct work_struct *work)
{
	struct frk_memory_report report = { };
	struct sysinfo swap;
	unsigned long total_pages = totalram_pages();
	unsigned long anon_pages;
	unsigned long dmabuf_pages;
	unsigned long largest_anon_pages;
	u64 largest_dmabuf_bytes;
	int values[6];

	if (!frk_active())
		goto done;

	report.type = FRK_OTHER_LEAK;
	report.total_kib = FRK_KIB(total_pages);
	report.free_kib = FRK_KIB(global_zone_page_state(NR_FREE_PAGES));
	report.available_kib = FRK_KIB(si_mem_available());

	si_swapinfo(&swap);
	anon_pages = frk_anon_pages() + swap.totalswap - swap.freeswap;
	if (anon_pages > total_pages / 2) {
		report.type = FRK_ANON_LEAK;
		frk_largest_anon_process(&report.pid, &largest_anon_pages);
		report.size_kib = FRK_KIB(largest_anon_pages);
	} else {
		dmabuf_pages = frk_dmabuf_pages();
		if (dmabuf_pages > FRK_PAGES(SZ_2G + SZ_1G)) {
			report.type = FRK_DMABUF_LEAK;
			frk_largest_dmabuf_process(&report.pid,
						   &largest_dmabuf_bytes);
			report.size_kib = min_t(u64,
				largest_dmabuf_bytes >> 10, INT_MAX);
		}
	}

	values[0] = report.type;
	values[1] = report.size_kib;
	values[2] = report.pid;
	values[3] = report.total_kib;
	values[4] = report.free_kib;
	values[5] = report.available_kib;
	frk_send_event(FRK_LOWMEM_WATCHER_EVENT, ARRAY_SIZE(values), values);
done:
	atomic_set(&frk_lowmem_work_pending, 0);
}

static void frk_direct_reclaim_begin(void *unused, int order,
				     gfp_t gfp_flags)
{
	unsigned long total_pages;
	u64 last;
	u64 now;

	if (!frk_active() || !frk_workqueue)
		return;
	total_pages = totalram_pages();
	if (frk_free_pages() + frk_file_pages() >
	    frk_lowmem_watermark(total_pages))
		return;

	now = get_jiffies_64();
	last = atomic64_read(&frk_last_lowmem_report);
	if (last && time_before64(now, last + FRK_LOWMEM_REPORT_INTERVAL))
		return;
	if (atomic64_cmpxchg(&frk_last_lowmem_report, last, now) != last)
		return;
	if (atomic_cmpxchg(&frk_lowmem_work_pending, 0, 1) != 0)
		return;
	if (!queue_work(frk_workqueue, &frk_lowmem_work))
		atomic_set(&frk_lowmem_work_pending, 0);
}

static int __init frk_stability_init(void)
{
	int ret;

	frk_workqueue = alloc_ordered_workqueue("oplus_stability",
						WQ_MEM_RECLAIM);
	if (!frk_workqueue)
		return -ENOMEM;
	INIT_WORK(&frk_lowmem_work, frk_lowmem_worker);

	ret = genl_register_family(&frk_genl_family);
	if (ret)
		goto destroy_workqueue;
	ret = register_trace_mm_vmscan_direct_reclaim_begin(
			frk_direct_reclaim_begin, NULL);
	if (ret)
		goto unregister_family;

	atomic_set(&frk_ready, 1);
	pr_info("registered family id=%u\n", frk_genl_family.id);
	return 0;

unregister_family:
	genl_unregister_family(&frk_genl_family);
destroy_workqueue:
	destroy_workqueue(frk_workqueue);
	frk_workqueue = NULL;
	return ret;
}

static void __exit frk_stability_exit(void)
{
	atomic_set(&frk_ready, 0);
	atomic_set(&frk_enabled, 0);
	atomic_set(&frk_client_port, 0);
	unregister_trace_mm_vmscan_direct_reclaim_begin(
		frk_direct_reclaim_begin, NULL);
	flush_workqueue(frk_workqueue);
	genl_unregister_family(&frk_genl_family);
	destroy_workqueue(frk_workqueue);
	frk_workqueue = NULL;
}

module_init(frk_stability_init);
module_exit(frk_stability_exit);

MODULE_DESCRIPTION("OPlus framework stability compatibility backend");
MODULE_LICENSE("GPL v2");
