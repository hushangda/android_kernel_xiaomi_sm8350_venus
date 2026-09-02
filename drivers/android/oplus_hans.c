// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compatibility backend for the OPlus HANS userspace freezer.
 *
 * The ABI follows OPlus' published raw-netlink protocol 29.  This
 * implementation is built into the kernel because the Venus configuration
 * does not enable loadable modules.
 */

#define pr_fmt(fmt) "oplus_hans: " fmt

#include <linux/cgroup.h>
#include <linux/freezer.h>
#include <linux/hashtable.h>
#include <linux/ipv6.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netlink.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/jobctl.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tcp.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <trace/hooks/binder.h>
#include <trace/hooks/signal.h>

#include "binder_alloc.h"
#include "binder_internal.h"

#if defined(CONFIG_CFS_BANDWIDTH)
#include "../../kernel/sched/sched.h"
#endif

#define HANS_RPC_NAME_LEN	140
#define HANS_PARCEL_OFFSET	16
#define HANS_MIN_APP_UID	10000
#define HANS_MAX_SYSTEM_UID	2000
#define HANS_SYSTEM_UID		1000
#define HANS_CPUCTL_VERSION	2
#define HANS_USE_CGROUP_V2	(-99)
#define HANS_CHECK_CGROUP_V2	(-8000)
#define HANS_SKIP_REPLY		(-9000)
#define HANS_UNSKIP_REPLY	(-9001)
#define HANS_TRANS_REPLY	(-100)
#define HANS_TRANS_TRANSACTION	(-200)

enum hans_message_type {
	HANS_ASYNC_BINDER,
	HANS_SYNC_BINDER,
	HANS_FROZEN_TRANS,
	HANS_SIGNAL,
	HANS_PACKAGE,
	HANS_SYNC_BINDER_CPUCTL,
	HANS_SIGNAL_CPUCTL,
	HANS_CPUCTL_TRANS,
	HANS_LOOP_BACK,
	HANS_TYPE_MAX,
};

enum hans_package_cmd {
	HANS_ADD_UID,
	HANS_DEL_UID,
	HANS_DEL_ALL_UIDS,
	HANS_PACKAGE_CMD_MAX,
};

/* Keep field order and native alignment exactly in sync with OPlus userspace. */
struct hans_message {
	int type;
	int port;
	int caller_uid;
	int caller_pid;
	int target_pid;
	int target_uid;
	int package_cmd;
	int code;
	char rpc_name[HANS_RPC_NAME_LEN];
	int persistent;
};

struct hans_uid_entry {
	uid_t uid;
	struct hlist_node node;
};

struct hans_persistent_uid_entry {
	uid_t uid;
	unsigned long last_data_jiffies;
	struct hlist_node node;
};

static atomic_t hans_daemon_port = ATOMIC_INIT(-1);
static struct sock *hans_netlink_sock;
static struct hlist_head *hans_binder_procs;
static struct mutex *hans_binder_procs_lock;
static bool hans_skip_binder_reply = true;

static DEFINE_HASHTABLE(hans_uid_map, 6);
static DEFINE_HASHTABLE(hans_persistent_uid_map, 5);
static DEFINE_SPINLOCK(hans_uid_lock);

static bool hans_daemon_ready(void)
{
	return atomic_read(&hans_daemon_port) > 0;
}

static int hans_report(enum hans_message_type type, int caller_pid,
		       int caller_uid, int target_pid, int target_uid,
		       const char *rpc_name, int code)
{
	struct hans_message message = {
		.type = type,
		.port = 0x15356,
		.caller_uid = caller_uid,
		.caller_pid = caller_pid,
		.target_pid = target_pid,
		.target_uid = target_uid,
		.package_cmd = -1,
		.code = code,
		.persistent = 0,
	};
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	int port;
	int ret;

	if (type < 0 || type >= HANS_TYPE_MAX)
		return -EINVAL;

	port = atomic_read(&hans_daemon_port);
	if (port <= 0)
		return -ENOTCONN;

	strlcpy(message.rpc_name, rpc_name ?: "", sizeof(message.rpc_name));
	skb = nlmsg_new(sizeof(message), GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, sizeof(message), 0);
	if (!nlh) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}
	memcpy(nlmsg_data(nlh), &message, sizeof(message));
	nlmsg_end(skb, nlh);

	ret = nlmsg_unicast(hans_netlink_sock, skb, port);
	if (ret == -ECONNREFUSED || ret == -ESRCH)
		atomic_cmpxchg(&hans_daemon_port, port, -1);

	return ret;
}

