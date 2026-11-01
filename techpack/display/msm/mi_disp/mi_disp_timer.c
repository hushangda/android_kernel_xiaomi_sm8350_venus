// SPDX-License-Identifier: GPL-2.0-only
/* Stock count type 3 enables a real commit-stage timer, never a panel command. */
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>
#include <drm/drm_atomic.h>
#include <drm/drm_modes.h>
#include "mi_disp_timer.h"

struct mi_commit_timer {
	struct timer_list timer;
	spinlock_t lock;
	u64 started, stage_started, stage_ns[MI_COMMIT_STAGES];
	u64 frame_budget_ns;
	u32 crtc_mask;
	enum mi_commit_stage stage;
	bool modeset;
};

static atomic_t mi_timer_enabled = ATOMIC_INIT(0);
/* The ROM's *_timeout_threshold_ms properties both default to 500. */
static atomic_t mi_unblank_timeout_ms = ATOMIC_INIT(500);
static atomic_t mi_blank_timeout_ms = ATOMIC_INIT(500);
static const char * const stage_name[] = {
	"fences", "prepare", "disable", "planes", "enable", "wait_commit",
	"cleanup", "complete",
};

void mi_disp_timer_enable(bool enable)
{
	atomic_set(&mi_timer_enabled, enable);
}

int mi_disp_timer_set_power_timeout(u32 type, s32 milliseconds)
{
	if ((type != 9 && type != 10) || milliseconds < 1 || milliseconds > 60000)
		return -EINVAL;
	atomic_set(type == 9 ? &mi_unblank_timeout_ms : &mi_blank_timeout_ms,
		milliseconds);
	return 0;
}

static void mi_disp_timer_expired(struct timer_list *timer)
{
	struct mi_commit_timer *r = from_timer(r, timer, timer);
	unsigned long flags;
	u64 now;

	if (!atomic_read(&mi_timer_enabled))
		return;
	spin_lock_irqsave(&r->lock, flags);
	now = ktime_get_ns();
	pr_warn_ratelimited("mi_disp_timer: crtc_mask=%x commit stalled in %s total=%lluns stage=%lluns\n",
		r->crtc_mask, stage_name[r->stage], now - r->started,
		now - r->stage_started);
	spin_unlock_irqrestore(&r->lock, flags);
}

struct mi_commit_timer *mi_disp_timer_begin(struct drm_atomic_state *state)
{
	struct mi_commit_timer *r;
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	u32 fps = 0;
	u32 power_timeout_ms = 0;
	unsigned long timeout;
	int i;

	if (!atomic_read(&mi_timer_enabled))
		return NULL;
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return NULL;
	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		r->crtc_mask |= drm_crtc_mask(crtc);
		r->modeset |= drm_atomic_crtc_needs_modeset(crtc_state);
		if (crtc_state->active_changed)
			power_timeout_ms = max_t(u32, power_timeout_ms,
				atomic_read(crtc_state->active ? &mi_unblank_timeout_ms :
					&mi_blank_timeout_ms));
		if (crtc_state->active)
			fps = max_t(u32, fps, drm_mode_vrefresh(&crtc_state->adjusted_mode));
	}
	/* Stock threshold: 1.4 frame periods at <=60 Hz, 1.6 above 60 Hz. */
	if (fps)
		r->frame_budget_ns = (fps > 60 ? 1600000000ULL : 1400000000ULL) / fps;
	timeout = r->frame_budget_ns ?
		msecs_to_jiffies((r->frame_budget_ns / 1000000) * 3) :
		msecs_to_jiffies(100);
	if (power_timeout_ms)
		timeout = msecs_to_jiffies(power_timeout_ms);
	spin_lock_init(&r->lock);
	r->started = r->stage_started = ktime_get_ns();
	timer_setup(&r->timer, mi_disp_timer_expired, 0);
	mod_timer(&r->timer, jiffies + max_t(unsigned long, timeout, 1));
	return r;
}

void mi_disp_timer_stage(struct mi_commit_timer *r, enum mi_commit_stage stage)
{
	unsigned long flags;
	u64 now;

	if (!r || stage >= MI_COMMIT_STAGES)
		return;
	spin_lock_irqsave(&r->lock, flags);
	now = ktime_get_ns();
	r->stage_ns[r->stage] += now - r->stage_started;
	r->stage_started = now;
	r->stage = stage;
	spin_unlock_irqrestore(&r->lock, flags);
}

void mi_disp_timer_end(struct mi_commit_timer *r)
{
	u64 elapsed;
	int i;

	if (!r)
		return;
	/* The callback only owns r, and cannot outlive the atomic commit. */
	del_timer_sync(&r->timer);
	elapsed = ktime_get_ns() - r->started;
	r->stage_ns[r->stage] += ktime_get_ns() - r->stage_started;
	if (atomic_read(&mi_timer_enabled) && !r->modeset &&
			r->frame_budget_ns && elapsed > r->frame_budget_ns) {
		pr_info_ratelimited("mi_disp_timer: crtc_mask=%x slow commit=%lluns budget=%lluns\n",
			r->crtc_mask, elapsed, r->frame_budget_ns);
		for (i = 0; i < MI_COMMIT_STAGES; i++)
			pr_debug("mi_disp_timer: %s=%lluns\n", stage_name[i], r->stage_ns[i]);
	}
	kfree(r);
}
