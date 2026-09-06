// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compatibility backend for the OPlus LinkPower userspace framework.
 *
 * The ABI is taken from the ROM's NetlinkLinkPower implementation.  OPlus
 * netd's netdisable BPF programs accept TCP resets and packets carrying bit
 * 27 in skb->mark.  Commands 11-14 therefore maintain that bit on sockets;
 * they do not implement a second packet filter in the kernel.
 */

#define pr_fmt(fmt) "oplus_linkpower: " fmt

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/genetlink.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>
#include <linux/oplus_linkpower.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/user_namespace.h>
#include <linux/workqueue.h>
#include <net/genetlink.h>
#include <net/inet_hashtables.h>
#include <net/inet_sock.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <trace/events/tcp.h>

#define LINKPOWER_FAMILY_NAME		"linkpower"
#define LINKPOWER_FAMILY_VERSION	1
#define LINKPOWER_DYNAMIC_FAMILY_ID	0
#define LINKPOWER_ATTR_MAX		999
#define LINKPOWER_SYSTEM_UID		1000
#define LINKPOWER_MARK_ALLOW		BIT(27)
#define LINKPOWER_MAX_PAIRS		100
#define LINKPOWER_PORT_PID_SLOTS	50
#define LINKPOWER_CONNECT_SLOTS		10
#define LINKPOWER_PUSH_CONFIG_SLOTS	4
#define LINKPOWER_PUSH_SOCKET_SLOTS	16
#define LINKPOWER_CLOSE_LIMIT		4096

enum linkpower_cmd {
	LINKPOWER_CMD_UNSPEC,
	LINKPOWER_CMD_DOWNLINK,
	LINKPOWER_CMD_UPLINK,
};

enum linkpower_message {
	LP_REQUEST_PORT_PID = 1,
	LP_RESPONSE_PORT_PID = 2,
	LP_REQUEST_CLOSE_PROCESS = 3,
	LP_RESPONSE_CLOSE_PROCESS = 4,
	LP_REQUEST_START_PUSH = 5,
	LP_REQUEST_STOP_PUSH = 6,
	LP_UNSOL_PUSH = 7,
	LP_REQUEST_MONITOR_CONNECT = 8,
	LP_REQUEST_CONNECT_INFO = 9,
	LP_RESPONSE_CONNECT_INFO = 10,
	LP_REQUEST_SET_PID_WHITE = 11,
	LP_REQUEST_DELETE_PID_WHITE = 12,
	LP_REQUEST_SET_DUAL_WHITE = 13,
	LP_REQUEST_DELETE_DUAL_WHITE = 14,
	LP_REQUEST_QRTR_WAKEUP = 401,
	LP_RESPONSE_QRTR_WAKEUP = 402,
	LP_REQUEST_IRQ_DATA_WAKEUP = 501,
	LP_RESPONSE_IRQ_DATA_WAKEUP = 502,
	LP_UNSOL_PID_WHITE_WAKEUP = 503,
};

struct linkpower_pair {
	u32 uid;
	u32 pid;
};

/* Native little-endian layout: Java reads each entry as one packed int. */
struct linkpower_port_pid {
	u16 port;
	u16 pid;
};

/* Native layout is four ints per entry in NetlinkLinkPower.java. */
struct linkpower_connect_info {
	u32 uid;
	u32 pid;
	u16 connect_count;
	u16 send_rst_count;
	u16 receive_rst_count;
	u16 retransmit_count;
};

struct linkpower_close_response {
	u32 uid;
	u32 pid;
	u32 count;
};

struct linkpower_close_work {
	struct work_struct work;
	u32 uid;
	u32 pid;
};

struct linkpower_qrtr_info {
	u16 service_id;
	u16 message_id;
	u32 count;
};

/* Exactly 32 bytes, matching requestStartMonitorPushSock(). */
struct linkpower_push_config {
	u32 uid;
	u32 beat_length;
	u32 beat_feature_length;
	u8 beat_feature[8];
	u32 push_feature_length;
	u8 push_feature[8];
};

struct linkpower_push_socket {
	u64 cookie;
	u32 uid;
	u32 pid;
	u16 beat_count;
	u16 push_count;
	bool notified;
};

struct linkpower_push_report {
	u32 uid;
	u32 pid;
	u32 type;
	u16 beat_count;
	u16 push_count;
};

static atomic_t linkpower_daemon_port = ATOMIC_INIT(0);
static struct workqueue_struct *linkpower_close_wq;

static DEFINE_SPINLOCK(linkpower_white_lock);
static struct linkpower_pair linkpower_pid_white[LINKPOWER_MAX_PAIRS];
static struct linkpower_pair linkpower_dual_white[LINKPOWER_MAX_PAIRS];
static unsigned int linkpower_pid_white_count;
static unsigned int linkpower_dual_white_count;
static atomic_t linkpower_white_active = ATOMIC_INIT(0);

static DEFINE_SPINLOCK(linkpower_port_lock);
static struct linkpower_port_pid
	linkpower_port_pid[LINKPOWER_PORT_PID_SLOTS];
