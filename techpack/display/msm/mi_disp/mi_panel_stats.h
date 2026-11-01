/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MI_PANEL_STATS_H_
#define _MI_PANEL_STATS_H_

#include <linux/spinlock.h>
#include <linux/types.h>

#define MI_BUILD_VERSION_SIZE 100

/* Software-generation diagnostics, separate from the Venus persistence ABI. */
struct mi_panel_stats {
	spinlock_t lock; /* leaf lock; no display locks or allocation under it */
	char build_version[MI_BUILD_VERSION_SIZE + 1];
	u64 power_start_ns, power_last_ns, power_total_ns, power_samples;
	u64 commit_long;
	u32 tpidle_count;
	u64 aod_start_ns, aod_total_ns, aod_longest_ns, aod_sessions;
	bool power_pending, fullscreen, low_power, aod_active;
};

void mi_panel_stats_init(struct mi_panel_stats *stats);
int mi_panel_stats_set(struct mi_panel_stats *stats, u32 type, s32 value,
		const char *payload, u32 len, bool panel_off);
void mi_panel_stats_backlight(struct mi_panel_stats *stats, u32 level);
void mi_panel_stats_power(struct mi_panel_stats *stats, bool low_power,
		bool off);
void mi_panel_stats_fullscreen(struct mi_panel_stats *stats, bool enable);
ssize_t mi_panel_stats_show(struct mi_panel_stats *stats, char *buf);

#endif
