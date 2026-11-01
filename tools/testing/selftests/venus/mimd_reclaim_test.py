#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual MIMD command decoder, bounded worker and queue; reclaim is a double."""
from pathlib import Path
import importlib.util,os,subprocess,tempfile
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('extract',HERE/'mi_memcg_test.py')
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
s=(a.ROOT/'drivers/misc/perf_helper_reclaim.c').read_text()
state=s[s.index('#define MIMD_RECLAIM_QUEUE'):s.index('static int mimd_reclaim_decode')]
env=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#define PAGE_SHIFT 12
#define DEFINE_SPINLOCK(n) pthread_mutex_t n=PTHREAD_MUTEX_INITIALIZER
#define spin_lock(p) pthread_mutex_lock(p)
#define spin_unlock(p) pthread_mutex_unlock(p)
#define READ_ONCE(v) (v)
#define min(a,b) ((a)<(b)?(a):(b))
#define time_before(a,b) ((long)((a)-(b))<0)
#define msecs_to_jiffies(n) (n)
struct work_struct {int unused;};
struct workqueue_struct {int unused;};
static struct work_struct mimd_reclaim_work;
static unsigned long jiffies;
static bool queued;
static void queue_work(struct workqueue_struct*w,struct work_struct*work) {assert(w&&work==&mimd_reclaim_work);queued=true;}
enum {XIAOMI_RECLAIM_ANON=1,XIAOMI_RECLAIM_FILE=3};
static int root_object,*root_mem_cgroup=&root_object;
static unsigned calls,anon_calls,file_calls,mode,advance,stop_after;
static unsigned long results[16],result_count;
static unsigned long xiaomi_reclaim_memcg(int*,unsigned long,int);
static void cond_resched(void) {jiffies+=advance;}
static void global_reclaim_record(unsigned long n) {assert(result_count<16);results[result_count++]=n;}
'''
functions=''.join(a.function(s,n) for n in ['mimd_reclaim_decode','mimd_reclaim_run','mimd_reclaim_worker','mimd_reclaim_submit'])
tests=r'''
static unsigned long xiaomi_reclaim_memcg(int*m,unsigned long n,int type)
{
 assert(m==root_mem_cgroup && n && n<=1024);calls++;
 if(type==XIAOMI_RECLAIM_ANON)anon_calls++;else {assert(type==XIAOMI_RECLAIM_FILE);file_calls++;}
 if(calls==stop_after)mimd_reclaim_stopping=true;
 if(mode==1 || (mode==2 && type==XIAOMI_RECLAIM_ANON))return 0;
 return n;
}
static void reset(void) {calls=anon_calls=file_calls=mode=advance=stop_after=0;mimd_reclaim_stopping=false;jiffies=0;}
int main(void)
{
 struct mimd_reclaim_request r;
 assert(!mimd_reclaim_decode(30012500,&r)&&r.type==3&&r.pages==12500);
 assert(!mimd_reclaim_decode(20001250,&r)&&r.type==2&&r.pages==1250);
 assert(!mimd_reclaim_decode(262144,&r)&&!r.type&&r.pages==262144);
 assert(mimd_reclaim_decode(10000001,&r)==-EINVAL && mimd_reclaim_decode(~0UL,&r)==-EINVAL);
 for(unsigned type=0;type<4;type++)if(type!=1) {
  reset();r=(struct mimd_reclaim_request){.pages=2500,.type=type};assert(mimd_reclaim_run(&r)==2500&&calls==3);
  assert(type==0?(anon_calls==2&&file_calls==1):type==2?!file_calls:!anon_calls);
  reset();mode=1;assert(!mimd_reclaim_run(&r)&&calls==(type?1:2));
  reset();stop_after=1;assert(mimd_reclaim_run(&r)==1024&&calls==1);
  reset();advance=5000;assert(mimd_reclaim_run(&r)==1024&&calls==1);
 }
 reset();r=(struct mimd_reclaim_request){.pages=2500};mode=2;assert(mimd_reclaim_run(&r)==2500&&file_calls==3&&anon_calls==3);
 reset();r.pages=9999999;assert(mimd_reclaim_run(&r)==4096UL*1024&&calls==4096);
 reset();mimd_reclaim_wq=(void*)1;for(unsigned i=0;i<16;i++)assert(!mimd_reclaim_submit(30000001+i));
 assert(mimd_reclaim_submit(20000001)==-ENOSPC&&queued);mimd_reclaim_worker(&mimd_reclaim_work);
 assert(result_count==16&&!mimd_reclaim_count);for(unsigned i=0;i<16;i++)assert(results[i]==i+1);
 mimd_reclaim_stopping=true;assert(mimd_reclaim_submit(1)==-ESHUTDOWN);
 puts("PASS: actual MIMD 10^7 decoder, strict anon/file selection, mixed no-progress, actual page totals, stop/deadline/call bound, bounded FIFO and queue-full/shutdown errors");
}
'''
with tempfile.TemporaryDirectory(prefix='mimd-reclaim-') as d:
 p=Path(d);(p/'test.c').write_text(env+state+functions+tests)
 subprocess.run([os.environ.get('HOSTCC','clang'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Wno-unused-parameter','-Werror','-fsanitize=address,undefined','-no-pie','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
