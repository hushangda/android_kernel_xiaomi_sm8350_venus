/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MI_EARLY_DIMMING_H_
#define _MI_EARLY_DIMMING_H_

#include <linux/types.h>

/* Protected by panel_lock, including command completion and backlight hooks. */
struct mi_early_dimming {
	bool layer_has_aod;
	bool pending;
	bool off_sent;
};

static inline void mi_early_dimming_request(struct mi_early_dimming *s, bool on)
{
	if (on)
		s->pending = true;
}

static inline bool mi_early_dimming_needs_off(struct mi_early_dimming *s,
		bool ready, bool fod)
{
	return s->pending && !s->off_sent && ready && !fod;
}

static inline bool mi_early_dimming_needs_on(struct mi_early_dimming *s,
		u32 brightness, bool blocked)
{
	return s->pending && s->off_sent && brightness &&
		!s->layer_has_aod && !blocked;
}

#endif
