/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_XR_QOS_H
#define _LINUX_XR_QOS_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/compiler.h>

struct task_struct;

struct xr_qos_rcu_wait {
	struct list_head entry;
	struct task_struct *task;
};

struct xr_qos_vip_node {
	struct list_head entry;
	struct task_struct *task;
};

enum xr_qos_dependency {
	XR_QOS_BINDER, XR_QOS_MUTEX, XR_QOS_RWSEM, XR_QOS_PERCPU_RWSEM,
	XR_QOS_FUTEX, XR_QOS_FUSE, XR_QOS_RCU, XR_QOS_DEPENDENCIES,
};

/* Embedded in the dependency object, never indexed by a recycled PID. */
struct xr_qos_link {
	struct list_head incoming;
	struct list_head outgoing;
	struct task_struct *from;
	struct task_struct *to;
	enum xr_qos_dependency type;
	bool tracked;
};

/* The caller serializes its dependency object's add/del operations. */
static inline bool xr_qos_link_matches(struct xr_qos_link *link,
				       struct task_struct *to)
{
	return to && READ_ONCE(link->to) == to && READ_ONCE(link->tracked);
}

#ifdef CONFIG_XIAOMI_XR_QOS
void xr_qos_rcu_wait_start(struct xr_qos_rcu_wait *wait);
void xr_qos_rcu_wait_end(struct xr_qos_rcu_wait *wait);
void xr_qos_rcu_boost(struct xr_qos_link *link, struct task_struct *reader);
void xr_qos_fork(struct task_struct *p);
void xr_qos_exit(struct task_struct *p);
void xr_qos_free(struct task_struct *p);
int xr_qos_set(struct task_struct *p, u32 qos);
u32 xr_qos_get(struct task_struct *p);
u32 xr_qos_get_static(struct task_struct *p);
bool xr_qos_group_requested(struct task_struct *p);
int xr_qos_async_arm(struct task_struct *p, bool enable);
bool xr_qos_async_consume(struct task_struct *p);
int xr_qos_link_add(struct xr_qos_link *link, struct task_struct *from,
		    struct task_struct *to, enum xr_qos_dependency type);
void xr_qos_link_del(struct xr_qos_link *link);
struct xr_qos_vip_node *xr_qos_vip_node(struct task_struct *p);
void xr_qos_sched_apply(struct task_struct *p, int rt_prio, unsigned int vip);
int xr_qos_effective_prio(struct task_struct *p, int prio);
bool xr_qos_only_rt_boost(struct task_struct *p);
int walt_xr_qos_set_group(struct task_struct *p, unsigned int group_id);
#else
static inline void xr_qos_fork(struct task_struct *p) { }
static inline void xr_qos_exit(struct task_struct *p) { }
static inline void xr_qos_free(struct task_struct *p) { }
static inline int xr_qos_effective_prio(struct task_struct *p, int prio)
{ return prio; }
static inline bool xr_qos_only_rt_boost(struct task_struct *p)
{ return false; }
#endif

#endif
