/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MI_CNSS_STATISTIC_H
#define _LINUX_MI_CNSS_STATISTIC_H

#include <linux/compiler.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_REACHABLE(CONFIG_MI_CNSS_STATISTIC)
extern bool wow_suspend_type;
int cnss_statistic_wow_wakeup(u16 subtype, u16 source_port, u16 dest_port);

static inline void cnss_statistic_set_suspend_type(bool runtime_suspend)
{
	WRITE_ONCE(wow_suspend_type, runtime_suspend);
}
#else
static inline int cnss_statistic_wow_wakeup(u16 subtype, u16 source_port,
					  u16 dest_port)
{
	return 0;
}

static inline void cnss_statistic_set_suspend_type(bool runtime_suspend) { }
#endif

#endif
