/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_OPLUS_STABILITY_H
#define _LINUX_OPLUS_STABILITY_H

#include <linux/kconfig.h>
#include <linux/types.h>

struct binder_proc;
struct binder_transaction;
struct task_struct;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_STABILITY_HELPER)
void oplus_stability_thread_created(struct task_struct *task,
				    int system_threads);
void oplus_stability_binder_transaction_init(struct binder_transaction *t);
void oplus_stability_binder_transaction_queued_locked(
		struct binder_proc *proc, struct binder_transaction *t,
		bool pending_async);
#else
static inline void oplus_stability_thread_created(struct task_struct *task,
					  int system_threads) { }
static inline void oplus_stability_binder_transaction_init(
		struct binder_transaction *t) { }
static inline void oplus_stability_binder_transaction_queued_locked(
		struct binder_proc *proc, struct binder_transaction *t,
		bool pending_async) { }
#endif

#endif /* _LINUX_OPLUS_STABILITY_H */
