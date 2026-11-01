#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual bounded MIMD log and dump notification state functions."""
from pathlib import Path
import importlib.util,os,subprocess,tempfile
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('extract',HERE/'mi_memcg_test.py')
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
s=(a.ROOT/'drivers/misc/perf_helper_mimd.c').read_text()
state=s[s.index('static s64 mimd_dump_value'):s.index('static ssize_t mimdlog_write')]
env=r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
typedef long long s64;
#define __user
#define DEFINE_MUTEX(n) pthread_mutex_t n=PTHREAD_MUTEX_INITIALIZER
#define DEFINE_SPINLOCK(n) DEFINE_MUTEX(n)
#define mutex_lock(p) pthread_mutex_lock(p)
#define mutex_unlock(p) pthread_mutex_unlock(p)
#define spin_lock(p) pthread_mutex_lock(p)
#define spin_unlock(p) pthread_mutex_unlock(p)
#define PAGE_SIZE 4096
#define scnprintf snprintf
struct file {};struct proc_dir_entry {};struct kobject {};struct kobj_attribute {};
struct timespec64 {long tv_sec,tv_nsec;};
struct rtc_time {int tm_year,tm_mon,tm_mday,tm_hour,tm_min,tm_sec;};
struct seq_file {char data[400000];size_t length;};
static DEFINE_SPINLOCK(mimd_lock);
static bool copy_fail;
static int copy_from_user(void*d,const void*s,size_t n){if(copy_fail)return 1;memcpy(d,s,n);return 0;}
static void ktime_get_real_ts64(struct timespec64*t){t->tv_sec=12345;}
static void rtc_time64_to_tm(long t,struct rtc_time*r){assert(t==12345);memset(r,0,sizeof(*r));r->tm_year=126;r->tm_mday=1;}
static void seq_printf(struct seq_file*s,const char*f,...){va_list a;va_start(a,f);int n=vsnprintf(s->data+s->length,sizeof(s->data)-s->length,f,a);va_end(a);assert(n>0&&(size_t)n<sizeof(s->data)-s->length);s->length+=n;}
static int kstrtoll(const char*s,int base,s64*v){char*end;errno=0;long long n=strtoll(s,&end,base);if(errno==ERANGE)return -ERANGE;if(end==s)return -EINVAL;if(*end=='\n')end++;if(*end)return -EINVAL;*v=n;return 0;}
'''
functions=''.join(a.function(s,n) for n in ['mimdlog_write','mimdlog_show','dump_set_show','dump_set_store'])
tests=r'''
int main(void)
{
 char b[4096];static struct seq_file out;
 assert(mimdlog_write(NULL,"",0,NULL)==-EINVAL);
 memset(b,'x',128);assert(mimdlog_write(NULL,b,128,NULL)==-EINVAL);
 assert(mimdlog_write(NULL,"a\0b",3,NULL)==-EINVAL);
 copy_fail=true;assert(mimdlog_write(NULL,"a",1,NULL)==-EFAULT);copy_fail=false;
 assert(!mimd_log_count);
 assert(mimdlog_write(NULL,b,127,NULL)==127&&!mimd_log[0].text[127]);
 for(unsigned i=0;i<2050;i++){int n=snprintf(b,sizeof(b),"event-%04u",i);assert(mimdlog_write(NULL,b,n,NULL)==n);}
 assert(mimd_log_count==2048&&mimd_log_head==3);
 assert(!mimdlog_show(&out,NULL));
 assert(strstr(out.data,"event-0002")&&strstr(out.data,"event-2049")&&!strstr(out.data,"event-0000")&&!strstr(out.data,"event-0001"));
 assert(strstr(out.data,"event-0002")<strstr(out.data,"event-2049"));
 const char*values[]={"0","-1","9223372036854775807","-9223372036854775808"};
 for(unsigned i=0;i<4;i++) {assert(dump_set_store(NULL,NULL,values[i],strlen(values[i]))==(ssize_t)strlen(values[i]));assert(dump_set_show(NULL,NULL,b)>0);assert(strtoll(b,NULL,10)==strtoll(values[i],NULL,10));}
 assert(dump_set_store(NULL,NULL,"9223372036854775808",19)==-ERANGE);
 assert(dump_set_store(NULL,NULL,"1x",2)==-EINVAL);
 puts("PASS: actual MIMD log boundaries, copy faults, 2048-record wrap chronology, no unterminated text, signed dump command roundtrips and errors");
}
'''
with tempfile.TemporaryDirectory(prefix='mimd-diag-') as d:
 p=Path(d);(p/'test.c').write_text(env+state+functions+tests)
 subprocess.run([os.environ.get('HOSTCC','clang'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-variable','-Werror','-fsanitize=address,undefined','-no-pie','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
