// SPDX-License-Identifier: GPL-2.0-only
/*
 * New Xiaomi count-info semantics verified against the stock SM8850 display
 * module (Pandora OS3.0.7 and Popsicle OS3.0.315). This is a Venus implementation,
 * not a copy of their panel-specific command sequences or structure layouts.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include "mi_panel_stats.h"

static u64 stats_add(u64 a, u64 b)
{
	return b > U64_MAX - a ? U64_MAX : a + b;
}

void mi_panel_stats_init(struct mi_panel_stats *s)
{
	memset(s, 0, sizeof(*s));
	spin_lock_init(&s->lock);
}

int mi_panel_stats_set(struct mi_panel_stats *s, u32 type, s32 value,
		const char *payload, u32 len, bool panel_off)
{
	char version[MI_BUILD_VERSION_SIZE + 1] = {0};
	unsigned long flags;
	u64 now;
	u32 i;

	/* Validate before locking or changing any state. */
	if (type == 1) {
		if (!payload || !len || len > MI_BUILD_VERSION_SIZE)
			return -EINVAL;
		memcpy(version, payload, len);
		for (i = 0; i < len && version[i]; i++)
			if (version[i] < ' ' || version[i] > '~')
				return -EINVAL;
		if (!i)
			return -EINVAL;
	} else if (len || (type == 0 && (value < 0 || value > 3)) ||
			((type == 2 || type == 4) && value < 0)) {
		return -EINVAL;
	} else if (type != 0 && type != 2 && type != 4) {
		return -EOPNOTSUPP;
	}

	spin_lock_irqsave(&s->lock, flags);
	now = ktime_get_ns();
	switch (type) {
	case 0:
		/* 0=OFF, 1=ON, 2=DOZE, 3=DOZE_SUSPEND in the Xiaomi ABI. */
		if (value == 1 && panel_off && !s->power_pending) {
			s->power_start_ns = now;
			s->power_pending = true;
		} else if (value != 1) {
			s->power_pending = false;
		}
		break;
	case 1:
		if (strcmp(s->build_version, version)) {
			/* These diagnostics belong to a software build, not panel life. */
			s->power_last_ns = s->power_total_ns = s->power_samples = 0;
			s->commit_long = 0;
			s->tpidle_count = 0;
			s->aod_total_ns = s->aod_longest_ns = 0;
			s->aod_sessions = s->aod_active ? 1 : 0;
			s->aod_start_ns = now;
			memcpy(s->build_version, version, sizeof(version));
		}
		break;
	case 2:
		s->commit_long = stats_add(s->commit_long, (u32)value);
		break;
	case 4:
		/* SF increments its own counter before sending: this is a snapshot. */
		s->tpidle_count = value;
		break;
	}
	spin_unlock_irqrestore(&s->lock, flags);
	return 0;
}

void mi_panel_stats_backlight(struct mi_panel_stats *s, u32 level)
{
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	if (level && s->power_pending) {
		s->power_last_ns = ktime_get_ns() - s->power_start_ns;
		s->power_total_ns = stats_add(s->power_total_ns, s->power_last_ns);
		s->power_samples = stats_add(s->power_samples, 1);
		s->power_pending = false;
	}
	spin_unlock_irqrestore(&s->lock, flags);
}

static void stats_aod_update(struct mi_panel_stats *s)
{
	bool active = s->fullscreen && s->low_power;
	u64 elapsed;

	if (active == s->aod_active)
		return;
	if (active) {
		s->aod_start_ns = ktime_get_ns();
		s->aod_sessions = stats_add(s->aod_sessions, 1);
	} else {
		elapsed = ktime_get_ns() - s->aod_start_ns;
		s->aod_total_ns = stats_add(s->aod_total_ns, elapsed);
		s->aod_longest_ns = max(s->aod_longest_ns, elapsed);
	}
	s->aod_active = active;
}

void mi_panel_stats_power(struct mi_panel_stats *s, bool low_power, bool off)
{
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	s->low_power = low_power && !off;
	if (off)
		s->power_pending = false;
	stats_aod_update(s);
	spin_unlock_irqrestore(&s->lock, flags);
}

void mi_panel_stats_fullscreen(struct mi_panel_stats *s, bool enable)
{
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	s->fullscreen = enable;
	stats_aod_update(s);
	spin_unlock_irqrestore(&s->lock, flags);
}

ssize_t mi_panel_stats_show(struct mi_panel_stats *s, char *buf)
{
	unsigned long flags;
	u64 active, longest, elapsed;
	ssize_t len;

	spin_lock_irqsave(&s->lock, flags);
	active = s->aod_total_ns;
	longest = s->aod_longest_ns;
	if (s->aod_active) {
		elapsed = ktime_get_ns() - s->aod_start_ns;
		active = stats_add(active, elapsed);
		longest = max(longest, elapsed);
	}
	len = scnprintf(buf, PAGE_SIZE,
		"system_build_version=%s\npoweron_samples=%llu\n"
		"poweron_last_ns=%llu\npoweron_total_ns=%llu\ncommit_long=%llu\n"
		"tpidle_count=%u\n"
		"fullscreen_aod_times=%llu\nfullscreen_aod_active_ns=%llu\n"
		"fullscreen_aod_longest_ns=%llu\n",
		s->build_version, s->power_samples, s->power_last_ns,
		s->power_total_ns, s->commit_long, s->tpidle_count,
		s->aod_sessions, active, longest);
	spin_unlock_irqrestore(&s->lock, flags);
	return len;
}