static bool hans_task_frozen(struct task_struct *task)
{
	struct task_struct *leader;
	bool result;

	if (!task)
		return false;

	rcu_read_lock();
	leader = task->group_leader;
	result = cgroup_task_frozen(task) ||
		 !!(READ_ONCE(task->jobctl) & JOBCTL_TRAP_FREEZE) ||
		 (leader && (frozen(leader) || freezing(leader)));
	rcu_read_unlock();

	return result;
}

static bool hans_task_cpu_limited(struct task_struct *task)
{
#if defined(CONFIG_CFS_BANDWIDTH)
	struct task_group *group;

	if (!task)
		return false;
	group = READ_ONCE(task->sched_task_group);
	return group && group->cfs_bandwidth.quota != RUNTIME_INF;
#else
	return false;
#endif
}

static int hans_task_uid(struct task_struct *task)
{
	return from_kuid_munged(&init_user_ns, task_uid(task));
}

static void hans_decode_interface_token(const struct binder_transaction_data *tr,
					char token[HANS_RPC_NAME_LEN])
{
	char raw[HANS_RPC_NAME_LEN];
	size_t size;
	size_t input;
	size_t output = 0;

	token[0] = '\0';
	if (!tr || tr->data_size <= HANS_PARCEL_OFFSET)
		return;

	size = min_t(size_t, tr->data_size, sizeof(raw));
	if (copy_from_user(raw, u64_to_user_ptr(tr->data.ptr.buffer), size))
		return;

	/* Java Parcel strings are UTF-16; the low bytes are enough for the ABI. */
	for (input = HANS_PARCEL_OFFSET;
	     input + 1 < size && output + 1 < HANS_RPC_NAME_LEN;
	     input += 2) {
		if (!raw[input])
			break;
		token[output++] = raw[input];
	}
	token[output] = '\0';
}

static void hans_binder_preset(void *unused, struct hlist_head *procs,
			       struct mutex *lock)
{
	if (!READ_ONCE(hans_binder_procs))
		WRITE_ONCE(hans_binder_procs, procs);
	if (!READ_ONCE(hans_binder_procs_lock))
		WRITE_ONCE(hans_binder_procs_lock, lock);
}

static void hans_binder_transaction(void *unused,
				    struct binder_proc *target_proc,
				    struct binder_proc *proc,
				    struct binder_thread *thread,
				    struct binder_transaction_data *tr)
{
	struct task_struct *caller;
	struct task_struct *target;
	char token[HANS_RPC_NAME_LEN];
	int caller_uid;
	int target_uid;
	bool oneway;

	if (!hans_daemon_ready() || !target_proc || !proc || !tr)
		return;
	caller = proc->tsk;
	target = target_proc->tsk;
	if (!caller || !target || proc->pid == target_proc->pid)
		return;

	caller_uid = hans_task_uid(caller);
	target_uid = hans_task_uid(target);
	oneway = !!(tr->flags & TF_ONE_WAY);

	if (!oneway && target_uid >= HANS_MIN_APP_UID &&
	    hans_task_frozen(target)) {
		hans_report(HANS_SYNC_BINDER, task_tgid_nr(caller), caller_uid,
			    task_tgid_nr(target), target_uid, "SYNC_BINDER", -1);
	}

	if (!oneway && (target_uid >= HANS_MIN_APP_UID ||
		       target_uid == HANS_SYSTEM_UID) &&
	    hans_task_cpu_limited(target)) {
		hans_report(HANS_SYNC_BINDER_CPUCTL, task_tgid_nr(caller),
			    caller_uid, task_tgid_nr(target), target_uid,
			    "SYNC_BINDER_CPUCTL", -1);
	}

