// SPDX-License-Identifier: GPL-2.0
/* Native task QoS: object-owned contributions, no private vendor layouts. */
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/xr_qos.h>

/* Three dependency edges, matching the bounded vendor inheritance chain. */
#define XR_QOS_DEPTH 3
#define XR_QOS_RT_PRIO 98
#define XR_QOS_TASK_MAX 8192
#define XR_QOS_ACTIVE_MAX 128
#define XR_QOS_UPDATE_MAX (2 * XR_QOS_ACTIVE_MAX)
#define XR_QOS_IN_MAX 64
#define XR_QOS_OUT_MAX 16

struct xr_qos_value {
	u32 qos;
	int rt_prio;
	u32 vip;
};

struct xr_qos_task {
	struct list_head all;
	struct list_head active;
	struct xr_qos_vip_node vip;
	struct list_head incoming;
	struct list_head outgoing;
	struct work_struct work;
	struct task_struct *task;
	struct xr_qos_value effective;
	struct xr_qos_value level[XR_QOS_DEPTH + 1];
	struct xr_qos_value trial[XR_QOS_DEPTH + 1];
	u64 epoch;
	u8 dirty;
	u16 incoming_count;
	u16 outgoing_count;
	u32 static_qos;
	bool async_once;
	bool pending;
	bool exiting;
};

/*
 * Graph state only. Never take pi_lock/rq locks or flush work under this lock.
 * The worker drops it before the scheduler's pi_lock -> rq lock sequence.
 * Dependency hooks may hold their object's lock; the worker never takes those
 * locks. Task references belong to a queued work item or to a linked edge.
 */
static DEFINE_RAW_SPINLOCK(xr_qos_lock);
static LIST_HEAD(xr_qos_tasks);
static LIST_HEAD(xr_qos_active);
static unsigned int xr_task_count, xr_active_count;
static u64 xr_epoch;
/* Serialized scratch space: never place a graph-sized array on kernel stack. */
static struct xr_qos_task *xr_update[XR_QOS_UPDATE_MAX];
static unsigned int xr_updates;

/* Non-focused draw slots are process-owned while a live task requests them. */
static struct pid *xr_draw_slots[3];

static unsigned int xr_group(struct xr_qos_task *q)
{
	struct xr_qos_task *other;
	struct pid *pid = task_tgid(q->task);
	u32 qos = q->effective.qos;
	bool used;
	int slot;

	if (qos & 0xc0)
		return 13;
	switch (qos & 0xf) {
	case 10: return 11;
	case 9: return 12;
	case 8: return 13;
	case 7: return 14;
	case 6:
		for (slot = 0; slot < ARRAY_SIZE(xr_draw_slots); slot++) {
			used = false;
			list_for_each_entry(other, &xr_qos_active, active)
				if ((other->effective.qos & 0xf) == 6 &&
				    task_tgid(other->task) == xr_draw_slots[slot]) {
					used = true;
					break;
				}
			if (!used) {
				put_pid(xr_draw_slots[slot]);
				xr_draw_slots[slot] = NULL;
			}
			if (xr_draw_slots[slot] == pid)
				return 15 + slot;
		}
		for (slot = 0; slot < ARRAY_SIZE(xr_draw_slots); slot++)
			if (!xr_draw_slots[slot]) {
				xr_draw_slots[slot] = get_pid(pid);
				return 15 + slot;
			}
		return 0;
	default: return 0;
	}
}

static struct xr_qos_value xr_value(u32 qos)
{
	struct xr_qos_value v = { .qos = qos, .rt_prio = MAX_PRIO };

	if (qos & 0xf) {
		if (qos & BIT(4))
			v.rt_prio = XR_QOS_RT_PRIO;
		if (qos & BIT(5))
			v.vip = 5;
	}
	return v;
}

static struct xr_qos_value xr_merge(struct xr_qos_value a,
				   struct xr_qos_value b)
{
	a.qos = max(a.qos & 0xf, b.qos & 0xf) | ((a.qos | b.qos) & 0xf0);
	a.rt_prio = min(a.rt_prio, b.rt_prio);
	a.vip = max(a.vip, b.vip);
	return a;
}

