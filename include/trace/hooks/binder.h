/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM binder
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_BINDER_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_BINDER_H
#include <linux/tracepoint.h>
#include <trace/hooks/vendor_hooks.h>
/*
 * Following tracepoints are not exported in tracefs and provide a
 * mechanism for vendor modules to hook and extend functionality
 */
#if defined(CONFIG_TRACEPOINTS) && defined(CONFIG_ANDROID_VENDOR_HOOKS)
struct binder_transaction;
struct binder_transaction_data;
struct binder_alloc;
struct binder_proc;
struct binder_thread;
struct hlist_head;
struct mutex;
struct task_struct;
DECLARE_HOOK(android_vh_binder_transaction_init,
	TP_PROTO(struct binder_transaction *t),
	TP_ARGS(t));
DECLARE_HOOK(android_vh_binder_set_priority,
	TP_PROTO(struct binder_transaction *t, struct task_struct *task),
	TP_ARGS(t, task));
DECLARE_HOOK(android_vh_binder_restore_priority,
	TP_PROTO(struct binder_transaction *t, struct task_struct *task),
	TP_ARGS(t, task));
DECLARE_HOOK(android_vh_binder_preset,
	TP_PROTO(struct hlist_head *hhead, struct mutex *lock),
	TP_ARGS(hhead, lock));
DECLARE_HOOK(android_vh_binder_trans,
	TP_PROTO(struct binder_proc *target_proc, struct binder_proc *proc,
		 struct binder_thread *thread,
		 struct binder_transaction_data *tr),
	TP_ARGS(target_proc, proc, thread, tr));
DECLARE_HOOK(android_vh_binder_reply,
	TP_PROTO(struct binder_proc *target_proc, struct binder_proc *proc,
		 struct binder_thread *thread,
		 struct binder_transaction_data *tr),
	TP_ARGS(target_proc, proc, thread, tr));
DECLARE_HOOK(android_vh_binder_alloc_new_buf_locked,
	TP_PROTO(size_t size, struct binder_alloc *alloc, int is_async),
	TP_ARGS(size, alloc, is_async));
#else
#define trace_android_vh_binder_transaction_init(t)
#define trace_android_vh_binder_set_priority(t, task)
#define trace_android_vh_binder_restore_priority(t, task)
#define trace_android_vh_binder_preset(hhead, lock)
#define trace_android_vh_binder_trans(target_proc, proc, thread, tr)
#define trace_android_vh_binder_reply(target_proc, proc, thread, tr)
#define trace_android_vh_binder_alloc_new_buf_locked(size, alloc, is_async)
#endif
#endif /* _TRACE_HOOK_BINDER_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
