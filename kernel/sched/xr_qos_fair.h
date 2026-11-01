/* SPDX-License-Identifier: GPL-2.0 */
/* Included by fair.c after CFS bandwidth helpers; all calls hold rq->lock. */
#ifdef CONFIG_XIAOMI_XR_QOS
#define XR_VIP_PERIOD_NS (8 * NSEC_PER_MSEC)
#define XR_VIP_BUDGET_NS NSEC_PER_MSEC

static bool xr_vip_budget(struct rq *rq)
{
	u64 now = rq_clock_task(rq);

	if (now - rq->xr_vip_period >= XR_VIP_PERIOD_NS) {
		rq->xr_vip_period = now;
		rq->xr_vip_runtime = 0;
	}
	return rq->xr_vip_runtime < XR_VIP_BUDGET_NS;
}

static void xr_vip_enqueue(struct rq *rq, struct task_struct *p)
{
	struct xr_qos_vip_node *node = xr_qos_vip_node(p);

	if (node && p->xr_qos_vip && list_empty(&node->entry))
		list_add_tail(&node->entry, &rq->xr_vip_tasks);
}

static void xr_vip_dequeue(struct task_struct *p)
{
	struct xr_qos_vip_node *node = xr_qos_vip_node(p);

	if (node && !list_empty(&node->entry))
		list_del_init(&node->entry);
}

void xr_qos_vip_changed(struct rq *rq, struct task_struct *p, unsigned int vip)
{
	xr_vip_dequeue(p);
	p->xr_qos_vip = vip;
	if (task_on_rq_queued(p) && p->sched_class == &fair_sched_class) {
		xr_vip_enqueue(rq, p);
		if (vip && xr_vip_budget(rq) && !throttled_hierarchy(task_cfs_rq(p)))
			resched_curr(rq);
	}
}

void xr_qos_vip_account(struct rq *rq, struct task_struct *p, u64 delta)
{
	if (!p->xr_qos_vip)
		return;
	if (xr_vip_budget(rq)) {
		rq->xr_vip_runtime += delta;
		if (rq->xr_vip_runtime >= XR_VIP_BUDGET_NS)
			resched_curr(rq);
	}
}

static struct task_struct *xr_vip_pick(struct rq *rq)
{
	struct task_struct *p, *best = NULL;
	struct sched_entity *se;
	struct xr_qos_vip_node *node;
	bool throttled;

	if (!xr_vip_budget(rq))
		return NULL;
	list_for_each_entry(node, &rq->xr_vip_tasks, entry) {
		p = node->task;
		if (!p->se.on_rq || throttled_hierarchy(task_cfs_rq(p)))
			continue;
		throttled = false;
		se = &p->se;
		for_each_sched_entity(se) {
			if (check_cfs_rq_runtime(cfs_rq_of(se))) {
				throttled = true;
				break;
			}
		}
		if (throttled)
			continue;
		if (!best || p->xr_qos_vip > best->xr_qos_vip)
			best = p;
	}
	/* Different cgroups do not share a vruntime coordinate system. */
	if (best)
		list_move_tail(&xr_qos_vip_node(best)->entry, &rq->xr_vip_tasks);
	return best;
}
#else
static inline void xr_vip_enqueue(struct rq *rq, struct task_struct *p) { }
static inline void xr_vip_dequeue(struct task_struct *p) { }
#endif
