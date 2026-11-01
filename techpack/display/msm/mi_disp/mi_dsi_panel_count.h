/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MI_DSI_PANEL_COUNT_H_
#define _MI_DSI_PANEL_COUNT_H_

#include <linux/spinlock.h>
#include <linux/types.h>

/* The order and units are the Venus stock disp_count persistence ABI. */
enum mi_panel_count_field {
	MI_COUNT_ACTIVE,
	MI_COUNT_KICKOFF,
	MI_COUNT_BOOTTIME,
	MI_COUNT_RTC,
	MI_COUNT_DAYS,
	MI_COUNT_BL_DURATION,
	MI_COUNT_BL_INTEGRAL,
	MI_COUNT_BL_HIGH,
	MI_COUNT_BL_LOW,
	MI_COUNT_HBM_DURATION,
	MI_COUNT_HBM_TIMES,
	MI_COUNT_FPS60,
	MI_COUNT_FPS90,
	MI_COUNT_FPS120,
	MI_COUNT_FIELDS,
};

struct mi_panel_count {
	/* Leaf lock: never acquire a display/panel lock while holding it. */
	spinlock_t lock;
	u64 value[MI_COUNT_FIELDS];
	u64 stamp;
	u64 hbm_start;
	u32 fps;
	u32 backlight;
	u32 fps_remainder[3];
	u32 hbm_remainder;
	bool active;
	bool hbm;
};

void mi_panel_count_init(struct mi_panel_count *count);
void mi_panel_count_active(struct mi_panel_count *count, bool active);
void mi_panel_count_disable(struct mi_panel_count *count);
void mi_panel_count_backlight(struct mi_panel_count *count, u32 level);
void mi_panel_count_hbm(struct mi_panel_count *count, bool enabled);
void mi_panel_count_fps(struct mi_panel_count *count, u32 fps);
void mi_panel_count_kickoff(struct mi_panel_count *count);
ssize_t mi_panel_count_show(struct mi_panel_count *count, char *buf);
int mi_panel_count_store(struct mi_panel_count *count, const char *buf,
			size_t size);

#endif