static unsigned int linkpower_port_pid_next;

static DEFINE_SPINLOCK(linkpower_connect_lock);
static struct linkpower_connect_info
	linkpower_connect[LINKPOWER_CONNECT_SLOTS];
static atomic_t linkpower_connect_active = ATOMIC_INIT(0);

static DEFINE_SPINLOCK(linkpower_qrtr_lock);
static struct linkpower_qrtr_info linkpower_qrtr[30];

static DEFINE_SPINLOCK(linkpower_push_lock);
static struct linkpower_push_config
	linkpower_push_config[LINKPOWER_PUSH_CONFIG_SLOTS];
static struct linkpower_push_socket
	linkpower_push_socket[LINKPOWER_PUSH_SOCKET_SLOTS];
static atomic_t linkpower_push_active = ATOMIC_INIT(0);

static int linkpower_genl_handler(struct sk_buff *skb,
				  struct genl_info *info);
static int linkpower_send_to_user(u16 type, const void *data, size_t length);

static const struct genl_ops linkpower_genl_ops[] = {
	{
		.cmd = LINKPOWER_CMD_DOWNLINK,
		.doit = linkpower_genl_handler,
	},
};

static struct genl_family linkpower_genl_family = {
	.id = LINKPOWER_DYNAMIC_FAMILY_ID,
	.hdrsize = 0,
	.name = LINKPOWER_FAMILY_NAME,
	.version = LINKPOWER_FAMILY_VERSION,
	.maxattr = LINKPOWER_ATTR_MAX,
	.ops = linkpower_genl_ops,
	.n_ops = ARRAY_SIZE(linkpower_genl_ops),
	.module = THIS_MODULE,
};

static u32 linkpower_sock_uid(const struct sock *sk)
{
	return from_kuid_munged(&init_user_ns, READ_ONCE(sk->sk_uid));
}

static u32 linkpower_sock_pid(const struct sock *sk)
{
	return (u32)READ_ONCE(sk->android_kabi_reserved7);
}

static bool linkpower_pair_match(const struct linkpower_pair *pair,
				 u32 uid, u32 pid)
{
	return pair->uid == uid && pair->pid == pid;
}

static bool linkpower_socket_is_white(u32 uid, u32 pid)
{
	unsigned long flags;
	unsigned int i;
	bool found = false;

	if (!atomic_read(&linkpower_white_active) || !uid || !pid)
		return false;

	spin_lock_irqsave(&linkpower_white_lock, flags);
	for (i = 0; i < linkpower_pid_white_count; i++) {
		if (linkpower_pair_match(&linkpower_pid_white[i], uid, pid)) {
			found = true;
			goto out;
		}
	}
	for (i = 0; i < linkpower_dual_white_count; i++) {
		if (linkpower_pair_match(&linkpower_dual_white[i], uid, pid)) {
			found = true;
			break;
		}
	}
out:
	spin_unlock_irqrestore(&linkpower_white_lock, flags);
	return found;
}

static void linkpower_apply_socket_mark(struct sock *sk)
{
	u32 mark;

	if (!sk || !sk_fullsock(sk))
		return;
	mark = READ_ONCE(sk->sk_mark);
	if (linkpower_socket_is_white(linkpower_sock_uid(sk),
				      linkpower_sock_pid(sk)))
		mark |= LINKPOWER_MARK_ALLOW;
	else
		mark &= ~LINKPOWER_MARK_ALLOW;
	WRITE_ONCE(sk->sk_mark, mark);
}

void oplus_linkpower_socket_created(struct sock *sk)
{
	if (!sk)
		return;

	/* OPlus' data-wakeup ABI reserves slot 7 for the creator TGID. */
	WRITE_ONCE(sk->android_kabi_reserved7,
		   current->mm ? (u64)task_tgid_nr(current) : 0);
	if (atomic_read(&linkpower_white_active))
		linkpower_apply_socket_mark(sk);
}

static void linkpower_count_inc(u16 *count)
{
	if (*count != U16_MAX)
		(*count)++;
}

enum linkpower_connect_event {
	LP_CONNECT,
	LP_SEND_RST,
	LP_RECEIVE_RST,
	LP_RETRANSMIT,
};

static void linkpower_account_connect(const struct sock *sk,
				      enum linkpower_connect_event event)
{
	unsigned long flags;
	unsigned int i;
	u32 uid;
	u32 pid;

	if (!sk || !atomic_read(&linkpower_connect_active))
		return;
	uid = linkpower_sock_uid(sk);
	pid = linkpower_sock_pid(sk);

	spin_lock_irqsave(&linkpower_connect_lock, flags);
	for (i = 0; i < LINKPOWER_CONNECT_SLOTS; i++) {
		struct linkpower_connect_info *entry = &linkpower_connect[i];

		if (!entry->uid)
			continue;
		if (entry->uid != uid || (entry->pid && entry->pid != pid))
			continue;
		switch (event) {
		case LP_CONNECT:
			linkpower_count_inc(&entry->connect_count);
			break;
		case LP_SEND_RST:
			linkpower_count_inc(&entry->send_rst_count);
			break;
		case LP_RECEIVE_RST:
			linkpower_count_inc(&entry->receive_rst_count);
			break;
		case LP_RETRANSMIT:
			linkpower_count_inc(&entry->retransmit_count);
			break;
		}
		break;
	}
	spin_unlock_irqrestore(&linkpower_connect_lock, flags);
}

