// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shared raw-netlink protocol 29 router for Android vendor freezers.
 *
 * OPlus HANS and Xiaomi MILLET both use protocol 29.  Only one kernel
 * socket can own a protocol number in a network namespace, so the first
 * valid userspace handshake selects a backend for the rest of the boot.
 */

#define pr_fmt(fmt) "vendor_freezer: " fmt

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/vendor_freezer_compat.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#define NETLINK_VENDOR_FREEZER	29

static DEFINE_MUTEX(vendor_freezer_lock);
static const struct vendor_freezer_backend_ops
	*vendor_freezer_ops[VENDOR_FREEZER_BACKEND_MAX];
static struct sock **vendor_freezer_sock_refs[VENDOR_FREEZER_BACKEND_MAX];
static struct sock *vendor_freezer_sock;
static struct kobject *vendor_freezer_kobj;
static atomic_t vendor_freezer_active = ATOMIC_INIT(VENDOR_FREEZER_AUTO);
static enum vendor_freezer_backend vendor_freezer_forced =
	VENDOR_FREEZER_AUTO;

static const char *vendor_freezer_backend_name(enum vendor_freezer_backend id)
{
	const struct vendor_freezer_backend_ops *ops;

	if (id == VENDOR_FREEZER_AUTO)
		return "auto";
	if (id <= VENDOR_FREEZER_AUTO || id >= VENDOR_FREEZER_BACKEND_MAX)
		return "invalid";

	ops = READ_ONCE(vendor_freezer_ops[id]);
	return ops && ops->name ? ops->name :
		(id == VENDOR_FREEZER_HANS ? "hans" : "millet");
}

static int __init vendor_freezer_setup(char *str)
{
	if (!str || !strcmp(str, "auto"))
		vendor_freezer_forced = VENDOR_FREEZER_AUTO;
	else if (!strcmp(str, "hans"))
		vendor_freezer_forced = VENDOR_FREEZER_HANS;
	else if (!strcmp(str, "millet"))
		vendor_freezer_forced = VENDOR_FREEZER_MILLET;
	else
		return 0;

	if (vendor_freezer_forced != VENDOR_FREEZER_AUTO)
		atomic_set(&vendor_freezer_active, vendor_freezer_forced);
	return 1;
}
__setup("vendor_freezer=", vendor_freezer_setup);

static ssize_t backend_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	enum vendor_freezer_backend active =
		atomic_read(&vendor_freezer_active);

	return scnprintf(buf, PAGE_SIZE, "%s%s\n",
			 vendor_freezer_backend_name(active),
			 vendor_freezer_forced == VENDOR_FREEZER_AUTO ? "" :
			 " (forced)");
}

static struct kobj_attribute backend_attr = __ATTR_RO(backend);

static void vendor_freezer_receive(struct sk_buff *skb)
{
	const struct vendor_freezer_backend_ops *ops = NULL;
	enum vendor_freezer_backend active;
	int id;

	active = atomic_read(&vendor_freezer_active);
	if (active == VENDOR_FREEZER_AUTO) {
		for (id = VENDOR_FREEZER_HANS;
		     id < VENDOR_FREEZER_BACKEND_MAX; id++) {
			ops = READ_ONCE(vendor_freezer_ops[id]);
			if (!ops || !ops->match_handshake ||
			    !ops->match_handshake(skb))
				continue;

			if (atomic_cmpxchg(&vendor_freezer_active,
					   VENDOR_FREEZER_AUTO, id) ==
					   VENDOR_FREEZER_AUTO)
				pr_info("selected %s backend\n", ops->name);
			break;
		}
		active = atomic_read(&vendor_freezer_active);
		if (active == VENDOR_FREEZER_AUTO)
			return;
	}

	if (active <= VENDOR_FREEZER_AUTO ||
	    active >= VENDOR_FREEZER_BACKEND_MAX)
		return;

	ops = READ_ONCE(vendor_freezer_ops[active]);
	if (ops && ops->receive)
		ops->receive(skb);
}

