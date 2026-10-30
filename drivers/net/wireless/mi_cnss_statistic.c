// SPDX-License-Identifier: GPL-2.0-only
/* Xiaomi Venus stock Wi-Fi wakeup-statistics generic-netlink ABI. */
#include <linux/init.h>
#include <linux/mi_cnss_statistic.h>
#include <linux/module.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>

enum {
	CNSS_STATISTIC_UNSPEC,
	CNSS_STATISTIC_EVENT,
	CNSS_STATISTIC_DEBUG,
	CNSS_STATISTIC_ENABLE,
	CNSS_STATISTIC_DISABLE,
};

enum {
	CNSS_STATISTIC_ATTR_UNSPEC,
	CNSS_STATISTIC_ATTR_SUBTYPE,
	CNSS_STATISTIC_ATTR_SOURCE_PORT,
	CNSS_STATISTIC_ATTR_DEST_PORT,
	__CNSS_STATISTIC_ATTR_MAX,
};

bool wow_suspend_type;
EXPORT_SYMBOL(wow_suspend_type);

static bool wow_statistic_enable;

static const struct nla_policy cnss_statistic_policy[] = {
	[CNSS_STATISTIC_ATTR_SUBTYPE] = { .type = NLA_U16 },
	[CNSS_STATISTIC_ATTR_SOURCE_PORT] = { .type = NLA_U16 },
	[CNSS_STATISTIC_ATTR_DEST_PORT] = { .type = NLA_U16 },
};

static int cnss_statistic_genl_debug(struct sk_buff *skb, struct genl_info *info)
{
	return 0;
}

static int cnss_statistic_genl_enable(struct sk_buff *skb, struct genl_info *info)
{
	WRITE_ONCE(wow_statistic_enable, true);
	return 0;
}

static int cnss_statistic_genl_disable(struct sk_buff *skb, struct genl_info *info)
{
	WRITE_ONCE(wow_statistic_enable, false);
	return 0;
}

static const struct genl_ops cnss_statistic_genl_ops[] = {
	{ .cmd = CNSS_STATISTIC_DEBUG, .doit = cnss_statistic_genl_debug },
	{ .cmd = CNSS_STATISTIC_ENABLE, .doit = cnss_statistic_genl_enable },
	{ .cmd = CNSS_STATISTIC_DISABLE, .doit = cnss_statistic_genl_disable },
};

static const struct genl_multicast_group cnss_statistic_mcgrps[] = {
	{ .name = "events" },
};

static struct genl_family cnss_statistic_genl_family = {
	.name = "cnss_statistic",
	.version = 1,
	.maxattr = __CNSS_STATISTIC_ATTR_MAX - 1,
	.policy = cnss_statistic_policy,
	.ops = cnss_statistic_genl_ops,
	.n_ops = ARRAY_SIZE(cnss_statistic_genl_ops),
	.mcgrps = cnss_statistic_mcgrps,
	.n_mcgrps = ARRAY_SIZE(cnss_statistic_mcgrps),
	.module = THIS_MODULE,
};

int cnss_statistic_wow_wakeup(u16 subtype, u16 source_port, u16 dest_port)
{
	struct sk_buff *skb;
	void *header;

	if (READ_ONCE(wow_suspend_type) || !READ_ONCE(wow_statistic_enable))
		return 0;
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	header = genlmsg_put(skb, 0, 0, &cnss_statistic_genl_family, 0,
			     CNSS_STATISTIC_EVENT);
	if (!header)
		goto free_skb;
	if (nla_put_u16(skb, CNSS_STATISTIC_ATTR_SUBTYPE, subtype) ||
	    nla_put_u16(skb, CNSS_STATISTIC_ATTR_SOURCE_PORT, source_port) ||
	    nla_put_u16(skb, CNSS_STATISTIC_ATTR_DEST_PORT, dest_port)) {
		genlmsg_cancel(skb, header);
		goto free_skb;
	}
	genlmsg_end(skb, header);
	return genlmsg_multicast_netns(&cnss_statistic_genl_family, &init_net,
				      skb, 0, 0, GFP_ATOMIC);

free_skb:
	nlmsg_free(skb);
	return -EMSGSIZE;
}
EXPORT_SYMBOL(cnss_statistic_wow_wakeup);

static int __init cnss_statistic_init(void)
{
	return genl_register_family(&cnss_statistic_genl_family);
}
module_init(cnss_statistic_init);

static void __exit cnss_statistic_exit(void)
{
	genl_unregister_family(&cnss_statistic_genl_family);
}
module_exit(cnss_statistic_exit);

MODULE_DESCRIPTION("Xiaomi Wi-Fi wakeup statistics");
MODULE_LICENSE("GPL v2");
