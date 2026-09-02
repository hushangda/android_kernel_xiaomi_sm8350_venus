/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_OPLUS_LINKPOWER_H
#define _LINUX_OPLUS_LINKPOWER_H

#include <linux/kconfig.h>
#include <linux/types.h>

struct sock;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_LINKPOWER)
void oplus_linkpower_socket_created(struct sock *sk);
void oplus_linkpower_tcp_connected(struct sock *sk);
void oplus_linkpower_qrtr_packet(u16 service_id, u16 message_id);
void oplus_linkpower_irq_wakeup(unsigned int irq);
#else
static inline void oplus_linkpower_socket_created(struct sock *sk) { }
static inline void oplus_linkpower_tcp_connected(struct sock *sk) { }
static inline void oplus_linkpower_qrtr_packet(u16 service_id, u16 message_id) { }
static inline void oplus_linkpower_irq_wakeup(unsigned int irq) { }
#endif

#endif /* _LINUX_OPLUS_LINKPOWER_H */