static void linkpower_trace_retransmit(void *unused, const struct sock *sk,
				       const struct sk_buff *skb)
{
	linkpower_account_connect(sk, LP_RETRANSMIT);
}

static void linkpower_trace_send_reset(void *unused, const struct sock *sk,
				       const struct sk_buff *skb)
{
	linkpower_account_connect(sk, LP_SEND_RST);
}

static void linkpower_trace_receive_reset(void *unused, struct sock *sk)
{
	linkpower_account_connect(sk, LP_RECEIVE_RST);
}

void oplus_linkpower_tcp_connected(struct sock *sk)
{
	struct linkpower_port_pid entry;
	struct inet_sock *inet;
	unsigned long flags;
	u32 pid;
	u32 uid;

	if (!sk)
		return;
	linkpower_account_connect(sk, LP_CONNECT);

	uid = linkpower_sock_uid(sk);
	if (uid != LINKPOWER_SYSTEM_UID)
		return;
	pid = linkpower_sock_pid(sk);
	inet = inet_sk(sk);
	if (!pid || pid > U16_MAX || !inet->inet_sport)
		return;

	entry.port = ntohs(inet->inet_sport);
	entry.pid = pid;
	spin_lock_irqsave(&linkpower_port_lock, flags);
	linkpower_port_pid[linkpower_port_pid_next] = entry;
	linkpower_port_pid_next =
		(linkpower_port_pid_next + 1) % LINKPOWER_PORT_PID_SLOTS;
	spin_unlock_irqrestore(&linkpower_port_lock, flags);
}

