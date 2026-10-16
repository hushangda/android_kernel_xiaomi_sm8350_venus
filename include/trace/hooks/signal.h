/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM signal
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_SIGNAL_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_SIGNAL_H
#include <linux/tracepoint.h>
#include <trace/hooks/vendor_hooks.h>

#if defined(CONFIG_TRACEPOINTS) && defined(CONFIG_ANDROID_VENDOR_HOOKS)
struct task_struct;
DECLARE_HOOK(android_vh_do_send_sig_info,
	TP_PROTO(int sig, struct task_struct *killer, struct task_struct *dst),
	TP_ARGS(sig, killer, dst));
#else
#define trace_android_vh_do_send_sig_info(sig, killer, dst)
#endif
#endif /* _TRACE_HOOK_SIGNAL_H */

#include <trace/define_trace.h>
