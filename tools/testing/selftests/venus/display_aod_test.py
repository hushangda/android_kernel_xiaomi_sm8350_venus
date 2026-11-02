#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual K2 AOD routing/state code with mocked panel transport."""
from pathlib import Path
import tempfile
import subprocess
root=Path(__file__).resolve().parents[4]
work=tempfile.TemporaryDirectory(prefix='venus-aod-')
p=Path(work.name)
s=(root/'techpack/display/msm/mi_disp/mi_dsi_panel.c').read_text()
d=(root/'techpack/display/msm/dsi/dsi_panel.c').read_text()
def fn(src,name):
 start=src.index(name+'('); start=src.rfind('\n',0,start)+1
 a=src.index('{',start); depth=1; b=a+1
 while depth:
  depth+=(src[b]=='{')-(src[b]=='}');b+=1
 return src[start:b]+'\n'
env=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define MI_DIMLAYER_AOD 4
#define MI_DIMLAYER_FOD_ANIM 8
#define MI_DIMLAYER_BL_PRESERVE 16
#define DSI_CORE_CLK 1
#define DSI_CLK_ON 1
#define DSI_CLK_OFF 0
#define EOPNOTSUPP 95
#define FEATURE_ON 1
#define DISP_FEATURE_FULLSCREEN_AOD 35
#define DISP_FEATURE_DC 8
#define DISP_FEATURE_FP_STATUS 12
#define ENROLL_STOP 2
#define AUTH_STOP 4
#define HEART_RATE_STOP 6
#define DOZE_TO_NORMAL 0
#define DOZE_BRIGHTNESS_HBM 1
#define DOZE_BRIGHTNESS_LBM 2
#define PANEL_STATE_DOZE_LOW 3
#define PANEL_STATE_DOZE_HIGH 2
#define PANEL_STATE_ON 1
#define STATE_DIM_BLOCK 1
#define SDE_MODE_DPMS_ON 0
#define SDE_MODE_DPMS_LP1 1
#define SDE_MODE_DPMS_LP2 2
#define SDE_MODE_DPMS_OFF 5
#define DSI_BACKLIGHT_WLED 1
#define DSI_BACKLIGHT_DCS 2
#define DSI_BACKLIGHT_EXTERNAL 3
#define DSI_BACKLIGHT_PWM 4
#define ENOTSUPP EOPNOTSUPP
#define DISP_INFO(...) do {} while(0)
#define DSI_INFO(...) do {} while(0)
#define DSI_DEBUG(...) do {} while(0)
#define DSI_ERR(...) do {} while(0)
enum dsi_cmd_set_type {DSI_CMD_SET_MI_DOZE_HBM,DSI_CMD_SET_MI_DOZE_LBM,DSI_CMD_SET_MI_DOZE_HBM_NOLP,DSI_CMD_SET_MI_DOZE_LBM_NOLP};
struct mi_dsi_panel_cfg {
 uint64_t panel_id; int feature_val[41]; bool aod_bl_51ctl;
 bool fullscreen_aod_bl_valid,fullscreen_aod_bl_pending; u32 fullscreen_aod_bl,fullscreen_aod_layer_state;
 bool in_fod_calibration,local_hbm_to_normal,fod_anim_on;
 u32 doze_brightness,doze_brightness_backup; int panel_state,dimming_state;
 int doze_lbm_dbv_level,doze_hbm_dbv_level;
 bool aod_brightness_work_flag,bl_need_update;
 u32 last_bl_level;
};
struct priv {struct {int count;} cmd_sets[4];};
struct mode {struct priv *priv_info;};
struct dsi_backlight_config {int type; void *raw_bd; bool bl_inverted_dbv;};
struct dsi_panel {
 struct mi_dsi_panel_cfg mi_cfg; int panel_lock,power_mode; bool panel_initialized,fod;
 struct mode *cur_mode; int mipi_device,mi_count,mi_stats;
 struct {bool ext_bridge_mode;} host_config;
 struct dsi_backlight_config bl_config; const char *type;
};
struct dsi_display {struct dsi_panel *panel; int dsi_clk_handle;};
static int clock_on_rc, clock_off_rc, clock_calls, lock_depth;
static void mutex_lock(int *m){(void)m;assert(!lock_depth);lock_depth++;}
static void mutex_unlock(int *m){(void)m;assert(lock_depth==1);lock_depth--;}
static int dsi_display_clk_ctrl(int h,int c,int on){(void)h;(void)c;assert(lock_depth==1);clock_calls++;return on?clock_on_rc:clock_off_rc;}
static int cmd_count,zero_count,last_cmd,cmd_rc,patch_rc,zero_rc,normal_count,dc_updates;
static bool skip_normal;
static int sequence[100], seqn, nonzero_rc;
static bool dsi_panel_initialized(struct dsi_panel *p){return p->panel_initialized;}
static bool is_hbm_fod_on(struct dsi_panel *p){return p->fod;}
static void mi_dsi_panel_update_last_bl_level(struct dsi_panel *p,u32 v){p->mi_cfg.last_bl_level=v;}
static int mipi_dsi_dcs_set_display_brightness(int *d,u32 v){(void)d;sequence[seqn++]=v;if(!v){zero_count++;return zero_rc;}return nonzero_rc;}
static void mi_panel_count_backlight(int *s,int v){*s=v;}
static void mi_panel_stats_backlight(int *s,int v){*s=v;}
static int mi_dsi_update_hbm_cmd_51reg(struct dsi_panel *p,enum dsi_cmd_set_type t,int v){(void)p;assert(v==(t?22:196));return patch_rc;}
static int dsi_panel_tx_cmd_set(struct dsi_panel *p,enum dsi_cmd_set_type t){assert(p->panel_initialized);cmd_count++;last_cmd=t;sequence[seqn++]=100000+t;return cmd_rc;}
static bool is_backlight_set_skip(struct dsi_panel *p,u32 v){(void)p;(void)v;return skip_normal;}
static int dsi_panel_update_backlight(struct dsi_panel *p,u32 v){(void)p;(void)v;normal_count++;return 0;}
static int dsi_panel_update_pwm_backlight(struct dsi_panel *p,u32 v){return dsi_panel_update_backlight(p,v);}
static int backlight_device_set_brightness(void *p,u32 v){(void)p;(void)v;return 0;}
static void mi_dsi_backlight_logging(struct dsi_panel *p,u32 v){(void)p;(void)v;}
static int mi_dsi_panel_update_dc_status(struct dsi_panel *p,u32 v){(void)p;(void)v;dc_updates++;return 0;}
'''
test=r'''
int main(void){
 struct priv priv={.cmd_sets={{1},{1},{1},{1}}}; struct mode mode={&priv};
 struct dsi_panel p={.panel_initialized=true,.power_mode=SDE_MODE_DPMS_LP1,.cur_mode=&mode,.bl_config={.type=DSI_BACKLIGHT_DCS}};
 struct mi_dsi_panel_cfg *c=&p.mi_cfg;
 c->panel_id=0x4A3200380800ULL;c->feature_val[35]=1;c->feature_val[8]=1;
 c->doze_hbm_dbv_level=196;c->doze_lbm_dbv_level=22;
 skip_normal=true;
 assert(!dsi_panel_set_backlight(&p,78));assert(cmd_count==1 && last_cmd==0 && c->doze_brightness==1 && c->doze_brightness_backup==1);
 assert(!normal_count && !dc_updates && c->feature_val[8]==1);
 assert(!dsi_panel_set_backlight(&p,326));assert(cmd_count==1);
 assert(!dsi_panel_set_backlight(&p,20));assert(cmd_count==2 && last_cmd==1);
 p.power_mode=SDE_MODE_DPMS_LP2;
 assert(!dsi_panel_set_backlight(&p,80));assert(cmd_count==3 && last_cmd==0);
 assert(!dsi_panel_set_backlight(&p,0));assert(zero_count==1 && c->doze_brightness==0 && c->doze_brightness_backup==1 && c->feature_val[8]==1);
 assert(!dsi_panel_set_backlight(&p,80));assert(cmd_count==4);
 p.fod=true;assert(!dsi_panel_set_backlight(&p,0));assert(zero_count==1 && c->fullscreen_aod_bl==0);
 p.fod=false;assert(!mi_dsi_panel_restore_fullscreen_aod_backlight(&p));assert(zero_count==2);
 c->fod_anim_on=true;assert(!dsi_panel_set_backlight(&p,80));assert(cmd_count==4);
 c->fod_anim_on=false;assert(!mi_dsi_panel_restore_fullscreen_aod_backlight(&p));assert(cmd_count==5);
 c->local_hbm_to_normal=true;assert(!dsi_panel_set_backlight(&p,20));assert(cmd_count==5);
 c->local_hbm_to_normal=false;c->in_fod_calibration=true;assert(!mi_dsi_panel_restore_fullscreen_aod_backlight(&p));assert(cmd_count==5);
 c->in_fod_calibration=false;cmd_rc=-EIO;
 assert(mi_dsi_panel_restore_fullscreen_aod_backlight(&p)==-EIO);assert(c->doze_brightness==0 && c->doze_brightness_backup==1);
 cmd_rc=0;assert(!mi_dsi_panel_restore_fullscreen_aod_backlight(&p));assert(c->doze_brightness==2);
 zero_rc=-EIO;assert(dsi_panel_set_backlight(&p,0)==-EIO && c->doze_brightness==2);zero_rc=0;
 c->panel_state=PANEL_STATE_ON;int n=cmd_count;
 assert(!dsi_panel_set_backlight(&p,20));assert(cmd_count==n+1); // reset/mode entry must reapply same level
 priv.cmd_sets[0].count=0;assert(dsi_panel_set_backlight(&p,80)==-EOPNOTSUPP);priv.cmd_sets[0].count=1;
 patch_rc=-EINVAL;n=cmd_count;assert(dsi_panel_set_backlight(&p,80)==-EINVAL && cmd_count==n);patch_rc=0;
 c->doze_lbm_dbv_level=0;assert(dsi_panel_set_backlight(&p,80)==-EOPNOTSUPP);c->doze_lbm_dbv_level=22;
 p.power_mode=SDE_MODE_DPMS_ON;n=cmd_count;
 assert(!dsi_panel_set_backlight(&p,80) && cmd_count==n && !normal_count); // normal DC guard retained
 skip_normal=false;assert(!dsi_panel_set_backlight(&p,500));assert(normal_count==1 && dc_updates==1);
 p.power_mode=SDE_MODE_DPMS_LP1;c->panel_id=123;assert(!mi_dsi_panel_is_k2_fullscreen_aod(&p));
 c->panel_id=0x4A3200380800ULL;c->feature_val[35]=0;assert(!mi_dsi_panel_is_k2_fullscreen_aod(&p));
 c->feature_val[35]=1;c->aod_bl_51ctl=true;assert(!mi_dsi_panel_is_k2_fullscreen_aod(&p));c->aod_bl_51ctl=false;
 p.panel_initialized=false;assert(!mi_dsi_panel_is_k2_fullscreen_aod(&p));
 // First-frame animation must switch mode while dark, then apply DBV.
 p.panel_initialized=true;c->fullscreen_aod_layer_state=MI_DIMLAYER_FOD_ANIM;c->feature_val[12]=3;
 c->doze_brightness=1;c->panel_state=PANEL_STATE_ON;c->fod_anim_on=false;
 seqn=0;assert(!dsi_panel_set_backlight(&p,58));
 assert(seqn==3 && sequence[0]==0 && sequence[1]==100002 && sequence[2]==196);
 assert(c->fod_anim_on && c->panel_state==PANEL_STATE_ON && c->doze_brightness_backup==1);
 // The subsequent animation fence must not re-enter native high doze.
 n=cmd_count;assert(!dsi_panel_set_backlight(&p,58));assert(cmd_count==n);
 c->fod_anim_on=false;c->doze_brightness=0;p.bl_config.bl_inverted_dbv=true;
 seqn=0;assert(!dsi_panel_set_backlight(&p,20));
 assert(seqn==3 && sequence[0]==0 && sequence[1]==100003 && sequence[2]==0x1600);
 p.bl_config.bl_inverted_dbv=false;c->fod_anim_on=false;c->doze_brightness=0;
 zero_rc=-EIO;seqn=0;assert(dsi_panel_set_backlight(&p,58)==-EIO);
 assert(seqn==1 && !c->fod_anim_on && c->doze_brightness==0);zero_rc=0;
 cmd_rc=-EIO;seqn=0;assert(dsi_panel_set_backlight(&p,58)==-EIO);
 assert(seqn==2 && sequence[0]==0 && !c->fod_anim_on);cmd_rc=0;
 nonzero_rc=-EIO;seqn=0;assert(dsi_panel_set_backlight(&p,58)==-EIO);
 assert(seqn==3 && !c->fod_anim_on && c->doze_brightness==0);nonzero_rc=0;
 priv.cmd_sets[2].count=0;seqn=0;
 assert(dsi_panel_set_backlight(&p,58)==-EOPNOTSUPP && seqn==0);priv.cmd_sets[2].count=1;
 // No animation, stopped authentication and FOD/calibration keep their old paths.
 c->feature_val[12]=AUTH_STOP;seqn=0;assert(!dsi_panel_set_backlight(&p,58));
 assert(seqn==1 && sequence[0]==100000 && !c->fod_anim_on);
 c->feature_val[12]=3;c->doze_brightness=0;c->in_fod_calibration=true;
 seqn=0;assert(!dsi_panel_set_backlight(&p,58));assert(seqn==0);
 c->in_fod_calibration=false;c->fullscreen_aod_layer_state=0;
 seqn=0;assert(!dsi_panel_set_backlight(&p,58));assert(seqn==1 && sequence[0]==100000);

 // Reproduce aod2 failure: preserve-only first frame must issue no command.
 struct dsi_display display={.panel=&p};
 c->fod_anim_on=false;c->fullscreen_aod_layer_state=0x10;c->doze_brightness=0;
 seqn=0;assert(!dsi_panel_set_backlight(&p,58));
 assert(seqn==0 && c->fullscreen_aod_bl_pending && c->fullscreen_aod_bl==58);
 assert(!mi_sde_connector_replay_aod_backlight(&display));assert(seqn==0 && clock_calls==0);
 // An empty intermediate frame does not accidentally release the pending DBV.
 c->fullscreen_aod_layer_state=0;assert(!dsi_panel_set_backlight(&p,57));
 assert(!mi_sde_connector_replay_aod_backlight(&display));assert(seqn==0 && c->fullscreen_aod_bl==57);
 // Ready animation releases it even when doze_brightness is still zero.
 c->fullscreen_aod_layer_state=0x18;
 clock_on_rc=-EIO;assert(mi_sde_connector_replay_aod_backlight(&display)==-EIO);
 assert(c->fullscreen_aod_bl_pending && seqn==0 && !lock_depth);clock_on_rc=0;
 cmd_rc=-EIO;assert(mi_sde_connector_replay_aod_backlight(&display)==-EIO);
 assert(c->fullscreen_aod_bl_pending && !c->fod_anim_on && !lock_depth);cmd_rc=0;
 seqn=0;assert(!mi_sde_connector_replay_aod_backlight(&display));
 assert(seqn==3 && sequence[0]==0 && sequence[1]==100002 && sequence[2]==196);
 assert(!c->fullscreen_aod_bl_pending && c->fod_anim_on && !lock_depth);
 n=clock_calls;assert(!mi_sde_connector_replay_aod_backlight(&display));assert(clock_calls==n);
 // Native AOD (without fingerprint animation) also releases the deferred level.
 c->fod_anim_on=false;c->fullscreen_aod_layer_state=0x10;seqn=0;
 assert(!dsi_panel_set_backlight(&p,20));assert(seqn==0 && c->fullscreen_aod_bl_pending);
 c->fullscreen_aod_layer_state=0x14;
 assert(!mi_sde_connector_replay_aod_backlight(&display));
 assert(seqn==1 && sequence[0]==100001 && !c->fullscreen_aod_bl_pending && !c->fod_anim_on);
 // An off request cancels the pending illumination, and FOD retains ownership.
 c->fullscreen_aod_layer_state=0x10;seqn=0;assert(!dsi_panel_set_backlight(&p,58));
 assert(c->fullscreen_aod_bl_pending);assert(!dsi_panel_set_backlight(&p,0));
 assert(seqn==1 && sequence[0]==0 && !c->fullscreen_aod_bl_pending);
 assert(!dsi_panel_set_backlight(&p,58));c->fullscreen_aod_layer_state=0x18;p.fod=true;
 seqn=0;assert(!mi_sde_connector_replay_aod_backlight(&display));
 assert(seqn==0 && c->fullscreen_aod_bl_pending);p.fod=false;
 assert(!mi_sde_connector_replay_aod_backlight(&display));assert(seqn==3 && !c->fullscreen_aod_bl_pending);
 puts("PASS: actual deferred-frame replay: 0x10->0x18/0x14, empty frame, clock/DSI failures, zero cancellation, FOD isolation");
 puts("PASS: first-frame animation zero->NOLP->DBV ordering, byte order, failure propagation and native AOD isolation");
 puts("PASS: actual AOD helper/routing, DC isolation, LP1/LP2, zero, FOD deferral/replay, reset, failures, panel scope");
}
'''
code=env+''.join(fn(s,n) for n in ['is_aod_and_panel_initialized','mi_dsi_panel_is_k2_fullscreen_aod','mi_dsi_panel_set_fullscreen_aod_backlight','mi_dsi_panel_restore_fullscreen_aod_backlight'])+fn(d,'dsi_panel_set_backlight')+fn((root/'techpack/display/msm/mi_disp/mi_sde_connector.c').read_text(),'mi_sde_connector_replay_aod_backlight')+test
(p/'test-aod.c').write_text(code)
for cc in ['gcc','clang']:
 subprocess.run([cc,'-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-g',str(p/'test-aod.c'),'-o',str(p/('test-aod-'+cc))],check=True)
 subprocess.run([str(p/('test-aod-'+cc))],check=True)

# The native command path must reject malformed DT command indices/payloads.
bounds_env=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint8_t u8;
#define DISP_ERROR(...) do {} while(0)
#define DISP_DEBUG(...) do {} while(0)
#define DISP_INFO(...) do {} while(0)
enum dsi_cmd_set_type { DSI_CMD_SET_MI_HBM_ON, DSI_CMD_SET_MI_HBM_OFF,
DSI_CMD_SET_MI_HBM_FOD_ON, DSI_CMD_SET_MI_HBM_FOD_OFF,
DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM,
DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL, DSI_CMD_SET_MI_DOZE_HBM, DSI_CMD_SET_MI_DOZE_LBM };
struct mi_dsi_panel_cfg {int hbm_on_51_index,hbm_off_51_index,hbm_fod_on_51_index,
hbm_fod_off_51_index,local_hbm_on_1000nit_51_index,local_hbm_off_to_hbm_51_index,
local_hbm_off_to_normal_51_index,aod_hbm_51_index,aod_lbm_51_index;};
struct dsi_cmd_desc {struct {void *tx_buf;size_t tx_len;} msg;};
struct dsi_display_mode_priv_info {struct {struct dsi_cmd_desc *cmds;u32 count;} cmd_sets[9];};
struct mode {struct dsi_display_mode_priv_info *priv_info;};
struct dsi_panel {struct mode *cur_mode;struct mi_dsi_panel_cfg mi_cfg;const char *type;};
'''
start=s.rindex('static int mi_dsi_update_hbm_cmd_51reg(')
bounds_test=r'''
int main(void){
 u8 data[3]={0x51,0xaa,0xbb};struct dsi_cmd_desc cmd={.msg={data,3}};
 struct dsi_display_mode_priv_info info={0};struct mode mode={&info};
 struct dsi_panel p={.cur_mode=&mode};int t=DSI_CMD_SET_MI_DOZE_HBM;
 info.cmd_sets[t].cmds=&cmd;info.cmd_sets[t].count=1;
 p.mi_cfg.aod_hbm_51_index=1;assert(mi_dsi_update_hbm_cmd_51reg(&p,t,196)==-EINVAL);
 p.mi_cfg.aod_hbm_51_index=0;
 for(size_t n=0;n<3;n++){cmd.msg.tx_len=n;assert(mi_dsi_update_hbm_cmd_51reg(&p,t,196)==-EINVAL);assert(data[1]==0xaa && data[2]==0xbb);}
 cmd.msg.tx_len=3;assert(!mi_dsi_update_hbm_cmd_51reg(&p,t,196));assert(data[1]==0 && data[2]==196);
 cmd.msg.tx_buf=NULL;assert(mi_dsi_update_hbm_cmd_51reg(&p,t,22)==-EINVAL);
 p.mi_cfg.aod_hbm_51_index=-1;assert(!mi_dsi_update_hbm_cmd_51reg(&p,t,22));
 puts("PASS: actual DT 0x51 updater rejects one-past-end, short and missing payloads");
}
'''
(p/'test-bounds.c').write_text(bounds_env+fn(s[start:],'mi_dsi_update_hbm_cmd_51reg')+bounds_test)
for cc in ['gcc','clang']:
 subprocess.run([cc,'-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test-bounds.c'),'-o',str(p/('test-bounds-'+cc))],check=True)
 subprocess.run([str(p/('test-bounds-'+cc))],check=True)
