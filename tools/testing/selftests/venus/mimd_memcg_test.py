#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract real per-memcg MIMD protocol/admission/reclaim functions; VM is a double."""
from pathlib import Path
import importlib.util, os, subprocess, tempfile
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('extract',HERE/'mi_memcg_test.py')
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
s=(a.ROOT/'mm/mi_memcg_mimd.c').read_text()
state=s[s.index('#define MI_MIMD_BASE'):s.index('static void mi_mimd_decode')]
env=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
typedef uint64_t u64;typedef int64_t s64;typedef uint32_t u32;
#define PAGE_SHIFT 12
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,x) ((v)=(x))
#define min(a,b) ((a)<(b)?(a):(b))
#define time_before(a,b) ((long)((a)-(b))<0)
#define msecs_to_jiffies(n) (n)
#define spin_lock_irqsave(p,f) ((f)=0,pthread_mutex_lock(p))
#define spin_unlock_irqrestore(p,f) ((void)(f),pthread_mutex_unlock(p))
struct cftype {int unused;};
struct cgroup_subsys_state {unsigned refs;bool online;};
struct mem_cgroup {struct cgroup_subsys_state css;};
struct mi_memcg {struct mem_cgroup *owner;pthread_mutex_t lock;bool online,mimd_active,mimd_foreground;u64 mimd_reclaimed_kb;};
static struct mem_cgroup group;
static struct mi_memcg data={.lock=PTHREAD_MUTEX_INITIALIZER};
static struct mem_cgroup *mem_cgroup_from_css(struct cgroup_subsys_state*c){assert(c==&group.css);return &group;}
static struct mi_memcg *mi_memcg_data(struct mem_cgroup*m){assert(m==&group);return &data;}
static bool css_tryget_online(struct cgroup_subsys_state*c){if(!c->online)return false;c->refs++;return true;}
static void css_put(struct cgroup_subsys_state*c){assert(c->refs);c->refs--;}
static unsigned long jiffies;
static unsigned mode,calls,anon_calls,file_calls,advance,action;
static bool fatal;
#define current 0
static bool fatal_signal_pending(int p){(void)p;return fatal;}
static void cond_resched(void){jiffies+=advance;}
enum {XIAOMI_RECLAIM_ANON=1,XIAOMI_RECLAIM_FILE=3};
static unsigned long xiaomi_reclaim_memcg(struct mem_cgroup*,unsigned long,int);
'''
functions=''.join(a.function(s,n) for n in ['mi_mimd_decode','mi_mimd_cancelled','mi_mimd_run','mi_mimd_reclaim_write','mi_mimd_reclaim_read','mi_mimd_foreground_write'])
tests=r'''
static unsigned long xiaomi_reclaim_memcg(struct mem_cgroup*m,unsigned long n,int type)
{
 assert(m==&group&&n>0&&n<=1024);calls++;
 if(type==XIAOMI_RECLAIM_ANON)anon_calls++;else {assert(type==XIAOMI_RECLAIM_FILE);file_calls++;}
 if(action==1) {assert(mi_mimd_reclaim_write(&group.css,NULL,1)==-EBUSY);assert(group.css.refs==1);}
 if(action==2)assert(!mi_mimd_foreground_write(&group.css,NULL,1));
 if(action==3)data.online=false;
 if(action==4)fatal=true;
 if(mode==1||(mode==2&&type==XIAOMI_RECLAIM_ANON))return 0;
 return mode==3?1:n;
}
static void reset(void)
{
 assert(!group.css.refs&&!data.mimd_active);
 group.css.online=true;data.owner=&group;data.online=true;data.mimd_foreground=false;data.mimd_reclaimed_kb=123;
 jiffies=mode=calls=anon_calls=file_calls=advance=action=0;fatal=false;
}
int main(void)
{
 struct mi_mimd_request r;
 for(u64 v=1,i=0;i<100000;i++) {
  v=v*6364136223846793005ULL+1442695040888963407ULL;
  mi_mimd_decode(v,&r);assert(r.pages==v%10000000);
  u64 digit=v/10000000%10,retry=v/100000000;
  assert(r.type==(digit==2||digit==3?digit:0));
  assert(r.retries==(retry>0&&retry<10?retry:10));
 }
 for(unsigned type=0;type<4;type++)if(type!=1) {
  reset();assert(!mi_mimd_reclaim_write(&group.css,NULL,type*10000000+12500));
  assert(calls==13&&mi_mimd_reclaim_read(&group.css,NULL)==50000);
  assert(type==0?(anon_calls==7&&file_calls==6):type==2?!file_calls:!anon_calls);
  reset();mode=1;assert(!mi_mimd_reclaim_write(&group.css,NULL,type*10000000+2500));
  assert(calls==(type?1:2)&&!mi_mimd_reclaim_read(&group.css,NULL));
 }
 reset();mode=2;assert(!mi_mimd_reclaim_write(&group.css,NULL,2500));assert(calls==6&&data.mimd_reclaimed_kb==10000);
 for(unsigned retry=1;retry<10;retry++) {
  reset();mode=3;assert(!mi_mimd_reclaim_write(&group.css,NULL,retry*100000000+20001250));
  assert(calls==retry&&data.mimd_reclaimed_kb==retry*4);
 }
 reset();mode=3;assert(!mi_mimd_reclaim_write(&group.css,NULL,20001250));assert(calls==10&&data.mimd_reclaimed_kb==40);
 reset();action=1;assert(!mi_mimd_reclaim_write(&group.css,NULL,2500));assert(data.mimd_reclaimed_kb==10000);
 for(unsigned i=2;i<=4;i++) {
  reset();action=i;assert(!mi_mimd_reclaim_write(&group.css,NULL,2500));assert(calls==1&&data.mimd_reclaimed_kb==4096);
 }
 reset();advance=5000;assert(!mi_mimd_reclaim_write(&group.css,NULL,2500));assert(calls==1&&data.mimd_reclaimed_kb==4096);
 reset();assert(!mi_mimd_reclaim_write(&group.css,NULL,9999999));assert(calls==4096&&data.mimd_reclaimed_kb==4096ULL*4096);
 reset();assert(!mi_mimd_foreground_write(&group.css,NULL,5)&&data.mimd_foreground);
 assert(!mi_mimd_foreground_write(&group.css,NULL,-1)&&!data.mimd_foreground);
 assert(!mi_mimd_foreground_write(&group.css,NULL,1));
 assert(!mi_mimd_reclaim_write(&group.css,NULL,1)&&!data.mimd_foreground&&data.mimd_reclaimed_kb==4);
 reset();group.css.online=false;assert(mi_mimd_reclaim_write(&group.css,NULL,1)==-ENODEV&&!calls&&!group.css.refs);
 reset();assert(!mi_mimd_reclaim_write(&group.css,NULL,0)&&!calls&&!data.mimd_reclaimed_kb);
 puts("PASS: actual MIMD memcg decoder 100000 wide inputs, retry/class/KB protocol, 12500-page target, CSS refs, foreground/offline/fatal cancellation, concurrent EBUSY, deadline/call bound");
}
'''
with tempfile.TemporaryDirectory(prefix='mimd-memcg-') as d:
 p=Path(d);(p/'test.c').write_text(env+state+functions+tests)
 subprocess.run([os.environ.get('HOSTCC','clang'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Wno-unused-parameter','-Werror','-fsanitize=address,undefined','-no-pie','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