void oplus_linkpower_qrtr_packet(u16 service_id, u16 message_id)
{
	unsigned long flags;
	unsigned int i;

	if (!service_id || !message_id)
		return;
	spin_lock_irqsave(&linkpower_qrtr_lock, flags);
	for (i = 0; i < ARRAY_SIZE(linkpower_qrtr); i++) {
		struct linkpower_qrtr_info *entry = &linkpower_qrtr[i];

		if (entry->service_id == service_id &&
		    entry->message_id == message_id) {
			if (entry->count != U32_MAX)
				entry->count++;
			break;
		}
		if (!entry->service_id && !entry->message_id) {
			entry->service_id = service_id;
			entry->message_id = message_id;
			entry->count = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&linkpower_qrtr_lock, flags);
}

void oplus_linkpower_irq_wakeup(unsigned int irq)
{
	/* Reserved for the 501/502 ABI once its nine counter meanings are known. */
}

static bool linkpower_socket_match(const struct sock *sk, u32 uid, u32 pid)
{
	return sk_fullsock(sk) && linkpower_sock_uid(sk) == uid &&
	       linkpower_sock_pid(sk) == pid && !sock_flag(sk, SOCK_DEAD);
}

static unsigned int linkpower_close_tcp_established(u32 uid, u32 pid)
{
	unsigned int bucket;
	unsigned int count = 0;

	for (bucket = 0; bucket <= tcp_hashinfo.ehash_mask &&
	     count < LINKPOWER_CLOSE_LIMIT; bucket++) {
		struct inet_ehash_bucket *head = &tcp_hashinfo.ehash[bucket];
		spinlock_t *lock = inet_ehash_lockp(&tcp_hashinfo, bucket);

		for (;;) {
			struct hlist_nulls_node *node;
			struct sock *target = NULL;
			struct sock *sk;

			spin_lock_bh(lock);
			sk_nulls_for_each(sk, node, &head->chain) {
				if (!linkpower_socket_match(sk, uid, pid))
					continue;
				if (refcount_inc_not_zero(&sk->sk_refcnt))
					target = sk;
				break;
			}
			spin_unlock_bh(lock);
			if (!target)
				break;
			if (!tcp_abort(target, ECONNABORTED))
				count++;
			sock_put(target);
			if (count >= LINKPOWER_CLOSE_LIMIT)
				break;
			cond_resched();
		}
	}
	return count;
}

static unsigned int linkpower_close_tcp_listeners(u32 uid, u32 pid,
					   unsigned int count)
{
	unsigned int bucket;

	for (bucket = 0; bucket < INET_LHTABLE_SIZE &&
	     count < LINKPOWER_CLOSE_LIMIT; bucket++) {
		struct inet_listen_hashbucket *head =
			&tcp_hashinfo.listening_hash[bucket];

		for (;;) {
			struct sock *target = NULL;
			struct sock *sk;

			spin_lock_bh(&head->lock);
			sk_for_each(sk, &head->head) {
				if (!linkpower_socket_match(sk, uid, pid))
					continue;
				if (refcount_inc_not_zero(&sk->sk_refcnt))
					target = sk;
				break;
			}
			spin_unlock_bh(&head->lock);
			if (!target)
				break;
			if (!tcp_abort(target, ECONNABORTED))
				count++;
			sock_put(target);
			if (count >= LINKPOWER_CLOSE_LIMIT)
				break;
			cond_resched();
		}
	}
	return count;
}

static unsigned int linkpower_close_udp(u32 uid, u32 pid,
					unsigned int count)
{
	unsigned int bucket;

	for (bucket = 0; bucket <= udp_table.mask &&
	     count < LINKPOWER_CLOSE_LIMIT; bucket++) {
		struct udp_hslot *head = &udp_table.hash[bucket];

		for (;;) {
			struct sock *target = NULL;
			struct sock *sk;

			spin_lock_bh(&head->lock);
			sk_for_each(sk, &head->head) {
				if (!linkpower_socket_match(sk, uid, pid))
					continue;
				if (refcount_inc_not_zero(&sk->sk_refcnt))
					target = sk;
				break;
			}
			spin_unlock_bh(&head->lock);
			if (!target)
				break;
			if (!udp_abort(target, ECONNABORTED))
				count++;
			sock_put(target);
			if (count >= LINKPOWER_CLOSE_LIMIT)
				break;
			cond_resched();
		}
	}
	return count;
}

static unsigned int linkpower_close_process(u32 uid, u32 pid)
{
	unsigned int count;

	count = linkpower_close_tcp_established(uid, pid);
	count = linkpower_close_tcp_listeners(uid, pid, count);
	return linkpower_close_udp(uid, pid, count);
}

static void linkpower_close_worker(struct work_struct *work)
{
	struct linkpower_close_work *close_work =
		container_of(work, struct linkpower_close_work, work);
	struct linkpower_close_response response = {
		.uid = close_work->uid,
		.pid = close_work->pid,
	};

	response.count = linkpower_close_process(response.uid, response.pid);
	linkpower_send_to_user(LP_RESPONSE_CLOSE_PROCESS, &response,
			       sizeof(response));
	kfree(close_work);
}

static int linkpower_queue_close(u32 uid, u32 pid)
{
	struct linkpower_close_work *close_work;

	close_work = kzalloc(sizeof(*close_work), GFP_KERNEL);
	if (!close_work)
		return -ENOMEM;
	close_work->uid = uid;
	close_work->pid = pid;
	INIT_WORK(&close_work->work, linkpower_close_worker);
	if (!queue_work(linkpower_close_wq, &close_work->work)) {
		kfree(close_work);
		return -EBUSY;
	}
	return 0;
}

static unsigned int linkpower_copy_pairs(struct linkpower_pair *destination,
					 const void *data, size_t length)
{
	const struct linkpower_pair *source = data;
	unsigned int count;
	unsigned int i;

	count = min_t(unsigned int, length / sizeof(*source),
		      LINKPOWER_MAX_PAIRS);
	for (i = 0; i < count; i++) {
		if (!source[i].uid || !source[i].pid)
			break;
		destination[i] = source[i];
	}
	if (i < LINKPOWER_MAX_PAIRS)
		memset(&destination[i], 0,
		       sizeof(*destination) * (LINKPOWER_MAX_PAIRS - i));
	return i;
}

static int linkpower_set_white(bool dual, const void *data, size_t length)
{
	struct linkpower_pair pairs[LINKPOWER_MAX_PAIRS] = { };
	unsigned long flags;
	unsigned int count;

	if (!data || length < sizeof(struct linkpower_pair) ||
	    length > sizeof(pairs) || length % sizeof(struct linkpower_pair))
		return -EINVAL;
	count = linkpower_copy_pairs(pairs, data, length);
	if (!count)
		return -EINVAL;

	spin_lock_irqsave(&linkpower_white_lock, flags);
	if (dual) {
		memcpy(linkpower_dual_white, pairs, sizeof(pairs));
		linkpower_dual_white_count = count;
	} else {
		memcpy(linkpower_pid_white, pairs, sizeof(pairs));
		linkpower_pid_white_count = count;
	}
	atomic_set(&linkpower_white_active,
		   linkpower_pid_white_count || linkpower_dual_white_count);
	spin_unlock_irqrestore(&linkpower_white_lock, flags);
	return 0;
}

static void linkpower_delete_white(bool dual)
{
	unsigned long flags;

	spin_lock_irqsave(&linkpower_white_lock, flags);
	if (dual) {
		memset(linkpower_dual_white, 0, sizeof(linkpower_dual_white));
		linkpower_dual_white_count = 0;
	} else {
		memset(linkpower_pid_white, 0, sizeof(linkpower_pid_white));
		linkpower_pid_white_count = 0;
	}
	atomic_set(&linkpower_white_active,
		   linkpower_pid_white_count || linkpower_dual_white_count);
	spin_unlock_irqrestore(&linkpower_white_lock, flags);
}

static int linkpower_send_to_user(u16 type, const void *data, size_t length)
{
	struct sk_buff *skb;
	void *header;
	int port;
	int ret;

	port = atomic_read(&linkpower_daemon_port);
	if (port <= 0)
		return -ENOTCONN;
	skb = genlmsg_new(nla_total_size(length), GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	header = genlmsg_put(skb, 0, 0, &linkpower_genl_family, 0,
			     LINKPOWER_CMD_UPLINK);
	if (!header) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}
	ret = nla_put(skb, type, length, data);
	if (ret) {
		genlmsg_cancel(skb, header);
		kfree_skb(skb);
		return ret;
	}
	genlmsg_end(skb, header);
	ret = genlmsg_unicast(&init_net, skb, port);
	if (ret == -ECONNREFUSED || ret == -ESRCH)
		atomic_cmpxchg(&linkpower_daemon_port, port, 0);
	return ret;
}

static int linkpower_reply_port_pid(void)
{
	struct linkpower_port_pid reply[LINKPOWER_PORT_PID_SLOTS] = { };
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&linkpower_port_lock, flags);
	for (i = 0; i < LINKPOWER_PORT_PID_SLOTS; i++)
		reply[i] = linkpower_port_pid[
			(linkpower_port_pid_next + i) % LINKPOWER_PORT_PID_SLOTS];
	memset(linkpower_port_pid, 0, sizeof(linkpower_port_pid));
	linkpower_port_pid_next = 0;
	spin_unlock_irqrestore(&linkpower_port_lock, flags);
	return linkpower_send_to_user(LP_RESPONSE_PORT_PID, reply,
				      sizeof(reply));
}

