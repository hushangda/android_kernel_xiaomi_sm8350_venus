/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VENDOR_FREEZER_COMPAT_H
#define _LINUX_VENDOR_FREEZER_COMPAT_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct sk_buff;
struct sock;

enum vendor_freezer_backend {
	VENDOR_FREEZER_AUTO = 0,
	VENDOR_FREEZER_HANS,
	VENDOR_FREEZER_MILLET,
	VENDOR_FREEZER_BACKEND_MAX,
};

struct vendor_freezer_backend_ops {
	enum vendor_freezer_backend backend;
	const char *name;
	bool (*match_handshake)(struct sk_buff *skb);
	void (*receive)(struct sk_buff *skb);
};

#if IS_ENABLED(CONFIG_ANDROID_VENDOR_FREEZER_COMPAT)
int vendor_freezer_register_backend(
		const struct vendor_freezer_backend_ops *ops,
		struct sock **sock);
void vendor_freezer_unregister_backend(enum vendor_freezer_backend backend);
bool vendor_freezer_backend_active(enum vendor_freezer_backend backend);
#else
static inline int vendor_freezer_register_backend(
		const struct vendor_freezer_backend_ops *ops,
		struct sock **sock)
{
	return -EOPNOTSUPP;
}

static inline void vendor_freezer_unregister_backend(
		enum vendor_freezer_backend backend)
{
}

static inline bool vendor_freezer_backend_active(
		enum vendor_freezer_backend backend)
{
	return false;
}
#endif

#endif /* _LINUX_VENDOR_FREEZER_COMPAT_H */