static bool xr_equal(struct xr_qos_value a, struct xr_qos_value b)
{
	return a.qos == b.qos && a.rt_prio == b.rt_prio && a.vip == b.vip;
}

static void xr_work(struct work_struct *work)
{
	struct xr_qos_task *q = container_of(work, struct xr_qos_task, work);
	struct task_struct *p = q->task;
	struct xr_qos_value value;
	unsigned long flags;
	unsigned int group;

	for (;;) {
		raw_spin_lock_irqsave(&xr_qos_lock, flags);
		value = q->effective;
		group = xr_group(q);
		raw_spin_unlock_irqrestore(&xr_qos_lock, flags);

		xr_qos_sched_apply(p, value.rt_prio, value.vip);
		/* Co-location is secondary to existing cgroup/explicit RTG ownership. */
		walt_xr_qos_set_group(p, group);

		raw_spin_lock_irqsave(&xr_qos_lock, flags);
		if (xr_equal(value, q->effective)) {
			q->pending = false;
			raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
			break;
		}
		raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
	}
	put_task_struct(p);
}

static void xr_queue(struct xr_qos_task *q)
{
	if (!q->pending) {
		q->pending = true;
		get_task_struct(q->task);
		queue_work(system_highpri_wq, &q->work);
	}
}

/* Caller owns a task reference. State is installed once, freed with task. */
static struct xr_qos_task *xr_get_or_alloc(struct task_struct *p)
{
	struct xr_qos_task *q, *new;
	unsigned long flags;

	q = smp_load_acquire(&p->xr_qos);
	if (q)
		return q;
	new = kzalloc(sizeof(*new), GFP_ATOMIC);
	if (!new)
		return NULL;
	INIT_LIST_HEAD(&new->incoming);
	INIT_LIST_HEAD(&new->active);
	INIT_LIST_HEAD(&new->vip.entry);
	new->vip.task = p;
	INIT_LIST_HEAD(&new->outgoing);
	INIT_WORK(&new->work, xr_work);
	new->task = p;
	new->effective = xr_value(0);
	{
		int depth;

		for (depth = 0; depth <= XR_QOS_DEPTH; depth++)
			new->level[depth] = new->effective;
	}
	raw_spin_lock_irqsave(&xr_qos_lock, flags);
	q = p->xr_qos;
	if (!q && !(p->flags & PF_EXITING) && xr_task_count < XR_QOS_TASK_MAX) {
		xr_task_count++;
		q = new;
		new = NULL;
		list_add_tail(&q->all, &xr_qos_tasks);
		smp_store_release(&p->xr_qos, q);
	}
	raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
	kfree(new);
	return q;
}

/*
 * L[d] is the merge of static roots at distance <= d, never a recursively
 * accumulated effective value. This removes cycles without a global reset.
 * Only changed values enqueue the next layer of downstream neighbours. A
 * transaction has at most 256 touched tasks and 64 incoming / 16 outgoing
 * edges per task; at most 128 tasks may have a nonzero effective value.
 * Limits are checked before committing: removal never needs extra capacity.
 */
static int xr_mark(struct xr_qos_task *q, int depth)
{
	if (q->epoch != xr_epoch) {
		if (xr_updates == XR_QOS_UPDATE_MAX)
			return -E2BIG;
		q->epoch = xr_epoch;
		q->dirty = 0;
		memcpy(q->trial, q->level, sizeof(q->level));
		xr_update[xr_updates++] = q;
	}
	q->dirty |= BIT(depth);
	return 0;
}

