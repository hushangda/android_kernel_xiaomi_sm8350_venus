/*
 * netlink interface
 *
 * Copyright (c) 2017 Goodix
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/types.h>
#include <net/sock.h>
#include <net/netlink.h>

#define NETLINK_TEST 25
#define MAX_MSGSIZE 32
void sendnlmsg(char *message);
static u32 pid;
struct sock *nl_sk;

void sendnlmsg(char *message)
{
	struct sk_buff *skb_1;
	struct nlmsghdr *nlh;
	int len = NLMSG_SPACE(MAX_MSGSIZE);
	int ret = 0;
	u32 portid = READ_ONCE(pid);

	if (!message || !nl_sk || !portid) {
		return;
	}

	skb_1 = alloc_skb(len, GFP_KERNEL);

	if (!skb_1) {
		pr_err("alloc_skb error\n");
		return;
	}

	nlh = nlmsg_put(skb_1, 0, 0, 0, MAX_MSGSIZE, 0);
	if (!nlh) {
		kfree_skb(skb_1);
		return;
	}
	NETLINK_CB(skb_1).portid = 0;
	NETLINK_CB(skb_1).dst_group = 0;
	/* Stock HAL messages contain one event byte in a 32-byte payload. */
	memset(NLMSG_DATA(nlh), 0, MAX_MSGSIZE);
	*(u8 *)NLMSG_DATA(nlh) = *(u8 *)message;
	ret = netlink_unicast(nl_sk, skb_1, portid, MSG_DONTWAIT);

	if (ret < 0) {
		/*kfree_skb(skb_1); */
		pr_err("send msg from kernel to usespace failed ret 0x%x\n",
		       ret);
	}
}

void nl_data_ready(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;

	if (skb->len < NLMSG_HDRLEN)
		return;
	nlh = nlmsg_hdr(skb);
	if (!nlmsg_ok(nlh, skb->len))
		return;
	WRITE_ONCE(pid, NETLINK_CB(skb).portid);
}

int netlink_init(void)
{
	struct netlink_kernel_cfg netlink_cfg;
	memset(&netlink_cfg, 0, sizeof(struct netlink_kernel_cfg));
	netlink_cfg.groups = 0;
	netlink_cfg.flags = 0;
	netlink_cfg.input = nl_data_ready;
	netlink_cfg.cb_mutex = NULL;
	nl_sk = netlink_kernel_create(&init_net, NETLINK_TEST, &netlink_cfg);

	if (!nl_sk) {
		pr_err("create netlink socket error\n");
		return 1;
	}

	return 0;
}

void netlink_exit(void)
{
	if (nl_sk != NULL) {
		WRITE_ONCE(pid, 0);
		netlink_kernel_release(nl_sk);
		nl_sk = NULL;
	}

	pr_info("self module exited\n");
}
