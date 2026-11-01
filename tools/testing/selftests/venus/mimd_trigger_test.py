#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual MIMD parser/FIFO with concurrent producers and a consumer."""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('extract', HERE / 'mi_memcg_test.py')
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)
s = (a.ROOT / 'drivers/misc/perf_helper_mimd.c').read_text()
state = s[s.index('#define MIMD_EVENT_SIZE'):s.index('static struct kobject *mimd_module')]
env = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/types.h>
typedef uint32_t u32;
typedef unsigned long long u64;
typedef uint8_t u8;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define PAGE_SIZE 4096
#define DEFINE_SPINLOCK(n) pthread_mutex_t n=PTHREAD_MUTEX_INITIALIZER
#define spin_lock(p) pthread_mutex_lock(p)
#define spin_unlock(p) pthread_mutex_unlock(p)
#define scnprintf snprintf
struct kobject {};
struct kobj_attribute {};
'''
functions = ''.join(a.function(s, name) for name in ['mimd_parse_event', 'mimdtrigger_store', 'mimdtrigger_show', 'stats_show', 'mimdlegacy_store', 'mimdlegacy_show', 'legacy_stats_show'])
tests = r'''
static atomic_int finished;
static bool seen[4][10000];
static void *producer(void *arg)
{
 unsigned long id=(unsigned long)arg;char buf[40];
 for(unsigned i=0;i<10000;i++) {
  int n=snprintf(buf,sizeof(buf),"5:%lu:%u",id,i);ssize_t ret;
  do { ret=mimdtrigger_store(NULL,NULL,buf,n);if(ret==-ENOSPC)sched_yield(); } while(ret==-ENOSPC);
  assert(ret==n);
 }
 atomic_fetch_add(&finished,1);return NULL;
}
static void *consumer(void *unused)
{
 (void)unused;char buf[4096];unsigned total=0;
 while(total<40000) {
  ssize_t n=mimdtrigger_show(NULL,NULL,buf);if(!n){sched_yield();continue;}
  assert(n>0 && n<40);buf[n]=0;unsigned uid,index;char tail;
  assert(sscanf(buf,"5:%u:%u%c",&uid,&index,&tail)==3 && tail=='\n');
  assert(uid<4 && index<10000 && !seen[uid][index]);seen[uid][index]=true;total++;
 }
 return NULL;
}
int main(void)
{
 struct mimd_event e;char out[4096];
 const char *good[]={"1:1","1:2\n","2:0","3:2147483647","4:25","5:10000:9","6:9"};
 const char *bad[]={"","\n","0:1","7:1","1:0","1:3","2",":1","2:","2:1:","2::1","5:1","2:1:3","5:1:2:3","-1:2","2:-1","2:2147483648","2:1\n2:2","2:1 ","2:1\r\n"};
 for(unsigned i=0;i<ARRAY_SIZE(good);i++)assert(!mimd_parse_event(good[i],strlen(good[i]),&e));
 for(unsigned i=0;i<ARRAY_SIZE(bad);i++)assert(mimd_parse_event(bad[i],strlen(bad[i]),&e)==-EINVAL);
 assert(mimd_parse_event("2:1\0x",5,&e)==-EINVAL);
 char huge[41];memset(huge,'1',sizeof(huge));assert(mimd_parse_event(huge,sizeof(huge),&e)==-EINVAL);
 for(unsigned repeat=0;repeat<10;repeat++) {
  for(unsigned i=0;i<MIMD_QUEUE_SIZE;i++) {char buf[40];int n=snprintf(buf,sizeof(buf),"2:%u",i);assert(mimdtrigger_store(NULL,NULL,buf,n)==n);}
  assert(mimdtrigger_store(NULL,NULL,"1:1",3)==-ENOSPC);
  for(unsigned i=0;i<MIMD_QUEUE_SIZE;i++) {char buf[40];int n=snprintf(buf,sizeof(buf),"2:%u\n",i);assert(mimdtrigger_show(NULL,NULL,out)==n && !memcmp(buf,out,n));}
  assert(!mimdtrigger_show(NULL,NULL,out));
 }
 assert(!mimdlegacy_show(NULL,NULL,out));
 const char *legacy_bad[]={"", "\n", "20", "-1", "1:1", "999", "0\n0", "1 ", "1\r\n"};
 for(unsigned i=0;i<ARRAY_SIZE(legacy_bad);i++)assert(mimdlegacy_store(NULL,NULL,legacy_bad[i],strlen(legacy_bad[i]))==-EINVAL);
 assert(mimdlegacy_store(NULL,NULL,"1\0",2)==-EINVAL);
 for(unsigned i=0;i<20;i++) {
  char b[4];int n=snprintf(b,sizeof(b),"%u\n",i);
  assert(mimdlegacy_store(NULL,NULL,b,n)==n);
  assert(mimdlegacy_store(NULL,NULL,"0",1)==-ENOSPC);
  assert(!mimdtrigger_show(NULL,NULL,out));
  assert(mimdlegacy_show(NULL,NULL,out)==n&&!memcmp(out,b,n));
  assert(!mimdlegacy_show(NULL,NULL,out));
 }
 assert(mimd_legacy_accepted==20&&mimd_legacy_consumed==20&&mimd_legacy_full==20);
 assert(legacy_stats_show(NULL,NULL,out)>0&&strstr(out,"pending 0"));
 pthread_t p[4],c;assert(!pthread_create(&c,NULL,consumer,NULL));
 for(unsigned long i=0;i<4;i++)assert(!pthread_create(&p[i],NULL,producer,(void*)i));
 for(unsigned i=0;i<4;i++)assert(!pthread_join(p[i],NULL));
 assert(!pthread_join(c,NULL));assert(atomic_load(&finished)==4);
 assert(mimd_accepted==41280 && mimd_consumed==41280 && !mimd_count && mimd_full>=10);
 assert(stats_show(NULL,NULL,out)>0 && strstr(out,"pending 0"));
 puts("PASS: actual MIMD2 parser and read-consumed FIFO, boundaries, queue full, wraparound, 40000 concurrent events without loss or duplicates");
}
'''
env = env.replace('#include <assert.h>', '#include <assert.h>\n#include <stdbool.h>')
with tempfile.TemporaryDirectory(prefix='mimd-trigger-') as d:
    p=Path(d);(p/'test.c').write_text(env+state+functions+tests)
    subprocess.run([os.environ.get('CC','clang'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