static int xr_recompute(struct xr_qos_task *root, int first, bool removing)
{
	struct xr_qos_task *q, *from;
	struct xr_qos_link *link;
	struct xr_qos_value value;
	int depth, ret, active = xr_active_count;
	unsigned int n;

	xr_epoch++;
	xr_updates = 0;
	for (depth = first; depth <= XR_QOS_DEPTH; depth++)
		xr_mark(root, depth);
	for (depth = first; depth <= XR_QOS_DEPTH; depth++) {
		for (n = 0; n < xr_updates; n++) {
			q = xr_update[n];
			if (!(q->dirty & BIT(depth)))
				continue;
			value = xr_value(q->exiting ? 0 : q->static_qos);
			if (depth && !q->exiting)
				list_for_each_entry(link, &q->incoming, incoming) {
					from = link->from->xr_qos;
					value = xr_merge(value, from->epoch == xr_epoch ?
						from->trial[depth - 1] : from->level[depth - 1]);
				}
			if (!removing && depth < XR_QOS_DEPTH &&
			    (value.qos & 0xf) && q->task->xr_qos_dropped)
				return -ENOSPC;
			if (xr_equal(value, q->trial[depth]))
				continue;
			q->trial[depth] = value;
			if (depth == XR_QOS_DEPTH)
				continue;
			list_for_each_entry(link, &q->outgoing, outgoing) {
				ret = xr_mark(link->to->xr_qos, depth + 1);
				if (ret)
					return ret;
			}
		}
	}
	for (n = 0; n < xr_updates; n++) {
		q = xr_update[n];
		active += !!(q->trial[XR_QOS_DEPTH].qos & 0xf) -
			  !!(q->effective.qos & 0xf);
	}
	if (active > XR_QOS_ACTIVE_MAX)
		return -ENOSPC;
	for (n = 0; n < xr_updates; n++) {
		q = xr_update[n];
		memcpy(q->level, q->trial, sizeof(q->level));
		value = q->level[XR_QOS_DEPTH];
		if (xr_equal(value, q->effective))
			continue;
		if (!(q->effective.qos & 0xf) && (value.qos & 0xf))
			list_add_tail(&q->active, &xr_qos_active);
		else if (!(value.qos & 0xf))
			list_del_init(&q->active);
		q->effective = value;
		xr_queue(q);
	}
	xr_active_count = active;
	return 0;
}

struct xr_qos_vip_node *xr_qos_vip_node(struct task_struct *p)
{
	struct xr_qos_task *q = smp_load_acquire(&p->xr_qos);

	return q ? &q->vip : NULL;
}

void xr_qos_fork(struct task_struct *p)
{
	/* Neither static privilege nor in-flight dependencies cross fork. */
	p->xr_qos = NULL;
	p->xr_qos_rt_prio = MAX_PRIO;
	p->xr_qos_rt_only = false;
	p->xr_qos_vip = 0;
	p->xr_qos_rtg_owned = false;
	p->xr_qos_rtg_blocked = false;
	p->xr_qos_dropped = 0;
}

int xr_qos_set(struct task_struct *p, u32 qos)
{
	struct xr_qos_task *q;
	unsigned long flags;
	u32 level = qos & 0xf, type = (qos >> 4) & 3, old;
	int ret = 0;

	if (qos & ~0xffU || type == 3 || level > 10 ||
	    (level && level < 3) || (level == 6 && type != 2) ||
	    (level >= 7 && !type))
		return -EINVAL;
	q = xr_get_or_alloc(p);
	if (!q)
		return p->flags & PF_EXITING ? -ESRCH : -ENOMEM;
	raw_spin_lock_irqsave(&xr_qos_lock, flags);
	if (q->exiting || p->flags & PF_EXITING) {
		ret = -ESRCH;
	} else {
		old = q->static_qos;
		q->static_qos = level ? qos : 0;
		ret = xr_recompute(q, 0, !level);
		if (ret)
			q->static_qos = old;
	}
	raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
	/* The userspace control path must not report before its backend runs. */
	if (!ret)
		flush_work(&q->work);
	return ret;
}

u32 xr_qos_get(struct task_struct *p)
{
	struct xr_qos_task *q = smp_load_acquire(&p->xr_qos);
	unsigned long flags;
	u32 qos;

	if (!q)
		return 0;
	raw_spin_lock_irqsave(&xr_qos_lock, flags);
	qos = q->effective.qos & 0xf;
	raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
	return qos;
}

u32 xr_qos_get_static(struct task_struct *p)
{
	struct xr_qos_task *q = smp_load_acquire(&p->xr_qos);

	return q ? READ_ONCE(q->static_qos) & 0xf : 0;
}