bool vendor_freezer_backend_active(enum vendor_freezer_backend backend)
{
	if (backend <= VENDOR_FREEZER_AUTO ||
	    backend >= VENDOR_FREEZER_BACKEND_MAX)
		return false;

	return atomic_read(&vendor_freezer_active) == backend &&
	       READ_ONCE(vendor_freezer_ops[backend]);
}
EXPORT_SYMBOL_GPL(vendor_freezer_backend_active);

int vendor_freezer_register_backend(
		const struct vendor_freezer_backend_ops *ops,
		struct sock **sock)
{
	struct netlink_kernel_cfg cfg = {
		.input = vendor_freezer_receive,
	};
	int ret = 0;

	if (!ops || !sock || !ops->name || !ops->match_handshake ||
	    !ops->receive || ops->backend <= VENDOR_FREEZER_AUTO ||
	    ops->backend >= VENDOR_FREEZER_BACKEND_MAX)
		return -EINVAL;

	mutex_lock(&vendor_freezer_lock);
	if (vendor_freezer_ops[ops->backend]) {
		ret = -EEXIST;
		goto out;
	}

	vendor_freezer_ops[ops->backend] = ops;
	vendor_freezer_sock_refs[ops->backend] = sock;
	if (!vendor_freezer_sock) {
		vendor_freezer_sock = netlink_kernel_create(&init_net,
				NETLINK_VENDOR_FREEZER, &cfg);
		if (!vendor_freezer_sock) {
			vendor_freezer_ops[ops->backend] = NULL;
			vendor_freezer_sock_refs[ops->backend] = NULL;
			ret = -ENOMEM;
			goto out;
		}

		vendor_freezer_kobj = kobject_create_and_add("vendor_freezer",
							    kernel_kobj);
		if (vendor_freezer_kobj &&
		    sysfs_create_file(vendor_freezer_kobj, &backend_attr.attr)) {
			kobject_put(vendor_freezer_kobj);
			vendor_freezer_kobj = NULL;
		}
		pr_info("raw-netlink protocol %d registered\n",
			NETLINK_VENDOR_FREEZER);
	}

	*sock = vendor_freezer_sock;
out:
	mutex_unlock(&vendor_freezer_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(vendor_freezer_register_backend);

void vendor_freezer_unregister_backend(enum vendor_freezer_backend backend)
{
	int id;
	bool any = false;

	if (backend <= VENDOR_FREEZER_AUTO ||
	    backend >= VENDOR_FREEZER_BACKEND_MAX)
		return;

	mutex_lock(&vendor_freezer_lock);
	if (vendor_freezer_sock_refs[backend])
		*vendor_freezer_sock_refs[backend] = NULL;
	vendor_freezer_sock_refs[backend] = NULL;
	vendor_freezer_ops[backend] = NULL;
	if (atomic_read(&vendor_freezer_active) == backend)
		atomic_set(&vendor_freezer_active, vendor_freezer_forced);

	for (id = VENDOR_FREEZER_HANS;
	     id < VENDOR_FREEZER_BACKEND_MAX; id++)
		any |= !!vendor_freezer_ops[id];

	if (!any && vendor_freezer_sock) {
		netlink_kernel_release(vendor_freezer_sock);
		vendor_freezer_sock = NULL;
		if (vendor_freezer_kobj) {
			sysfs_remove_file(vendor_freezer_kobj,
					  &backend_attr.attr);
			kobject_put(vendor_freezer_kobj);
			vendor_freezer_kobj = NULL;
		}
	}
	mutex_unlock(&vendor_freezer_lock);
}
EXPORT_SYMBOL_GPL(vendor_freezer_unregister_backend);

MODULE_DESCRIPTION("Android vendor freezer protocol 29 multiplexer");
MODULE_LICENSE("GPL v2");
