#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual queued/synchronous ZRAM fault code: lifetime, completion, failure paths."""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('extract',HERE/'mi_memcg_test.py')
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
s=(a.ROOT/'drivers/block/zram/zram_group_io.c').read_text()
start=s.index('struct zram_group_read_work {');end=s.index('\n};',start)+3
structure=s[start:end]
env=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GFP_ATOMIC 0
#define REQ_OP_READ 0
#define PAGE_SIZE 4096
#define atomic64_inc(p) (++*(p))
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define errno_to_blk_status(e) (e)
struct page { unsigned char data[PAGE_SIZE]; };
struct bio { int bi_status,bi_opf;struct bio*parent;void(*bi_end_io)(struct bio*); };
struct bio_vec { struct page*bv_page;unsigned bv_len,bv_offset; };
struct work_struct { void(*fn)(struct work_struct*); };
struct zram { struct {unsigned bd_reads;} stats;void*group_read_wq;unsigned refs; };
static struct work_struct *pending;
static unsigned allocations,completed,parents,freed,stack_works;
static int fail,io_error;
static void*kmalloc(size_t n,int flags) { (void)flags;if(fail==1)return NULL;void*p=calloc(1,n);assert(p);allocations++;return p; }
static void kfree(void*p) { assert(allocations);allocations--;free(p); }
static struct bio*bio_alloc(int flags,unsigned n) { (void)flags;assert(n==1);if(fail==2)return NULL;return kmalloc(sizeof(struct bio),0); }
static void bio_put(struct bio*b) { kfree(b); }
static unsigned bio_add_page(struct bio*b,struct page*p,unsigned len,unsigned off) { (void)b;assert(p&&len==PAGE_SIZE&&!off);return fail==3?0:len; }
static void bio_chain(struct bio*b,struct bio*p) { assert(p);b->parent=p; }
static void zram_page_end_io(struct bio*b) { completed++;assert(b->bi_status==io_error);bio_put(b); }
static void bio_endio(struct bio*b) { if(b->parent){parents++;b->parent->bi_status=b->bi_status;bio_put(b);}else b->bi_end_io(b); }
#define INIT_WORK(p,f) ((p)->fn=(f))
#define INIT_WORK_ONSTACK(p,f) do { (p)->fn=(f);stack_works++; } while(0)
static void queue_work(void*q,struct work_struct*w) { assert(q&&!pending);pending=w; }
static void flush_work(struct work_struct*w) { assert(pending==w);pending=NULL;w->fn(w); }
static void destroy_work_on_stack(struct work_struct*w) { (void)w;assert(stack_works);stack_works--; }
static void free_block_bdev(struct zram*z,unsigned long block) { assert(block==5&&z->refs);if(!--z->refs)freed++; }
static int zram_group_bio(struct zram*z,struct page*p,unsigned long block,bool write) { assert(!write&&z->refs&&block==5);if(!io_error)memset(p->data,0x5a,PAGE_SIZE);return io_error; }
'''
functions=''.join(a.function(s,n) for n in ['zram_group_read_workfn','zram_group_read_async'])
tests=r'''
int main(void)
{
 struct zram z={.group_read_wq=&z};struct page page;struct bio_vec v={&page,PAGE_SIZE,0};struct bio parent;
 for(unsigned sync=0;sync<2;sync++)for(unsigned has_parent=0;has_parent<2;has_parent++)for(unsigned error=0;error<2;error++) {
  z.refs=2;memset(&page,0,sizeof(page));memset(&parent,0,sizeof(parent));io_error=error?-EIO:0;fail=0;
  unsigned c=completed,p=parents,f=freed;
  int ret=zram_group_read_async(&z,&v,5,has_parent?&parent:NULL,sync);
  if(sync) {assert(ret==io_error && !pending && z.refs==1 && completed==c && parents==p);free_block_bdev(&z,5);}
  else {
   assert(ret==1 && pending && z.refs==2 && page.data[0]==0);
   /* Discard/reset cannot recycle a backing page while the read is queued. */
   free_block_bdev(&z,5);assert(z.refs==1 && freed==f);
   flush_work(pending);assert(!z.refs);
   assert(completed==c+!has_parent && parents==p+has_parent);
   if(has_parent)assert(parent.bi_status==io_error);
  }
  assert(freed==f+1 && !allocations && !stack_works && !pending);
  assert(page.data[0]==(error?0:0x5a));
 }
 for(fail=1;fail<=3;fail++) {
  z.refs=1;unsigned c=completed,p=parents;
  assert(zram_group_read_async(&z,&v,5,&parent,false)==-ENOMEM);
  assert(!z.refs && !pending && !allocations && completed==c && parents==p);
 }
 puts("PASS: actual fault work lifetime and pins, async single bio completion, partial-page synchronous success/error, three allocation failures, drain-before-recycle");
}
'''
with tempfile.TemporaryDirectory(prefix='zram-fault-') as d:
    p=Path(d);(p/'test.c').write_text(env+structure+functions+tests)
    subprocess.run([os.environ.get('HOSTCC','clang'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