bool xr_qos_group_requested(struct task_struct *p)
{
	struct xr_qos_task *q = smp_load_acquire(&p->xr_qos);
	u32 qos = q ? READ_ONCE(q->effective.qos) : 0;

	return (qos & 0xf) >= 6 || (qos & 0xc0);
}

int xr_qos_async_arm(struct task_struct *p, bool enable)
{
	struct xr_qos_task *q = xr_get_or_alloc(p);
	unsigned long flags;
	int ret = 0;

	if (!q)
		return p->flags & PF_EXITING ? -ESRCH : -ENOMEM;
	raw_spin_lock_irqsave(&xr_qos_lock, flags);
	if (q->exiting || p->flags & PF_EXITING)
		ret = -ESRCH;
	else
		q->async_once = enable;
	raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
	return ret;
}

bool xr_qos_async_consume(struct task_struct *p)
{
	struct xr_qos_task *q = smp_load_acquire(&p->xr_qos);
	unsigned long flags;
	bool armed = false;

	if (!q)
		return false;
	raw_spin_lock_irqsave(&xr_qos_lock, flags);
	if (!q->exiting) {
		armed = q->async_once;
		q->async_once = false;
	}
	raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
	return armed;
}

/*
 * A rejected edge remains caller-owned until link_del(): its reference/count
 * makes later SET fail explicitly instead of silently accepting missing
 * topology. No scheduler donation is made for a rejected edge.
 */
int xr_qos_link_add(struct xr_qos_link *link, struct task_struct *from,
		    struct task_struct *to, enum xr_qos_dependency type)
{
	struct xr_qos_task *a, *b;
	unsigned long flags;
	int ret = 0;

	if (!from || !to || from == to || type >= XR_QOS_DEPENDENCIES)
		return -EINVAL;
	a = xr_get_or_alloc(from);
	b = xr_get_or_alloc(to);
	raw_spin_lock_irqsave(&xr_qos_lock, flags);
	if (link->from) {
		ret = -EALREADY;
		goto out;
	}
	if ((from->flags | to->flags) & PF_EXITING) {
		ret = -ESRCH;
		goto out;
	}
	get_task_struct(from);
	get_task_struct(to);
	link->from = from;
	link->to = to;
	link->type = type;
	link->tracked = false;
	if (!a || !b) {
		ret = -ENOMEM;
	} else if (a->outgoing_count == XR_QOS_OUT_MAX ||
		   b->incoming_count == XR_QOS_IN_MAX) {
		ret = -ENOSPC;
	} else {
		list_add_tail(&link->incoming, &b->incoming);
		list_add_tail(&link->outgoing, &a->outgoing);
		a->outgoing_count++;
		b->incoming_count++;
		if (a->level[XR_QOS_DEPTH - 1].qos & 0xf)
			ret = xr_recompute(b, 1, false);
		if (ret) {
			list_del_init(&link->incoming);
			list_del_init(&link->outgoing);
			a->outgoing_count--;
			b->incoming_count--;
		} else {
			link->tracked = true;
		}
	}
	if (ret)
		from->xr_qos_dropped++;
out:
	raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
	if (ret == -ENOMEM || ret == -ENOSPC || ret == -E2BIG)
		pr_warn_ratelimited("xr_qos: dependency capacity error %d (%d -> %d)\n",
			ret, task_pid_nr(from), task_pid_nr(to));
	return ret;
}

/* xr_qos_lock held; caller saves both referenced tasks before detaching. */
static void xr_unlink(struct xr_qos_link *link)
{
	struct xr_qos_task *a = link->from->xr_qos;
	struct xr_qos_task *b = link->to->xr_qos;

	if (link->tracked) {
		list_del_init(&link->incoming);
		list_del_init(&link->outgoing);
		a->outgoing_count--;
		b->incoming_count--;
		if (a->level[XR_QOS_DEPTH - 1].qos & 0xf)
			WARN_ON_ONCE(xr_recompute(b, 1, true));
	} else {
		link->from->xr_qos_dropped--;
	}
	link->from = NULL;
	link->to = NULL;
	link->tracked = false;
}