static int linkpower_set_connect_monitor(const void *data, size_t length)
{
	const struct linkpower_pair *pair = data;
	unsigned long flags;
	unsigned int i;
	int empty = -1;

	if (!data || length < sizeof(*pair) || !pair->uid)
		return -EINVAL;
	spin_lock_irqsave(&linkpower_connect_lock, flags);
	for (i = 0; i < LINKPOWER_CONNECT_SLOTS; i++) {
		if (!linkpower_connect[i].uid && empty < 0)
			empty = i;
		if (linkpower_connect[i].uid == pair->uid &&
		    linkpower_connect[i].pid == pair->pid) {
			memset(&linkpower_connect[i], 0,
			       sizeof(linkpower_connect[i]));
			linkpower_connect[i].uid = pair->uid;
			linkpower_connect[i].pid = pair->pid;
			goto out;
		}
	}
	if (empty < 0) {
		spin_unlock_irqrestore(&linkpower_connect_lock, flags);
		return -ENOSPC;
	}
	linkpower_connect[empty].uid = pair->uid;
	linkpower_connect[empty].pid = pair->pid;
out:
	atomic_set(&linkpower_connect_active, 1);
	spin_unlock_irqrestore(&linkpower_connect_lock, flags);
	return 0;
}

static int linkpower_reply_connect(void)
{
	struct linkpower_connect_info reply[LINKPOWER_CONNECT_SLOTS];
	unsigned long flags;

	spin_lock_irqsave(&linkpower_connect_lock, flags);
	memcpy(reply, linkpower_connect, sizeof(reply));
	memset(linkpower_connect, 0, sizeof(linkpower_connect));
	atomic_set(&linkpower_connect_active, 0);
	spin_unlock_irqrestore(&linkpower_connect_lock, flags);
	return linkpower_send_to_user(LP_RESPONSE_CONNECT_INFO, reply,
				      sizeof(reply));
}

static int linkpower_reply_qrtr(void)
{
	struct linkpower_qrtr_info reply[30];
	unsigned long flags;

	spin_lock_irqsave(&linkpower_qrtr_lock, flags);
	memcpy(reply, linkpower_qrtr, sizeof(reply));
	memset(linkpower_qrtr, 0, sizeof(linkpower_qrtr));
	spin_unlock_irqrestore(&linkpower_qrtr_lock, flags);
	return linkpower_send_to_user(LP_RESPONSE_QRTR_WAKEUP, reply,
				      sizeof(reply));
}

static bool linkpower_skb_has_feature(const struct sk_buff *skb,
				      unsigned int offset,
				      unsigned int payload_length,
				      const u8 *feature,
				      unsigned int feature_length)
{
	u8 sample[8];
	unsigned int scan_length;
	unsigned int i;

	if (!feature_length || feature_length > sizeof(sample) ||
	    payload_length < feature_length)
		return false;
	/* OPlus' heartbeat signatures are near the start of the TCP payload. */
	scan_length = min_t(unsigned int, payload_length, 256);
	for (i = 0; i + feature_length <= scan_length; i++) {
		if (skb_copy_bits(skb, offset + i, sample, feature_length))
			return false;
		if (!memcmp(sample, feature, feature_length))
			return true;
	}
	return false;
}

static struct linkpower_push_config *
linkpower_find_push_config_locked(u32 uid)
{
	unsigned int i;

	for (i = 0; i < LINKPOWER_PUSH_CONFIG_SLOTS; i++) {
		if (linkpower_push_config[i].uid == uid)
			return &linkpower_push_config[i];
	}
	return NULL;
}

