/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SUSFS_SELINUX_H
#define _LINUX_SUSFS_SELINUX_H

#include <linux/types.h>

#define SUSFS_BAKASU_SELINUX_BRIDGE 1

struct av_decision;
extern bool ksu_selinux_hide_running;
int ksu_susfs_context_to_sid(const char *context, u32 len, u32 *sid,
                           u32 *live_sid, int *live_error);
int ksu_susfs_sid_to_context(u32 sid, char **context, u32 *len);
void ksu_susfs_compute_av(u32 ssid, u32 tsid, u16 tclass,
                         struct av_decision *avd);

#endif
