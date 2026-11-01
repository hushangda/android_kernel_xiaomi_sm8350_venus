// SPDX-License-Identifier: GPL-2.0-only
/*
 * Venus display lifetime counters. The persisted format is the stock Venus
 * 15-field ABI, not the newer Vili format containing six refresh rates.
 *
 * Protocol reference: hypeartist/android-kernel-xiaomi-vili,
 * 7c48fad18f7f230a8f24faf7dadb0d0bec019234 (GPL-2.0-only), mi_dsi_panel_count.c;
 * Venus units/field list verified against the stock msm_drm.ko implementation.
 * This implementation uses per-panel state and a leaf lock rather than the
 * original global display index, timestamps and formatting buffers.
 */
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/timekeeping.h>

#include "mi_dsi_panel_count.h"

#define MI_COUNT_DAY_SECONDS 86400ULL

static const char * const mi_count_keys[] = {
	"panel_active", "panel_kickoff_count", "kernel_boottime",
	"kernel_rtctime", "kernel_days", "bl_duration", "bl_level_integral",
	"bl_highlevel_duration", "bl_lowlevel_duration", "hbm_duration",
	"hbm_times", "fps60_times", "fps90_times", "fps120_times", "record_end",
};

static u64 mi_count_add(u64 a, u64 b)
{
	return b > U64_MAX - a ? U64_MAX : a + b;
}

static u64 mi_count_mul(u64 ticks, u32 level)
{
	if (level && ticks > div_u64(U64_MAX, level))
		return U64_MAX;
	return ticks * level;
}

static void mi_count_seconds(u64 *value, u32 *remainder, u64 ticks)
{
	u32 fraction;
	u64 seconds = div_u64_rem(ticks, HZ, &fraction);

	fraction += *remainder;
	*value = mi_count_add(*value, seconds + fraction / HZ);
	*remainder = fraction % HZ;
}

/* Called with count->lock held. All event timestamps are sampled under it. */
static void mi_count_update(struct mi_panel_count *count, u64 now)
{
	u64 ticks = now - count->stamp;
	u32 index;

	count->stamp = now;
	if (count->active) {
		count->value[MI_COUNT_ACTIVE] =
			mi_count_add(count->value[MI_COUNT_ACTIVE], ticks);
		switch (count->fps) {
		case 60: index = 0; break;
		case 90: index = 1; break;
		case 120: index = 2; break;
		default: index = 3; break;
		}
		if (index < 3)
			mi_count_seconds(&count->value[MI_COUNT_FPS60 + index],
				&count->fps_remainder[index], ticks);
	}
	if (count->backlight) {
		count->value[MI_COUNT_BL_DURATION] =
			mi_count_add(count->value[MI_COUNT_BL_DURATION], ticks);
		count->value[MI_COUNT_BL_INTEGRAL] =
			mi_count_add(count->value[MI_COUNT_BL_INTEGRAL],
				mi_count_mul(ticks, count->backlight));
		index = count->backlight >= 1536 ? MI_COUNT_BL_HIGH : MI_COUNT_BL_LOW;
		count->value[index] = mi_count_add(count->value[index], ticks);
	}
	if (count->hbm)
		mi_count_seconds(&count->value[MI_COUNT_HBM_DURATION],
			&count->hbm_remainder, ticks);
}

static void mi_count_end_hbm(struct mi_panel_count *count, u64 now)
{
	if (count->hbm && now != count->hbm_start)
		count->value[MI_COUNT_HBM_TIMES] =
			mi_count_add(count->value[MI_COUNT_HBM_TIMES], 1);
	count->hbm = false;
	/* Stock HBM duration rounds each completed session down to seconds. */
	count->hbm_remainder = 0;
}

static void mi_count_days(struct mi_panel_count *count, u64 now)
{
	u64 anchor = count->value[MI_COUNT_RTC];
	u64 days;
	u32 remainder;

	/* Do not turn an unset, corrected or implausible RTC into lifetime days. */
	if (!anchor || now < anchor || now - anchor > 10 * 365 * MI_COUNT_DAY_SECONDS) {
		count->value[MI_COUNT_RTC] = now;
		return;
	}
	days = div_u64_rem(now - anchor, MI_COUNT_DAY_SECONDS, &remainder);
	count->value[MI_COUNT_DAYS] = mi_count_add(count->value[MI_COUNT_DAYS], days);
	count->value[MI_COUNT_RTC] = now - remainder;
}

void mi_panel_count_init(struct mi_panel_count *count)
{
	memset(count, 0, sizeof(*count));
	spin_lock_init(&count->lock);
	count->stamp = get_jiffies_64();
}

void mi_panel_count_active(struct mi_panel_count *count, bool active)
{
	unsigned long flags;

	spin_lock_irqsave(&count->lock, flags);
	mi_count_update(count, get_jiffies_64());
	count->active = active;
	spin_unlock_irqrestore(&count->lock, flags);
}