	if (!oneway || target_uid < HANS_MIN_APP_UID ||
	    !hans_task_frozen(target))
		return;

	hans_decode_interface_token(tr, token);
	hans_report(HANS_ASYNC_BINDER, task_tgid_nr(caller), caller_uid,
		    task_tgid_nr(target), target_uid,
		    token[0] ? token : "ASYNC_BINDER", tr->code);
}

static void hans_binder_reply(void *unused, struct binder_proc *target_proc,
			      struct binder_proc *proc,
			      struct binder_thread *thread,
			      struct binder_transaction_data *tr)
{
	struct task_struct *caller;
	struct task_struct *target;
	int target_uid;

	if (!hans_daemon_ready() || !target_proc || !proc)
		return;
	caller = proc->tsk;
	target = target_proc->tsk;
	if (!caller || !target || proc->pid == target_proc->pid)
		return;
	target_uid = hans_task_uid(target);
	if (target_uid > HANS_MAX_SYSTEM_UID || !hans_task_frozen(target))
		return;

	hans_report(HANS_SYNC_BINDER, task_tgid_nr(caller),
		    hans_task_uid(caller), task_tgid_nr(target), target_uid,
		    "SYNC_BINDER_REPLY", -1);
}

static void hans_binder_buffer(void *unused, size_t size,
			       struct binder_alloc *alloc, int is_async)
{
	struct task_struct *target;

	if (!hans_daemon_ready() || !is_async || !alloc)
		return;
	if (alloc->free_async_space >= 3 *
	    (size + sizeof(struct binder_buffer)) &&
	    alloc->free_async_space >= 100 * 1024)
		return;

	target = find_get_task_by_vpid(alloc->pid);
	if (!target)
		return;
	if (hans_task_frozen(target))
		hans_report(HANS_ASYNC_BINDER, task_tgid_nr(current),
			    hans_task_uid(current), task_tgid_nr(target),
			    hans_task_uid(target), "free_buffer_full", -1);
	put_task_struct(target);
}

static void hans_signal(void *unused, int sig, struct task_struct *killer,
			struct task_struct *target)
{
	if (!hans_daemon_ready() || !killer || !target)
		return;

	if (hans_task_frozen(target) &&
	    (sig == SIGKILL || sig == SIGTERM || sig == SIGABRT ||
	     sig == SIGQUIT)) {
		hans_report(HANS_SIGNAL, task_tgid_nr(killer),
			    hans_task_uid(killer), task_tgid_nr(target),
			    hans_task_uid(target), "signal", sig);
	}

	if (hans_task_cpu_limited(target) &&
	    (sig == SIGKILL || sig == SIGTERM || sig == SIGABRT ||
	     sig == SIGQUIT || sig == SIGIO)) {
		hans_report(HANS_SIGNAL_CPUCTL, task_tgid_nr(killer),
			    hans_task_uid(killer), task_tgid_nr(target),
			    hans_task_uid(target), "signal_cpuctl", sig);
	}
}

static bool hans_sync_transaction(const struct binder_transaction *transaction)
{
	if (!transaction || (transaction->flags & TF_ONE_WAY))
		return false;
	return !hans_skip_binder_reply || transaction->need_reply;
}

