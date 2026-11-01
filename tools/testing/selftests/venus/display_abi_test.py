#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual count ioctl, statistics and commit timer with kernel API mocks."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
BASE = ROOT / 'techpack/display/msm/mi_disp'

def source(name):
    return '\n'.join(x for x in (BASE / name).read_text().splitlines()
                     if not x.startswith('#include'))

env = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdatomic.h>
#define __user
#define __KERNEL__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#include "mi_disp.h"
#pragma GCC diagnostic pop
typedef unsigned long long u64;
typedef unsigned int u32;
typedef int s32;
typedef pthread_mutex_t spinlock_t;
#define U64_MAX UINT64_MAX
#define PAGE_SIZE 4096
#define GFP_KERNEL 0
#define max(a,b) ((a) > (b) ? (a) : (b))
#define max_t(t,a,b) max((t)(a),(t)(b))
#define spin_lock_init(p) assert(!pthread_mutex_init(p, NULL))
#define spin_lock_irqsave(p,f) do { (f)=0; assert(!pthread_mutex_lock(p)); } while(0)
#define spin_unlock_irqrestore(p,f) do { (void)(f); assert(!pthread_mutex_unlock(p)); } while(0)
#define mutex_lock(p) assert(!pthread_mutex_lock(p))
#define mutex_unlock(p) assert(!pthread_mutex_unlock(p))
static int mutex_lock_interruptible(pthread_mutex_t *p) { return -pthread_mutex_lock(p); }
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define from_timer(v,p,m) container_of(p,typeof(*v),m)
static u64 now_ns;
static u64 ktime_get_ns(void) { return now_ns; }
static unsigned long jiffies;
static unsigned long msecs_to_jiffies(u64 ms) { return (ms+3)/4; }
static int allocations;
static bool fail_alloc, fail_copy, fail_queue;
static void *kzalloc(size_t n, int flags) {
    (void)flags;
    if (fail_alloc) return NULL;
    void *p = calloc(1,n); if (p) allocations++; return p;
}
static void kfree(void *p) { if (p) { allocations--; free(p); } }
static int scnprintf(char *s,size_t n,const char *fmt,...) {
    va_list ap; va_start(ap,fmt); int r=vsnprintf(s,n,fmt,ap); va_end(ap);
    return r >= (int)n ? (int)n-1 : r;
}
typedef atomic_int atomic_t;
#define ATOMIC_INIT(v) (v)
#define atomic_read(p) atomic_load(p)
#define atomic_set(p,v) atomic_store(p,v)
static int warns, slow;
static char last_log[1024];
static void test_log(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vsnprintf(last_log,sizeof(last_log),fmt,ap); va_end(ap);
}
#define pr_warn_ratelimited(...) do { warns++; test_log(__VA_ARGS__); } while(0)
#define pr_info_ratelimited(...) do { slow++; test_log(__VA_ARGS__); } while(0)
#define pr_debug(...) test_log(__VA_ARGS__)
struct timer_list { void (*fn)(struct timer_list *); bool armed; unsigned long expires; };
#define timer_setup(p,f,flags) do { (p)->fn=(f); (p)->armed=false; (void)(flags); } while(0)
static void mod_timer(struct timer_list *p, unsigned long expires) { p->armed=true; p->expires=expires; }
static void del_timer_sync(struct timer_list *p) { p->armed=false; }
struct drm_display_mode { int fps; };
struct drm_crtc { u32 mask; };
struct drm_crtc_state { bool active,modeset; struct drm_display_mode adjusted_mode; bool active_changed; };
struct drm_atomic_state { int n; struct drm_crtc crtc[2]; struct drm_crtc_state cs[2]; };
#define for_each_new_crtc_in_state(s,c,newcs,i) for ((i)=0;(i)<(s)->n && (((c)=&(s)->crtc[i]),((newcs)=&(s)->cs[i]),1);(i)++)
static bool drm_atomic_crtc_needs_modeset(struct drm_crtc_state *s) { return s->modeset; }
static u32 drm_crtc_mask(struct drm_crtc *c) { return c->mask; }
static u32 drm_mode_vrefresh(struct drm_display_mode *m) { return m->fps; }
'''

ioctl_env = r'''
#define SDE_MODE_DPMS_OFF 5
#define MI_INTF_DSI 0
#define DSI_CMD_SET_MI_DIMMINGOFF 0
#define DSI_CMD_SET_MI_DIMMINGON 1
#define STATE_DIM_BLOCK 1
struct mi_cfg { struct mi_early_dimming early_dimming; bool in_fod_calibration; int dimming_state; int feature_val[DISP_FEATURE_MAX]; };
struct mode_info { struct { int count; } cmd_sets[2]; };
struct panel_mode { struct mode_info *priv_info; };
struct dsi_panel { struct mi_panel_stats mi_stats; pthread_mutex_t panel_lock; int power_mode; bool panel_initialized,fod; struct mi_cfg mi_cfg; struct panel_mode *cur_mode; };
#define mi_dsi_panel_cfg mi_cfg
static bool is_hbm_fod_on(struct dsi_panel *p) { return p->fod; }
static int dimming_commands[2], command_result, sleeps;
static int dsi_panel_tx_cmd_set(struct dsi_panel *p,int cmd) {
    assert(p->panel_initialized); dimming_commands[cmd]++; return command_result;
}
static void usleep_range(int low,int high) { assert(low==10000 && high==10010); sleeps++; }
struct dsi_display { struct dsi_panel *panel; };
struct disp_display { void *display; int intf_type; struct { int worker; } d_thread; };
struct disp_feature { struct disp_display d_display[2]; };
struct disp_feature_client { struct disp_feature *df; pthread_mutex_t client_lock; };
struct kthread_work { void (*fn)(struct kthread_work *); };
static struct kthread_work *queued;
static void kthread_init_work(struct kthread_work *w, void (*fn)(struct kthread_work *)) { w->fn=fn; }
static bool kthread_queue_work(int *worker, struct kthread_work *w) {
    (void)worker; if (fail_queue) return false; assert(!queued); queued=w; return true;
}
#define u64_to_user_ptr(v) ((void *)(uintptr_t)(v))
static int copy_from_user(void *dest, void *src, size_t n) {
    if (fail_copy || !src)
        return 1;
    memcpy(dest,src,n);
    return 0;
}
'''

tests = r'''
_Static_assert(sizeof(struct disp_count_req)==48,"count wire size");
_Static_assert(offsetof(struct disp_count_req,tx_ptr)==24,"tx pointer");
_Static_assert(offsetof(struct disp_count_req,rx_ptr)==40,"rx pointer");
_Static_assert(MI_DISP_IOCTL_SET_COUNT_INFO==0xc030440dUL,"count command");
static void stats_tests(void) {
    struct mi_panel_stats s, other;
    char text[PAGE_SIZE], version[100];
    mi_panel_stats_init(&s); mi_panel_stats_init(&other);
    memset(version,'A',sizeof(version));
    assert(!mi_panel_stats_set(&s,1,0,version,100,false));
    assert(strlen(s.build_version)==100);
    assert(mi_panel_stats_set(&s,1,0,version,101,false)==-EINVAL);
    assert(mi_panel_stats_set(&s,1,0,"a\nb",3,false)==-EINVAL);
    assert(!mi_panel_stats_set(&s,2,3,NULL,0,false)); assert(s.commit_long==3);
    assert(!mi_panel_stats_set(&s,1,0,version,100,false)); assert(s.commit_long==3);
    assert(!mi_panel_stats_set(&s,1,0,"build2",6,false)); assert(!s.commit_long);
    assert(!other.build_version[0]);
    for (int bad=-100;bad<0;bad++) {
        assert(mi_panel_stats_set(&s,0,bad,NULL,0,true)==-EINVAL);
        assert(mi_panel_stats_set(&s,2,bad,NULL,0,true)==-EINVAL);
    }
    assert(!mi_panel_stats_set(&s,4,100,NULL,0,false));
    assert(!mi_panel_stats_set(&s,4,100,NULL,0,false) && s.tpidle_count==100);
    assert(!mi_panel_stats_set(&s,4,1,NULL,0,false) && s.tpidle_count==1);
    assert(mi_panel_stats_set(&s,5,0,NULL,0,false)==-EOPNOTSUPP);
    now_ns=100;
    assert(!mi_panel_stats_set(&s,0,1,NULL,0,false)); assert(!s.power_pending);
    assert(!mi_panel_stats_set(&s,0,1,NULL,0,true));
    now_ns=200;
    assert(!mi_panel_stats_set(&s,0,1,NULL,0,true)); assert(s.power_start_ns==100);
    mi_panel_stats_backlight(&s,0); assert(s.power_pending);
    now_ns=350; mi_panel_stats_backlight(&s,100);
    assert(!s.power_pending && s.power_samples==1 && s.power_last_ns==250);
    mi_panel_stats_backlight(&s,200); assert(s.power_samples==1);
    assert(!mi_panel_stats_set(&s,0,1,NULL,0,true));
    mi_panel_stats_power(&s,false,true); assert(!s.power_pending);
    mi_panel_stats_fullscreen(&s,true); assert(!s.aod_active);
    now_ns=1000; mi_panel_stats_power(&s,true,false); assert(s.aod_sessions==1);
    mi_panel_stats_power(&s,true,false); mi_panel_stats_fullscreen(&s,true);
    assert(s.aod_sessions==1);
    now_ns=2500; mi_panel_stats_show(&s,text);
    assert(strstr(text,"fullscreen_aod_active_ns=1500\n"));
    mi_panel_stats_fullscreen(&s,false); assert(s.aod_total_ns==1500 && s.aod_longest_ns==1500);
    now_ns=3000; mi_panel_stats_fullscreen(&s,true);
    now_ns=3500; mi_panel_stats_power(&s,false,false);
    assert(s.aod_sessions==2 && s.aod_total_ns==2000 && s.aod_longest_ns==1500);
    s.commit_long=U64_MAX-1; assert(!mi_panel_stats_set(&s,2,2,NULL,0,false)); assert(s.commit_long==U64_MAX);
    s.power_total_ns=U64_MAX-1; s.power_samples=U64_MAX;
    assert(!mi_panel_stats_set(&s,0,1,NULL,0,true)); now_ns+=100; mi_panel_stats_backlight(&s,1);
    assert(s.power_total_ns==U64_MAX && s.power_samples==U64_MAX);
}
static void timer_tests(void) {
    struct drm_atomic_state state={.n=2,.crtc={{1},{2}},.cs={{true,false,{60},false},{true,false,{120},false}}};
    assert(!mi_disp_timer_begin(&state));
    mi_disp_timer_enable(true); fail_alloc=true; assert(!mi_disp_timer_begin(&state)); fail_alloc=false;
    now_ns=100; struct mi_commit_timer *r=mi_disp_timer_begin(&state);
    assert(r && r->crtc_mask==3 && r->frame_budget_ns==1600000000ULL/120);
    assert(r->timer.armed && r->timer.expires==10);
    now_ns=1000; mi_disp_timer_stage(r,MI_COMMIT_PREPARE); assert(r->stage_ns[0]==900);
    now_ns=2000; r->timer.fn(&r->timer); assert(warns==1 && strstr(last_log,"stalled in prepare"));
    mi_disp_timer_enable(false); r->timer.fn(&r->timer); assert(warns==1);
    mi_disp_timer_end(r); assert(!allocations);
    mi_disp_timer_enable(true); r=mi_disp_timer_begin(&state); now_ns+=20000000;
    mi_disp_timer_end(r); assert(slow==1 && !allocations);
    state.cs[0].modeset=true; r=mi_disp_timer_begin(&state); now_ns+=20000000;
    mi_disp_timer_end(r); assert(slow==1 && !allocations);
    state.cs[0].active=state.cs[1].active=false; r=mi_disp_timer_begin(&state);
    assert(!r->frame_budget_ns && r->timer.expires==25); mi_disp_timer_end(r);
    state.cs[0].active_changed=true; state.cs[0].active=true;
    assert(!mi_disp_timer_set_power_timeout(9,700));
    r=mi_disp_timer_begin(&state); assert(r->timer.expires==175); mi_disp_timer_end(r);
    state.cs[0].active=false;
    assert(!mi_disp_timer_set_power_timeout(10,900));
    r=mi_disp_timer_begin(&state); assert(r->timer.expires==225); mi_disp_timer_end(r);
    assert(mi_disp_timer_set_power_timeout(9,0)==-EINVAL);
    assert(mi_disp_timer_set_power_timeout(10,60001)==-EINVAL);
    mi_disp_timer_enable(false);
}
static void ioctl_tests(void) {
    struct dsi_panel panel={0}; struct dsi_display display={&panel};
    struct disp_feature feature={.d_display={{&display,MI_INTF_DSI, {0}},{NULL,MI_INTF_DSI,{0}}}};
    struct disp_feature_client client={.df=&feature};
    struct disp_count_req req={.count_type=1};
    char payload[92]="VenusPort.OS4.0.5";
    mi_panel_stats_init(&panel.mi_stats);
    pthread_mutex_init(&panel.panel_lock,NULL); pthread_mutex_init(&client.client_lock,NULL);
    req.tx_len=92; req.tx_ptr=(uintptr_t)payload; req.base.flag=1;
    assert(!mi_disp_ioctl_set_count_info(&client,&req)); assert(queued && allocations==1);
    memset(payload,'X',sizeof(payload)); /* The caller may free/replace its buffer immediately. */
    client.df=NULL; /* Queued work must not retain the ioctl client. */
    queued->fn(queued); queued=NULL; client.df=&feature;
    assert(!allocations && !strcmp(panel.mi_stats.build_version,"VenusPort.OS4.0.5"));
    req.count_type=2; req.count_value=5; req.tx_len=0; req.base.flag=0;
    assert(!mi_disp_ioctl_set_count_info(&client,&req)); assert(panel.mi_stats.commit_long==5);
    for (u32 i=5;i<256;i++) { if (i==9 || i==10) continue; req.count_type=i; assert(mi_disp_ioctl_set_count_info(&client,&req)==-EOPNOTSUPP); }
    req.count_type=9; req.count_value=500; assert(!mi_disp_ioctl_set_count_info(&client,&req));
    assert(atomic_read(&mi_unblank_timeout_ms)==500);
    req.count_type=10; req.count_value=600; assert(!mi_disp_ioctl_set_count_info(&client,&req));
    assert(atomic_read(&mi_blank_timeout_ms)==600);
    req.count_type=3; req.count_value=2; assert(mi_disp_ioctl_set_count_info(&client,&req)==-EINVAL);
    req.count_value=1; assert(!mi_disp_ioctl_set_count_info(&client,&req)); assert(atomic_read(&mi_timer_enabled));
    req.count_value=0; assert(!mi_disp_ioctl_set_count_info(&client,&req)); assert(!atomic_read(&mi_timer_enabled));
    req.base.disp_id=1; assert(mi_disp_ioctl_set_count_info(&client,&req)==-ENODEV);
    req.base.disp_id=2; assert(mi_disp_ioctl_set_count_info(&client,&req)==-EINVAL);
    req.base.disp_id=0; req.rx_len=4; assert(mi_disp_ioctl_set_count_info(&client,&req)==-EINVAL); req.rx_len=0;
    fail_alloc=true; assert(mi_disp_ioctl_set_count_info(&client,&req)==-ENOMEM); fail_alloc=false;
    fail_queue=true; req.base.flag=1; assert(mi_disp_ioctl_set_count_info(&client,&req)==-EBUSY); fail_queue=false;
    req.count_type=1; req.tx_len=92; fail_copy=true;
    assert(mi_disp_ioctl_set_count_info(&client,&req)==-EFAULT); fail_copy=false;
    assert(!allocations && !queued);
    assert(is_support_disp_feature_id(35) && !is_support_disp_feature_id(34));
    assert(is_support_disp_feature_id(39) && is_support_disp_feature_id(40));
}
static void dimming_tests(void) {
    struct mode_info info={.cmd_sets={{1},{1}}}; struct panel_mode mode={&info};
    struct dsi_panel p={.cur_mode=&mode}; struct mi_early_dimming *d=&p.mi_cfg.early_dimming;
    mi_early_dimming_request(d,true);
    assert(!mi_dsi_panel_prepare_early_dimming(&p) && !d->off_sent);
    p.panel_initialized=true; p.fod=true;
    assert(!mi_dsi_panel_prepare_early_dimming(&p) && !dimming_commands[0]);
    p.fod=false; command_result=-EIO;
    assert(mi_dsi_panel_prepare_early_dimming(&p)==-EIO && !d->off_sent && d->pending);
    command_result=0;
    assert(!mi_dsi_panel_prepare_early_dimming(&p) && d->off_sent);
    assert(dimming_commands[0]==2);
    mi_early_dimming_request(d,true); mi_early_dimming_request(d,false);
    assert(!mi_dsi_panel_prepare_early_dimming(&p) && dimming_commands[0]==2);
    assert(!mi_dsi_panel_finish_early_dimming(&p,0) && !sleeps && d->pending);
    d->layer_has_aod=true;
    assert(!mi_dsi_panel_finish_early_dimming(&p,100) && !sleeps && d->pending);
    d->layer_has_aod=false; p.mi_cfg.dimming_state=STATE_DIM_BLOCK;
    assert(!mi_dsi_panel_finish_early_dimming(&p,100) && !sleeps && d->pending);
    p.mi_cfg.dimming_state=0; p.fod=true;
    assert(!mi_dsi_panel_finish_early_dimming(&p,100) && !sleeps && d->pending);
    p.fod=false; command_result=-EIO;
    assert(mi_dsi_panel_finish_early_dimming(&p,100)==-EIO && sleeps==1 && d->pending);
    command_result=0;
    assert(!mi_dsi_panel_finish_early_dimming(&p,100) && sleeps==2 && !d->pending && !d->off_sent);
    assert(dimming_commands[1]==2 && p.mi_cfg.feature_val[DISP_FEATURE_DIMMING]==FEATURE_ON);
    assert(!mi_dsi_panel_finish_early_dimming(&p,100) && sleeps==2);
    mi_early_dimming_request(d,true); info.cmd_sets[1].count=0;
    assert(mi_dsi_panel_prepare_early_dimming(&p)==-EOPNOTSUPP && !d->off_sent);
}
int main(void) {
    fprintf(stderr,"display_abi_test: main entered\n");
    stats_tests(); timer_tests(); ioctl_tests(); dimming_tests();
    puts("PASS: actual count ioctl ownership/errors; 48-byte ABI; build attribution, power/AOD clocks, saturation; commit timer enable/stages/expiry/cancel; early dimming actual DCS commands, FOD/AOD/zero-DBV gating and failed-command retry");
    return 0;
}
'''

dimming = (BASE / 'mi_dsi_panel.c').read_text()
dimming = dimming[dimming.index('int mi_dsi_panel_prepare_early_dimming('):dimming.index('void mi_dsi_panel_update_last_bl_level(')]
handler = (BASE / 'mi_disp_file.c').read_text()
handler = handler[handler.index('struct mi_count_work {'):handler.index('int mi_disp_open(')]
with tempfile.TemporaryDirectory(prefix='venus-display-abi-test-') as tmp:
    tmp=Path(tmp)
    (tmp/'test.c').write_text(env + source('mi_panel_stats.h') + source('mi_panel_stats.c') +
        source('mi_disp_timer.h') + source('mi_disp_timer.c') + source('mi_early_dimming.h') + ioctl_env + dimming + handler + tests)
    subprocess.run([os.environ.get('HOSTCC','clang'), '-std=gnu11','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined',
        '-no-pie','-pthread','-I'+str(ROOT/'techpack/display/include/uapi/display/drm'),
        str(tmp/'test.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