void mi_panel_count_disable(struct mi_panel_count *count)
{
	unsigned long flags;
	u64 now;

	spin_lock_irqsave(&count->lock, flags);
	now = get_jiffies_64();
	mi_count_update(count, now);
	mi_count_end_hbm(count, now);
	count->active = false;
	count->backlight = 0;
	spin_unlock_irqrestore(&count->lock, flags);
}

void mi_panel_count_backlight(struct mi_panel_count *count, u32 level)
{
	unsigned long flags;

	spin_lock_irqsave(&count->lock, flags);
	mi_count_update(count, get_jiffies_64());
	count->backlight = level;
	spin_unlock_irqrestore(&count->lock, flags);
}

void mi_panel_count_hbm(struct mi_panel_count *count, bool enabled)
{
	unsigned long flags;
	u64 now;

	spin_lock_irqsave(&count->lock, flags);
	now = get_jiffies_64();
	mi_count_update(count, now);
	if (enabled && !count->hbm) {
		count->hbm = true;
		count->hbm_start = now;
	} else if (!enabled) {
		mi_count_end_hbm(count, now);
	}
	spin_unlock_irqrestore(&count->lock, flags);
}

void mi_panel_count_fps(struct mi_panel_count *count, u32 fps)
{
	unsigned long flags;

	spin_lock_irqsave(&count->lock, flags);
	mi_count_update(count, get_jiffies_64());
	count->fps = fps;
	spin_unlock_irqrestore(&count->lock, flags);
}

void mi_panel_count_kickoff(struct mi_panel_count *count)
{
	unsigned long flags;

	spin_lock_irqsave(&count->lock, flags);
	count->value[MI_COUNT_KICKOFF] =
		mi_count_add(count->value[MI_COUNT_KICKOFF], 1);
	spin_unlock_irqrestore(&count->lock, flags);
}

ssize_t mi_panel_count_show(struct mi_panel_count *count, char *buf)
{
	u64 value[MI_COUNT_FIELDS + 1];
	unsigned long flags;
	ssize_t len = 0;
	int i;

	spin_lock_irqsave(&count->lock, flags);
	mi_count_update(count, get_jiffies_64());
	mi_count_days(count, ktime_get_real_seconds());
	memcpy(value, count->value, sizeof(count->value));
	value[MI_COUNT_BOOTTIME] = mi_count_add(value[MI_COUNT_BOOTTIME],
		ktime_get_boottime_seconds());
	spin_unlock_irqrestore(&count->lock, flags);
	value[MI_COUNT_FIELDS] = 0;

	for (i = 0; i < ARRAY_SIZE(mi_count_keys); i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s=%llu\n",
			mi_count_keys[i], value[i]);
	return len;
}

/* Parse bounded bytes, not an sscanf prefix: bad records never modify state. */
static int mi_count_parse(const char *buf, size_t size, u64 *value)
{
	size_t pos = 0, keylen;
	u64 number;
	unsigned int digit;
	int i;

	if (!size || size >= PAGE_SIZE)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(mi_count_keys); i++) {
		keylen = strlen(mi_count_keys[i]);
		if (size - pos <= keylen ||
		    memcmp(buf + pos, mi_count_keys[i], keylen) ||
		    buf[pos + keylen] != '=')
			return -EINVAL;
		pos += keylen + 1;
		if (pos == size || buf[pos] < '0' || buf[pos] > '9')
			return -EINVAL;
		number = 0;
		while (pos < size && buf[pos] >= '0' && buf[pos] <= '9') {
			digit = buf[pos++] - '0';
			if (number > div_u64(U64_MAX - digit, 10))
				return -ERANGE;
			number = number * 10 + digit;
		}
		value[i] = number;
		if (pos < size && buf[pos] == '\n')
			pos++;
		else if (i != MI_COUNT_FIELDS || pos != size)
			return -EINVAL;
	}
	return pos == size && !value[MI_COUNT_FIELDS] ? 0 : -EINVAL;
}

int mi_panel_count_store(struct mi_panel_count *count, const char *buf, size_t size)
{
	u64 value[MI_COUNT_FIELDS + 1];
	unsigned long flags;
	int ret = mi_count_parse(buf, size, value);

	if (ret)
		return ret;
	spin_lock_irqsave(&count->lock, flags);
	memcpy(count->value, value, sizeof(count->value));
	/* Restore is a replacement of persisted totals; keep live panel state. */
	count->stamp = get_jiffies_64();
	count->hbm_start = count->stamp;
	count->hbm_remainder = 0;
	memset(count->fps_remainder, 0, sizeof(count->fps_remainder));
	mi_count_days(count, ktime_get_real_seconds());
	spin_unlock_irqrestore(&count->lock, flags);
	return 0;
}