/* Called with proc->inner_lock held. */
static bool hans_proc_has_sync_work(struct binder_proc *proc)
{
	struct binder_transaction *transaction;
	struct binder_thread *thread;
	struct binder_work *work;
	struct rb_node *node;

	list_for_each_entry(work, &proc->todo, entry) {
		if (work->type != BINDER_WORK_TRANSACTION)
			continue;
		transaction = container_of(work, struct binder_transaction, work);
		if (hans_sync_transaction(transaction))
			return true;
	}

	for (node = rb_first(&proc->threads); node; node = rb_next(node)) {
		thread = rb_entry(node, struct binder_thread, rb_node);
		list_for_each_entry(work, &thread->todo, entry) {
			if (work->type != BINDER_WORK_TRANSACTION)
				continue;
			transaction = container_of(work,
						   struct binder_transaction, work);
			if (hans_sync_transaction(transaction))
				return true;
		}
		transaction = thread->transaction_stack;
		if (transaction) {
			bool incoming;

			spin_lock(&transaction->lock);
			incoming = transaction->to_thread == thread &&
				   hans_sync_transaction(transaction);
			spin_unlock(&transaction->lock);
			if (incoming)
				return true;
		}
	}

	return false;
}

static void hans_check_frozen_transactions(uid_t uid,
					   enum hans_message_type type)
{
	struct hlist_head *procs = READ_ONCE(hans_binder_procs);
	struct mutex *lock = READ_ONCE(hans_binder_procs_lock);
	struct binder_proc *proc;
	int code = -1;
	bool found = false;

	if (!procs || !lock)
		return;

	mutex_lock(lock);
	hlist_for_each_entry(proc, procs, proc_node) {
		if (!proc->tsk || hans_task_uid(proc->tsk) != uid)
			continue;
		spin_lock(&proc->inner_lock);
		found = hans_proc_has_sync_work(proc);
		if (found)
			code = HANS_TRANS_TRANSACTION;
		spin_unlock(&proc->inner_lock);
		if (found)
			break;
	}
	mutex_unlock(lock);

	if (found)
		hans_report(type, -1, -1, -1, uid, "FROZEN_TRANS", code);
}

static bool hans_persistent_recent_locked(uid_t uid)
{
	struct hans_persistent_uid_entry *entry;

	hash_for_each_possible(hans_persistent_uid_map, entry, node, uid) {
		if (entry->uid == uid)
			return time_before(jiffies, entry->last_data_jiffies + HZ);
	}
	return false;
}