static struct linkpower_push_socket *
linkpower_find_push_socket_locked(u64 cookie, u32 uid, u32 pid)
{
	struct linkpower_push_socket *empty = NULL;
	unsigned int i;

	for (i = 0; i < LINKPOWER_PUSH_SOCKET_SLOTS; i++) {
		struct linkpower_push_socket *entry = &linkpower_push_socket[i];

		if (entry->cookie == cookie)
			return entry;
		if (!entry->cookie && !empty)
			empty = entry;
	}
	if (!empty)
		return NULL;
	empty->cookie = cookie;
	empty->uid = uid;
	empty->pid = pid;
	return empty;
}

static void linkpower_push_packet(struct sk_buff *skb, bool output)
{
	struct linkpower_push_config config;
	struct linkpower_push_config *config_entry;
	struct linkpower_push_socket *entry;
	struct linkpower_push_report report;
	struct tcphdr tcp_buffer;
	struct tcphdr *tcp;
	struct sock *sk;
	unsigned long flags;
	unsigned int payload_length;
	unsigned int payload_offset;
	unsigned int transport_offset;
	u64 cookie;
	u32 uid;
	u32 pid;
	bool beat = false;
	bool push = false;
	bool notify = false;

	if (!atomic_read(&linkpower_push_active))
		return;
	sk = skb_to_full_sk(skb);
	if (!sk || sk->sk_protocol != IPPROTO_TCP)
		return;
	uid = linkpower_sock_uid(sk);
	pid = linkpower_sock_pid(sk);
	if (!uid || !pid)
		return;

	spin_lock_irqsave(&linkpower_push_lock, flags);
	config_entry = linkpower_find_push_config_locked(uid);
	if (!config_entry) {
		spin_unlock_irqrestore(&linkpower_push_lock, flags);
		return;
	}
	memcpy(&config, config_entry, sizeof(config));
	spin_unlock_irqrestore(&linkpower_push_lock, flags);

	transport_offset = skb_transport_offset(skb);
	tcp = skb_header_pointer(skb, transport_offset, sizeof(tcp_buffer),
				 &tcp_buffer);
	if (!tcp || tcp->doff < sizeof(*tcp) / 4)
		return;
	payload_offset = transport_offset + tcp->doff * 4;
	if (payload_offset > skb->len)
		return;
	payload_length = skb->len - payload_offset;
	if (!payload_length)
		return;

	if (output && payload_length == config.beat_length)
		beat = linkpower_skb_has_feature(skb, payload_offset,
						 payload_length,
						 config.beat_feature,
						 config.beat_feature_length);
	else if (!output)
		push = linkpower_skb_has_feature(skb, payload_offset,
						 payload_length,
						 config.push_feature,
						 config.push_feature_length);
	if (!beat && !push)
		return;

	cookie = sock_gen_cookie(sk);
	spin_lock_irqsave(&linkpower_push_lock, flags);
	entry = linkpower_find_push_socket_locked(cookie, uid, pid);
	if (entry) {
		if (beat)
			linkpower_count_inc(&entry->beat_count);
		if (push)
			linkpower_count_inc(&entry->push_count);
		if (!entry->notified) {
			entry->notified = true;
			report.uid = entry->uid;
			report.pid = entry->pid;
			report.type = 1;
			report.beat_count = entry->beat_count;
			report.push_count = entry->push_count;
			notify = true;
		}
	}
	spin_unlock_irqrestore(&linkpower_push_lock, flags);
	if (notify)
		linkpower_send_to_user(LP_UNSOL_PUSH, &report, sizeof(report));
}

static int linkpower_start_push(const void *data, size_t length)
{
	const struct linkpower_push_config *request = data;
	unsigned long flags;
	unsigned int i;
	int empty = -1;

	if (!data || length < sizeof(*request) || !request->uid ||
	    !request->beat_length || request->beat_feature_length > 8 ||
	    request->push_feature_length > 8 ||
	    !request->beat_feature_length || !request->push_feature_length)
		return -EINVAL;

	spin_lock_irqsave(&linkpower_push_lock, flags);
	for (i = 0; i < LINKPOWER_PUSH_CONFIG_SLOTS; i++) {
		if (!linkpower_push_config[i].uid && empty < 0)
			empty = i;
		if (linkpower_push_config[i].uid == request->uid) {
			memcpy(&linkpower_push_config[i], request,
			       sizeof(*request));
			goto out;
		}
	}
	if (empty < 0) {
		spin_unlock_irqrestore(&linkpower_push_lock, flags);
		return -ENOSPC;
	}
	memcpy(&linkpower_push_config[empty], request, sizeof(*request));
out:
	atomic_set(&linkpower_push_active, 1);
	spin_unlock_irqrestore(&linkpower_push_lock, flags);
	return 0;
}

