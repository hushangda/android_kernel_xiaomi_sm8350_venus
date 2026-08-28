/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_OPLUS_COLOROS_COMPAT_H
#define _LINUX_OPLUS_COLOROS_COMPAT_H

struct task_struct;

#ifdef CONFIG_OPLUS_COLOROS_COMPAT
int oplus_coloros_task_boost(struct task_struct *task, int native_boost);
#else
static inline int oplus_coloros_task_boost(struct task_struct *task,
					    int native_boost)
{
	return native_boost;
}
#endif

#endif
