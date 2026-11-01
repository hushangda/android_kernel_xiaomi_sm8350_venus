/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MI_DISP_TIMER_H_
#define _MI_DISP_TIMER_H_

#include <linux/types.h>

struct mi_commit_timer;
struct drm_atomic_state;

enum mi_commit_stage {
	MI_COMMIT_FENCES,
	MI_COMMIT_PREPARE,
	MI_COMMIT_DISABLE,
	MI_COMMIT_PLANES,
	MI_COMMIT_ENABLE,
	MI_COMMIT_WAIT,
	MI_COMMIT_CLEANUP,
	MI_COMMIT_COMPLETE,
	MI_COMMIT_STAGES,
};

void mi_disp_timer_enable(bool enable);
int mi_disp_timer_set_power_timeout(u32 type, s32 milliseconds);
struct mi_commit_timer *mi_disp_timer_begin(struct drm_atomic_state *state);
void mi_disp_timer_stage(struct mi_commit_timer *record, enum mi_commit_stage stage);
void mi_disp_timer_end(struct mi_commit_timer *record);

#endif