static int hans_add_uid(uid_t uid, int persistent)
{
	struct hans_persistent_uid_entry *persistent_entry;
	struct hans_persistent_uid_entry *persistent_iter;
	struct hans_uid_entry *entry;
	struct hans_uid_entry *uid_iter;
	unsigned long flags;
	bool report_now = false;

	if (uid < HANS_MIN_APP_UID || persistent < 0 || persistent > 2)
		return -EINVAL;

	if (persistent == 2) {
		spin_lock_irqsave(&hans_uid_lock, flags);
		hash_for_each_possible(hans_persistent_uid_map,
				       persistent_entry, node, uid) {
			if (persistent_entry->uid == uid) {
				hash_del(&persistent_entry->node);
				kfree(persistent_entry);
				break;
			}
		}
		spin_unlock_irqrestore(&hans_uid_lock, flags);
		return 0;
	}

	if (persistent == 1) {
		persistent_entry = kzalloc(sizeof(*persistent_entry), GFP_KERNEL);
		if (!persistent_entry)
			return -ENOMEM;
		persistent_entry->uid = uid;

		spin_lock_irqsave(&hans_uid_lock, flags);
		hash_for_each_possible(hans_persistent_uid_map, persistent_iter,
				       node, uid) {
			if (persistent_iter->uid == uid) {
				spin_unlock_irqrestore(&hans_uid_lock, flags);
				kfree(persistent_entry);
				return -EEXIST;
			}
		}
		hash_add(hans_persistent_uid_map, &persistent_entry->node, uid);
		spin_unlock_irqrestore(&hans_uid_lock, flags);
		return 0;
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->uid = uid;

	spin_lock_irqsave(&hans_uid_lock, flags);
	hash_for_each_possible(hans_uid_map, uid_iter, node, uid) {
		if (uid_iter->uid == uid) {
			spin_unlock_irqrestore(&hans_uid_lock, flags);
			kfree(entry);
			return -EEXIST;
		}
	}
	report_now = hans_persistent_recent_locked(uid);
	if (!report_now)
		hash_add(hans_uid_map, &entry->node, uid);
	spin_unlock_irqrestore(&hans_uid_lock, flags);

	if (report_now) {
		kfree(entry);
		hans_report(HANS_PACKAGE, -1, uid, -1, uid, "PKG", -1);
	}
	return 0;
}

static bool hans_take_uid(uid_t uid)
{
	struct hans_uid_entry *entry;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&hans_uid_lock, flags);
	hash_for_each_possible(hans_uid_map, entry, node, uid) {
		if (entry->uid == uid) {
			hash_del(&entry->node);
			kfree(entry);
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&hans_uid_lock, flags);
	return found;
}

static void hans_note_persistent_data(uid_t uid)
{
	struct hans_persistent_uid_entry *entry;
	unsigned long flags;

	spin_lock_irqsave(&hans_uid_lock, flags);
	hash_for_each_possible(hans_persistent_uid_map, entry, node, uid) {
		if (entry->uid == uid) {
			entry->last_data_jiffies = jiffies;
			break;
		}
	}
	spin_unlock_irqrestore(&hans_uid_lock, flags);
}

static void hans_clear_uids(void)
{
	struct hans_persistent_uid_entry *persistent_entry;
	struct hans_uid_entry *entry;
	struct hlist_node *tmp;
	unsigned long flags;
	int bucket;

	spin_lock_irqsave(&hans_uid_lock, flags);
	hash_for_each_safe(hans_uid_map, bucket, tmp, entry, node) {
		hash_del(&entry->node);
		kfree(entry);
	}
	hash_for_each_safe(hans_persistent_uid_map, bucket, tmp,
			   persistent_entry, node) {
		hash_del(&persistent_entry->node);
		kfree(persistent_entry);
	}
	spin_unlock_irqrestore(&hans_uid_lock, flags);
}

static void hans_del_uid(uid_t uid)
{
	hans_take_uid(uid);
}

static bool hans_tcp_packet_has_data(struct sk_buff *skb, bool *has_data)
{
	struct tcphdr tcp_buffer;
	const struct tcphdr *tcp;
	unsigned int transport_offset;
	unsigned int packet_end;
	unsigned int tcp_header_len;
	unsigned short fragment_offset = 0;
	u8 version;
	int protocol;

	*has_data = false;
	if (!pskb_may_pull(skb, skb_network_offset(skb) + 1))
		return false;
	version = ip_hdr(skb)->version;

	if (version == 4) {
		struct iphdr *ip;

		if (!pskb_may_pull(skb, skb_network_offset(skb) +
				   sizeof(struct iphdr)))
			return false;
		ip = ip_hdr(skb);
		if (ip->protocol != IPPROTO_TCP ||
		    (ntohs(ip->frag_off) & IP_OFFSET))
			return false;
		transport_offset = skb_network_offset(skb) + ip_hdrlen(skb);
		packet_end = skb_network_offset(skb) + ntohs(ip->tot_len);
	} else if (version == 6) {
#if IS_ENABLED(CONFIG_IPV6)
		struct ipv6hdr *ip6;

		if (!pskb_may_pull(skb, skb_network_offset(skb) +
				   sizeof(struct ipv6hdr)))
			return false;
		ip6 = ipv6_hdr(skb);
		transport_offset = 0;
		protocol = ipv6_find_hdr(skb, &transport_offset, -1,
					 &fragment_offset, NULL);
		if (protocol != IPPROTO_TCP || fragment_offset)
			return false;
		packet_end = skb_network_offset(skb) + sizeof(*ip6) +
			     ntohs(ip6->payload_len);
#else
		return false;
#endif
	} else {
		return false;
	}

	tcp = skb_header_pointer(skb, transport_offset, sizeof(tcp_buffer),
				 &tcp_buffer);
	if (!tcp || tcp->doff < 5)
		return false;
	tcp_header_len = tcp->doff * 4;
	if (packet_end > transport_offset + tcp_header_len)
		*has_data = true;
	return true;
}

static unsigned int hans_netfilter_input(void *priv, struct sk_buff *skb,
					 const struct nf_hook_state *state)
{
	struct sock *sk;
	uid_t uid;
	bool has_data;

	if (!hans_daemon_ready() || !skb || !skb->len || !state ||
	    state->hook != NF_INET_LOCAL_IN)
		return NF_ACCEPT;
	if (!hans_tcp_packet_has_data(skb, &has_data))
		return NF_ACCEPT;

	sk = skb_to_full_sk(skb);
	if (!sk || !sk_fullsock(sk))
		return NF_ACCEPT;
	uid = from_kuid_munged(&init_user_ns, sock_i_uid(sk));
	if (uid < HANS_MIN_APP_UID)
		return NF_ACCEPT;

	if (has_data)
		hans_note_persistent_data(uid);
	if (hans_take_uid(uid))
		hans_report(HANS_PACKAGE, -1, -1, -1, uid, "PKG", -1);

	return NF_ACCEPT;
}

static struct nf_hook_ops hans_nf_ops[] = {
	{
		.hook = hans_netfilter_input,
		.pf = NFPROTO_IPV4,
		.hooknum = NF_INET_LOCAL_IN,
		.priority = NF_IP_PRI_SELINUX_LAST + 1,
	},
#if IS_ENABLED(CONFIG_IPV6)
	{
		.hook = hans_netfilter_input,
		.pf = NFPROTO_IPV6,
		.hooknum = NF_INET_LOCAL_IN,
		.priority = NF_IP6_PRI_SELINUX_LAST + 1,
	},
#endif
};

static void hans_netlink_handler(struct sk_buff *skb)
{
	struct hans_message message = { };
	struct nlmsghdr *nlh;
	kuid_t sender;
	uid_t sender_uid;
	size_t length;
	int ret = 0;

	if (!skb || skb->len < NLMSG_SPACE(0))
		return;
	nlh = nlmsg_hdr(skb);
	if (!nlmsg_ok(nlh, skb->len))
		return;
	length = nlmsg_len(nlh);
	if (length < sizeof(message) - sizeof(message.persistent))
		return;
	memcpy(&message, nlmsg_data(nlh), min(length, sizeof(message)));

	sender = NETLINK_CREDS(skb)->uid;
	sender_uid = from_kuid_munged(&init_user_ns, sender);
	if (sender_uid != 0 && sender_uid != HANS_SYSTEM_UID)
		return;
	if (message.type < 0 || message.type >= HANS_TYPE_MAX)
		return;
	if (!hans_daemon_ready() && message.type != HANS_LOOP_BACK)
		return;

	switch (message.type) {
	case HANS_LOOP_BACK:
		if (message.port <= 0)
			return;
		atomic_set(&hans_daemon_port, message.port);
		hans_report(HANS_LOOP_BACK, -1, -1, -1, -1,
			    "loop back", HANS_CPUCTL_VERSION);
		/* Match the published ABI capability response expected by ofreezer. */
		hans_report(HANS_PACKAGE, -1, -1, -1, -1,
			    "PKG", HANS_USE_CGROUP_V2);
		pr_info("daemon handshake complete (uid=%u)\n", sender_uid);
		break;
	case HANS_PACKAGE:
		if (message.package_cmd < 0 ||
		    message.package_cmd >= HANS_PACKAGE_CMD_MAX)
			return;
		if (message.package_cmd != HANS_DEL_ALL_UIDS &&
		    message.target_uid < HANS_MIN_APP_UID)
			return;
		if (message.package_cmd == HANS_ADD_UID)
			ret = hans_add_uid(message.target_uid,
					   length < sizeof(message) ? 0 :
					   message.persistent);
		else if (message.package_cmd == HANS_DEL_UID)
			hans_del_uid(message.target_uid);
		else
			hans_clear_uids();
		break;
	case HANS_FROZEN_TRANS:
	case HANS_CPUCTL_TRANS:
		if (message.target_uid == HANS_CHECK_CGROUP_V2) {
			hans_report(HANS_PACKAGE, -1, -1, -1, -1,
				    "PKG", HANS_USE_CGROUP_V2);
		} else if (message.target_uid == HANS_SKIP_REPLY) {
			WRITE_ONCE(hans_skip_binder_reply, true);
		} else if (message.target_uid == HANS_UNSKIP_REPLY) {
			WRITE_ONCE(hans_skip_binder_reply, false);
		} else if (message.target_uid >= 0) {
			hans_check_frozen_transactions(message.target_uid,
						       message.type);
		} else {
			return;
		}
		break;
	default:
		return;
	}

	if (ret && ret != -EEXIST)
		pr_warn_ratelimited("message type=%d failed: %d\n",
				    message.type, ret);
}

static int hans_register_trace_hooks(void)
{
	int ret;

	ret = register_trace_android_vh_binder_preset(hans_binder_preset, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_binder_trans(hans_binder_transaction,
						     NULL);
	if (ret)
		goto unregister_preset;
	ret = register_trace_android_vh_binder_reply(hans_binder_reply, NULL);
	if (ret)
		goto unregister_transaction;
	ret = register_trace_android_vh_binder_alloc_new_buf_locked(
			hans_binder_buffer, NULL);
	if (ret)
		goto unregister_reply;
	ret = register_trace_android_vh_do_send_sig_info(hans_signal, NULL);
	if (ret)
		goto unregister_buffer;
	return 0;

unregister_buffer:
	unregister_trace_android_vh_binder_alloc_new_buf_locked(
			hans_binder_buffer, NULL);
unregister_reply:
	unregister_trace_android_vh_binder_reply(hans_binder_reply, NULL);
unregister_transaction:
	unregister_trace_android_vh_binder_trans(hans_binder_transaction, NULL);
unregister_preset:
	unregister_trace_android_vh_binder_preset(hans_binder_preset, NULL);
	return ret;
}

static void hans_unregister_trace_hooks(void)
{
	unregister_trace_android_vh_do_send_sig_info(hans_signal, NULL);
	unregister_trace_android_vh_binder_alloc_new_buf_locked(
			hans_binder_buffer, NULL);
	unregister_trace_android_vh_binder_reply(hans_binder_reply, NULL);
	unregister_trace_android_vh_binder_trans(hans_binder_transaction, NULL);
	unregister_trace_android_vh_binder_preset(hans_binder_preset, NULL);
}

static int __init oplus_hans_init(void)
{
	struct netlink_kernel_cfg cfg = {
		.input = hans_netlink_handler,
	};
	int ret;

	hash_init(hans_uid_map);
	hash_init(hans_persistent_uid_map);
	atomic_set(&hans_daemon_port, -1);

	hans_netlink_sock = netlink_kernel_create(&init_net,
						 NETLINK_OPLUS_HANS, &cfg);
	if (!hans_netlink_sock)
		return -ENOMEM;
	ret = hans_register_trace_hooks();
	if (ret)
		goto release_netlink;
	ret = nf_register_net_hooks(&init_net, hans_nf_ops,
				    ARRAY_SIZE(hans_nf_ops));
	if (ret)
		goto unregister_hooks;

	pr_info("raw-netlink protocol %d registered\n", NETLINK_OPLUS_HANS);
	return 0;

unregister_hooks:
	hans_unregister_trace_hooks();
release_netlink:
	netlink_kernel_release(hans_netlink_sock);
	hans_netlink_sock = NULL;
	return ret;
}

static void __exit oplus_hans_exit(void)
{
	atomic_set(&hans_daemon_port, -1);
	nf_unregister_net_hooks(&init_net, hans_nf_ops, ARRAY_SIZE(hans_nf_ops));
	hans_unregister_trace_hooks();
	if (hans_netlink_sock) {
		netlink_kernel_release(hans_netlink_sock);
		hans_netlink_sock = NULL;
	}
	hans_clear_uids();
}

module_init(oplus_hans_init);
module_exit(oplus_hans_exit);

MODULE_DESCRIPTION("OPlus HANS freezer compatibility backend");
MODULE_LICENSE("GPL v2");