static int linkpower_stop_push(const void *data, size_t length)
{
	struct linkpower_push_report reports[LINKPOWER_PUSH_SOCKET_SLOTS];
	unsigned long flags;
	unsigned int report_count = 0;
	unsigned int active_count = 0;
	unsigned int i;
	u32 uid;

	if (!data || length < sizeof(uid))
		return -EINVAL;
	memcpy(&uid, data, sizeof(uid));
	if (!uid)
		return -EINVAL;

	spin_lock_irqsave(&linkpower_push_lock, flags);
	for (i = 0; i < LINKPOWER_PUSH_CONFIG_SLOTS; i++) {
		if (linkpower_push_config[i].uid == uid)
			memset(&linkpower_push_config[i], 0,
			       sizeof(linkpower_push_config[i]));
		if (linkpower_push_config[i].uid)
			active_count++;
	}
	for (i = 0; i < LINKPOWER_PUSH_SOCKET_SLOTS; i++) {
		struct linkpower_push_socket *entry = &linkpower_push_socket[i];

		if (!entry->cookie || entry->uid != uid)
			continue;
		reports[report_count].uid = entry->uid;
		reports[report_count].pid = entry->pid;
		reports[report_count].type = 3;
		reports[report_count].beat_count = entry->beat_count;
		reports[report_count].push_count = entry->push_count;
		report_count++;
		memset(entry, 0, sizeof(*entry));
	}
	atomic_set(&linkpower_push_active, active_count != 0);
	spin_unlock_irqrestore(&linkpower_push_lock, flags);

	for (i = 0; i < report_count; i++)
		linkpower_send_to_user(LP_UNSOL_PUSH, &reports[i],
				       sizeof(reports[i]));
	return 0;
}

static void linkpower_trace_destroy(void *unused, struct sock *sk)
{
	struct linkpower_push_report report;
	unsigned long flags;
	unsigned int i;
	u64 cookie;
	bool notify = false;

	if (!sk || !atomic_read(&linkpower_push_active))
		return;
	cookie = sock_gen_cookie(sk);
	spin_lock_irqsave(&linkpower_push_lock, flags);
	for (i = 0; i < LINKPOWER_PUSH_SOCKET_SLOTS; i++) {
		struct linkpower_push_socket *entry = &linkpower_push_socket[i];

		if (entry->cookie != cookie)
			continue;
		report.uid = entry->uid;
		report.pid = entry->pid;
		report.type = 2;
		report.beat_count = entry->beat_count;
		report.push_count = entry->push_count;
		memset(entry, 0, sizeof(*entry));
		notify = true;
		break;
	}
	spin_unlock_irqrestore(&linkpower_push_lock, flags);
	if (notify)
		linkpower_send_to_user(LP_UNSOL_PUSH, &report, sizeof(report));
}

static unsigned int linkpower_mark_packet(void *priv, struct sk_buff *skb,
					  const struct nf_hook_state *state)
{
	struct sock *sk;

	sk = skb_to_full_sk(skb);
	if (sk && sk_fullsock(sk)) {
		/* Evaluate lazily to avoid a full socket-table scan per rule update. */
		linkpower_apply_socket_mark(sk);
		if (READ_ONCE(sk->sk_mark) & LINKPOWER_MARK_ALLOW)
			skb->mark |= LINKPOWER_MARK_ALLOW;
		else
			skb->mark &= ~LINKPOWER_MARK_ALLOW;
	}
	if (atomic_read(&linkpower_push_active))
		linkpower_push_packet(skb, state->hook == NF_INET_LOCAL_OUT);
	return NF_ACCEPT;
}

static struct nf_hook_ops linkpower_nf_ops[] __read_mostly = {
	{
		.hook = linkpower_mark_packet,
		.pf = NFPROTO_IPV4,
		.hooknum = NF_INET_LOCAL_IN,
		.priority = NF_IP_PRI_FIRST,
	},
	{
		.hook = linkpower_mark_packet,
		.pf = NFPROTO_IPV4,
		.hooknum = NF_INET_LOCAL_OUT,
		.priority = NF_IP_PRI_FIRST,
	},
#if IS_ENABLED(CONFIG_IPV6)
	{
		.hook = linkpower_mark_packet,
		.pf = NFPROTO_IPV6,
		.hooknum = NF_INET_LOCAL_IN,
		.priority = NF_IP6_PRI_FIRST,
	},
	{
		.hook = linkpower_mark_packet,
		.pf = NFPROTO_IPV6,
		.hooknum = NF_INET_LOCAL_OUT,
		.priority = NF_IP6_PRI_FIRST,
	},
#endif
};

static int linkpower_genl_handler(struct sk_buff *skb,
				  struct genl_info *info)
{
	struct linkpower_pair close_request;
	struct genlmsghdr *genlhdr;
	struct nlattr *attribute;
	struct nlmsghdr *nlhdr;
	kuid_t sender;
	uid_t sender_uid;
	size_t length;
	u16 type;

	if (!skb || !info)
		return -EINVAL;
	nlhdr = nlmsg_hdr(skb);
	genlhdr = nlmsg_data(nlhdr);
	if (genlmsg_len(genlhdr) < NLA_HDRLEN)
		return -EINVAL;
	attribute = genlmsg_data(genlhdr);
	if (!nla_ok(attribute, genlmsg_len(genlhdr)))
		return -EINVAL;
	type = nla_type(attribute);
	length = nla_len(attribute);