void xr_qos_link_del(struct xr_qos_link *link)
{
	struct task_struct *from, *to;
	unsigned long flags;

	raw_spin_lock_irqsave(&xr_qos_lock, flags);
	from = link->from;
	to = link->to;
	if (from)
		xr_unlink(link);
	raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
	if (from) {
		put_task_struct(from);
		put_task_struct(to);
	}
}

void xr_qos_exit(struct task_struct *p)
{
	struct xr_qos_task *q = smp_load_acquire(&p->xr_qos);
	struct xr_qos_link *link;
	struct task_struct *from, *to;
	unsigned long flags;

	if (!q)
		return;
	raw_spin_lock_irqsave(&xr_qos_lock, flags);
	q->exiting = true;
	q->async_once = false;
	q->static_qos = 0;
	WARN_ON_ONCE(xr_recompute(q, 0, true));
	raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
	for (;;) {
		raw_spin_lock_irqsave(&xr_qos_lock, flags);
		if (!list_empty(&q->incoming))
			link = list_first_entry(&q->incoming, struct xr_qos_link, incoming);
		else if (!list_empty(&q->outgoing))
			link = list_first_entry(&q->outgoing, struct xr_qos_link, outgoing);
		else {
			list_del_init(&q->all);
			xr_task_count--;
			raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
			break;
		}
		from = link->from;
		to = link->to;
		xr_unlink(link);
		raw_spin_unlock_irqrestore(&xr_qos_lock, flags);
		put_task_struct(from);
		put_task_struct(to);
	}
	flush_work(&q->work);
}

void xr_qos_free(struct task_struct *p)
{
	struct xr_qos_task *q = p->xr_qos;

	if (q) {
		WARN_ON(q->pending || !list_empty(&q->incoming) ||
			!list_empty(&q->outgoing) || !list_empty(&q->all) ||
			!list_empty(&q->active) || !list_empty(&q->vip.entry));
		kfree(q);
	}
}

/* RCU's existing boost engine supplies the actual blocking reader. Registry
 * entries live on synchronize_rcu's stack; the lock protects their lifetime. */
static DEFINE_RAW_SPINLOCK(xr_rcu_lock);
static LIST_HEAD(xr_rcu_waiters);
static unsigned int xr_rcu_wait_count;

void xr_qos_rcu_wait_start(struct xr_qos_rcu_wait *wait)
{
	unsigned long flags;

	INIT_LIST_HEAD(&wait->entry);
	wait->task = current;
	raw_spin_lock_irqsave(&xr_rcu_lock, flags);
	if (xr_rcu_wait_count < XR_QOS_ACTIVE_MAX) {
		list_add_tail(&wait->entry, &xr_rcu_waiters);
		xr_rcu_wait_count++;
	} else {
		pr_warn_ratelimited("xr_qos: RCU waiter capacity exhausted\n");
	}
	raw_spin_unlock_irqrestore(&xr_rcu_lock, flags);
}

void xr_qos_rcu_wait_end(struct xr_qos_rcu_wait *wait)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&xr_rcu_lock, flags);
	if (!list_empty(&wait->entry)) {
		list_del_init(&wait->entry);
		xr_rcu_wait_count--;
	}
	raw_spin_unlock_irqrestore(&xr_rcu_lock, flags);
}

void xr_qos_rcu_boost(struct xr_qos_link *link, struct task_struct *reader)
{
	struct xr_qos_rcu_wait *wait;
	struct task_struct *donor = NULL;
	unsigned long flags;

	/* Match the donor policy: most recent live QoS grace-period waiter. */
	raw_spin_lock_irqsave(&xr_rcu_lock, flags);
	list_for_each_entry_reverse(wait, &xr_rcu_waiters, entry) {
		if (wait->task != reader && xr_qos_get(wait->task)) {
			donor = wait->task;
			get_task_struct(donor);
			break;
		}
	}
	if (donor)
		xr_qos_link_add(link, donor, reader, XR_QOS_RCU);
	raw_spin_unlock_irqrestore(&xr_rcu_lock, flags);
	if (donor)
		put_task_struct(donor);
}