	sender = NETLINK_CREDS(skb)->uid;
	sender_uid = from_kuid_munged(&init_user_ns, sender);
	if (sender_uid != 0 && sender_uid != LINKPOWER_SYSTEM_UID)
		return -EPERM;
	if (!info->snd_portid)
		return -EINVAL;
	atomic_set(&linkpower_daemon_port, info->snd_portid);

	switch (type) {
	case LP_REQUEST_PORT_PID:
		return linkpower_reply_port_pid();
	case LP_REQUEST_CLOSE_PROCESS:
		if (length < sizeof(struct linkpower_pair))
			return -EINVAL;
		memcpy(&close_request, nla_data(attribute),
		       sizeof(close_request));
		if (!close_request.uid || !close_request.pid)
			return -EINVAL;
		return linkpower_queue_close(close_request.uid,
					     close_request.pid);
	case LP_REQUEST_START_PUSH:
		return linkpower_start_push(nla_data(attribute), length);
	case LP_REQUEST_STOP_PUSH:
		return linkpower_stop_push(nla_data(attribute), length);
	case LP_REQUEST_MONITOR_CONNECT:
		return linkpower_set_connect_monitor(nla_data(attribute), length);
	case LP_REQUEST_CONNECT_INFO:
		return linkpower_reply_connect();
	case LP_REQUEST_SET_PID_WHITE:
		return linkpower_set_white(false, nla_data(attribute), length);
	case LP_REQUEST_DELETE_PID_WHITE:
		linkpower_delete_white(false);
		return 0;
	case LP_REQUEST_SET_DUAL_WHITE:
		return linkpower_set_white(true, nla_data(attribute), length);
	case LP_REQUEST_DELETE_DUAL_WHITE:
		linkpower_delete_white(true);
		return 0;
	case LP_REQUEST_QRTR_WAKEUP:
		return linkpower_reply_qrtr();
	case LP_REQUEST_IRQ_DATA_WAKEUP:
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
}

static int __init linkpower_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct linkpower_port_pid) != 4);
	BUILD_BUG_ON(sizeof(struct linkpower_close_response) != 12);
	BUILD_BUG_ON(sizeof(struct linkpower_connect_info) != 16);
	BUILD_BUG_ON(sizeof(struct linkpower_push_config) != 32);
	BUILD_BUG_ON(sizeof(struct linkpower_push_report) != 16);
	BUILD_BUG_ON(sizeof(struct linkpower_qrtr_info) != 8);

	linkpower_close_wq = alloc_ordered_workqueue("oplus_linkpower_close",
						      WQ_MEM_RECLAIM);
	if (!linkpower_close_wq)
		return -ENOMEM;

	ret = genl_register_family(&linkpower_genl_family);
	if (ret)
		goto destroy_workqueue;
	ret = nf_register_net_hooks(&init_net, linkpower_nf_ops,
				    ARRAY_SIZE(linkpower_nf_ops));
	if (ret)
		goto unregister_genl;
	ret = register_trace_tcp_retransmit_skb(linkpower_trace_retransmit,
						NULL);
	if (ret)
		goto unregister_nf;
	ret = register_trace_tcp_send_reset(linkpower_trace_send_reset, NULL);
	if (ret)
		goto unregister_retransmit;
	ret = register_trace_tcp_receive_reset(linkpower_trace_receive_reset,
						NULL);
	if (ret)
		goto unregister_send_reset;
	ret = register_trace_tcp_destroy_sock(linkpower_trace_destroy, NULL);
	if (ret)
		goto unregister_receive_reset;

	pr_info("registered family id=%u\n", linkpower_genl_family.id);
	return 0;

unregister_receive_reset:
	unregister_trace_tcp_receive_reset(linkpower_trace_receive_reset, NULL);
unregister_send_reset:
	unregister_trace_tcp_send_reset(linkpower_trace_send_reset, NULL);
unregister_retransmit:
	unregister_trace_tcp_retransmit_skb(linkpower_trace_retransmit, NULL);
unregister_nf:
	nf_unregister_net_hooks(&init_net, linkpower_nf_ops,
				ARRAY_SIZE(linkpower_nf_ops));
unregister_genl:
	genl_unregister_family(&linkpower_genl_family);
destroy_workqueue:
	destroy_workqueue(linkpower_close_wq);
	linkpower_close_wq = NULL;
	return ret;
}

static void __exit linkpower_exit(void)
{
	genl_unregister_family(&linkpower_genl_family);
	flush_workqueue(linkpower_close_wq);
	unregister_trace_tcp_destroy_sock(linkpower_trace_destroy, NULL);
	unregister_trace_tcp_receive_reset(linkpower_trace_receive_reset, NULL);
	unregister_trace_tcp_send_reset(linkpower_trace_send_reset, NULL);
	unregister_trace_tcp_retransmit_skb(linkpower_trace_retransmit, NULL);
	nf_unregister_net_hooks(&init_net, linkpower_nf_ops,
				ARRAY_SIZE(linkpower_nf_ops));
	destroy_workqueue(linkpower_close_wq);
	linkpower_close_wq = NULL;
}

module_init(linkpower_init);
module_exit(linkpower_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("OPlus LinkPower compatibility backend");
