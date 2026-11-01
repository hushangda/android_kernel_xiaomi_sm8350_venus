#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual group IO/normal compression commit paths with failing/racing block IO.

The device, codec and allocator are test doubles. Control flow, quota, block
references, compare-and-commit, accounting and CSS ownership are kernel source.
"""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('account_fixture', HERE / 'mi_memcg_test.py')
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)
source = (a.ROOT / 'drivers/block/zram/zram_drv.c').read_text()
io = (a.ROOT / 'drivers/block/zram/zram_group_io.c').read_text()
objects = a.objects[:a.objects.index('enum { ZRAM_WB')]
objects = objects.replace('struct page { struct mem_cgroup *memcg; bool swapcache; };',
    'struct page { struct mem_cgroup *memcg; bool swapcache; unsigned char data[4096]; };')
functions = a.functions[:a.functions.index('static void zram_memcg_clear')]
env = r'''
#include <stdlib.h>
#define CONFIG_ZRAM_WRITEBACK 1
#define PAGE_SHIFT 12
#define ZRAM_PRIMARY_COMP 0
#define ZS_MM_WO 0
#define __GFP_KSWAPD_RECLAIM 0
#define __GFP_NOWARN 0
#define __GFP_HIGHMEM 0
#define __GFP_MOVABLE 0
#define __GFP_CMA 0
#define __GFP_OFFLINABLE 0
#define GFP_NOIO 0
#define unlikely(x) (x)
#define pr_err(...) ((void)0)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
typedef atomic_int atomic_t;
typedef atomic_llong atomic64_t;
#define atomic_set(p,v) atomic_store(p,v)
#define atomic_inc(p) atomic_fetch_add(p,1)
#define atomic_dec_and_test(p) (atomic_fetch_sub(p,1)==1)
#define atomic64_inc(p) atomic_fetch_add(p,1)
#define atomic64_dec(p) atomic_fetch_sub(p,1)
#define atomic64_add(n,p) atomic_fetch_add(p,n)
#define atomic64_sub(n,p) atomic_fetch_sub(p,n)
enum zram_pageflags { ZRAM_WB, ZRAM_SAME, ZRAM_UNDER_WB, ZRAM_IDLE, ZRAM_HUGE };
struct zram_wb_token { unsigned long block; };
struct zram_table_entry { struct mem_cgroup *memcg; u32 bytes, flags; unsigned long handle; };
struct zcomp { int unused; };
struct zcomp_strm { unsigned char buffer[4096]; };
struct bio { int unused; };
struct bio_vec { struct page *bv_page; unsigned int bv_len, bv_offset; };
struct zram_stats { atomic64_t bd_count,bd_reads,bd_writes,same_pages,compr_data_size,huge_pages,pages_stored,writestall; };
struct zram {
 struct zram_table_entry table[4]; struct zram_stats stats;
 bool bitmap[9]; atomic_t backing_refs[9]; unsigned long nr_pages;
 spinlock_t wb_limit_lock; bool wb_limit_enable; u64 bd_wb_limit;
 bool group_enabled; unsigned long limit_pages; void *mem_pool; struct zcomp *comps[1];
};
static bool locked;
static unsigned int allocs, comp_size=400;
static int comp_error;
static bool same_page;
static size_t huge_class_size = 4096;
static unsigned char media[9][4096];
static void zram_slot_lock(struct zram *z, u32 i) { (void)z;(void)i;assert(!locked);locked=true; }
static void zram_slot_unlock(struct zram *z,u32 i) { (void)z;(void)i;assert(locked);locked=false; }
static bool zram_test_flag(struct zram *z,u32 i,enum zram_pageflags f) { return z->table[i].flags & (1U<<f); }
static void zram_set_flag(struct zram *z,u32 i,enum zram_pageflags f) { assert(locked);z->table[i].flags |= 1U<<f; }
static void zram_clear_flag(struct zram *z,u32 i,enum zram_pageflags f) { assert(locked);z->table[i].flags &= ~(1U<<f); }
static u32 zram_get_obj_size(struct zram *z,u32 i) { return z->table[i].bytes; }
static void zram_set_obj_size(struct zram *z,u32 i,u32 b) { z->table[i].bytes=b; }
static void zram_set_handle(struct zram *z,u32 i,unsigned long h) { z->table[i].handle=h; }
static unsigned long zram_get_element(struct zram *z,u32 i) { return z->table[i].handle; }
static void zram_set_element(struct zram *z,u32 i,unsigned long h) { z->table[i].handle=h; }
static bool zram_allocated(struct zram *z,u32 i) { return z->table[i].bytes || zram_test_flag(z,i,ZRAM_WB) || zram_test_flag(z,i,ZRAM_SAME); }
static unsigned long find_next_zero_bit(bool *bits,unsigned long n,unsigned long i) { while(i<n && bits[i]) i++;return i; }
static bool test_and_set_bit(unsigned long i,bool *bits) { bool old=bits[i];bits[i]=true;return old; }
static bool test_and_clear_bit(unsigned long i,bool *bits) { bool old=bits[i];bits[i]=false;return old; }
static bool mem_cgroup_is_descendant(struct mem_cgroup *m,struct mem_cgroup *parent)
{ while(m) { if(m==parent)return true; m=parent_mem_cgroup(m); } return false; }
static void *kmap_atomic(struct page *p) { return p->data; }
static void kunmap_atomic(void *p) { (void)p; }
static bool page_same_filled(void *p,unsigned long *element) { (void)p;*element=0;return same_page; }
static struct zcomp_strm stream;
static struct zcomp_strm *zcomp_stream_get(struct zcomp *c) { (void)c;return &stream; }
static void zcomp_stream_put(struct zcomp *c) { (void)c; }
static int zcomp_compress(struct zcomp_strm *s,void *p,unsigned int *len)
{ memcpy(s->buffer,p,PAGE_SIZE);*len=comp_size;return comp_error; }
static unsigned long zs_malloc(void *pool,unsigned int size,int flags)
{ (void)pool;(void)flags;void *p=malloc(size);assert(p);allocs++;return (unsigned long)p; }
static void zs_free(void *pool,unsigned long h) { (void)pool;if(h){assert(allocs);allocs--;free((void*)h);} }
static unsigned long zs_get_total_pages(void *p) { (void)p;return allocs; }
static void update_used_max(struct zram *z,unsigned long p) { (void)z;(void)p; }
static void *zs_map_object(void *p,unsigned long h,int mode) { (void)p;(void)mode;return (void*)h; }
static void zs_unmap_object(void *p,unsigned long h) { (void)p;(void)h; }
static int zram_read_from_zspool(struct zram *z,struct page *p,u32 i)
{ assert(locked);memset(p->data,*(unsigned char*)z->table[i].handle,PAGE_SIZE);return 0; }
'''
helpers = ''.join(a.function(source, n) for n in [
    'zram_memcg_clear','zram_memcg_preserve','zram_memcg_restore',
    'alloc_block_bdev','free_block_bdev'])
free_page = r'''
static void zram_free_page(struct zram *z,size_t i)
{
 assert(locked);
 bool present=zram_allocated(z,i);
 zram_memcg_clear(z,i);
 if(zram_test_flag(z,i,ZRAM_WB)) free_block_bdev(z,zram_get_element(z,i));
 else if(zram_test_flag(z,i,ZRAM_SAME)) atomic64_dec(&z->stats.same_pages);
 else if(z->table[i].handle) {
  zs_free(z->mem_pool,z->table[i].handle);
  atomic64_sub(z->table[i].bytes,&z->stats.compr_data_size);
 }
 if(present) atomic64_dec(&z->stats.pages_stored);
 z->table[i].handle=0;z->table[i].bytes=0;
 z->table[i].flags &= 1U<<ZRAM_UNDER_WB;
}
'''
write = a.function(source, '__zram_bvec_write')
faults = r'''
enum { CLEAN, IO_FAIL, ACCESS, DISCARD, REPLACE };
static int io_action;
static struct page original;
static struct memcg_fixture owner_fixture, alien_fixture;
static int zram_group_bio(struct zram *z,struct page *p,unsigned long block,bool writing)
{
 assert(!locked); assert(block>0 && block<z->nr_pages && z->bitmap[block]);
 if(io_action==IO_FAIL)return -EIO;
 if(writing)memcpy(media[block],p->data,PAGE_SIZE);else memcpy(p->data,media[block],PAGE_SIZE);
 if(io_action==ACCESS) { zram_slot_lock(z,0); zram_clear_flag(z,0,ZRAM_IDLE); zram_slot_unlock(z,0); }
 if(io_action==DISCARD || io_action==REPLACE) {
  zram_slot_lock(z,0);zram_free_page(z,0);zram_slot_unlock(z,0);
  if(!writing) { /* Slot was discarded while its storage IO is pinned. */
   assert(z->bitmap[block] && atomic_load(&z->backing_refs[block])==1);
   unsigned long newblock=alloc_block_bdev(z);assert(newblock && newblock!=block);free_block_bdev(z,newblock);
  }
  if(io_action==REPLACE) {
   struct bio_vec v={.bv_page=&original,.bv_len=4096};
   struct mem_cgroup *saved=original.memcg;original.memcg=&alien_fixture.m;
   assert(!__zram_bvec_write(z,&v,0,NULL,&original,NULL));original.memcg=saved;
  }
 }
 return 0;
}
'''
io_functions=''.join(a.function(io,n) for n in ['zram_group_matches','zram_group_reserve_quota',
    'zram_group_release_quota','zram_group_ratio_allows','zram_group_write_slot','zram_group_read_slot'])
tests = r'''
static void check_owner(u64 cp,u64 cb,u64 wp,u64 wb,unsigned int refs)
{
 struct mi_memcg_stats s;mi_memcg_stats(&owner_fixture.m,&s);
 assert(s.compressed_pages==cp && s.compressed_bytes==cb && s.writeback_pages==wp && s.writeback_bytes==wb);
 assert(atomic_load(&owner_fixture.m.css.refs)==refs);
}
static void clear_slot(struct zram *z) { zram_slot_lock(z,0);zram_free_page(z,0);zram_slot_unlock(z,0); }
static void store(struct zram *z)
{
 struct bio_vec v={.bv_page=&original,.bv_len=4096};
 assert(!__zram_bvec_write(z,&v,0,NULL,&original,NULL));
 check_owner(1,400,0,0,2);
}
int main(void)
{
 struct zram z={.nr_pages=9,.group_enabled=true};struct page scratch={0};u64 done;
 spin_lock_init(&z.wb_limit_lock);
 atomic_init(&owner_fixture.m.css.refs,1);mi_memcg_init(&owner_fixture.m);mi_memcg_online(&owner_fixture.m);
 atomic_init(&alien_fixture.m.css.refs,1);mi_memcg_init(&alien_fixture.m);mi_memcg_online(&alien_fixture.m);
 original.memcg=&owner_fixture.m;original.swapcache=true;memset(original.data,0x5a,PAGE_SIZE);
 /* Wide oracle for the physical-byte budget after replacing one compressed slot. */
 for(unsigned i=0;i<10000;i++) {
  u64 c=((u64)rand()<<33)^((u64)rand()<<2),w=((u64)rand()<<33)^((u64)rand()<<2);
  unsigned bytes=rand()%4097,ratio=rand()%101;
  owner_fixture.data.stats.compressed_bytes=c;owner_fixture.data.stats.writeback_bytes=w;
  __uint128_t nw=(__uint128_t)w+PAGE_SIZE,total=(__uint128_t)c-bytes+nw;
  bool expected=ratio==100 || (c>=bytes && nw<=U64_MAX && total<=U64_MAX && nw<=total*ratio/100);
  assert(zram_group_ratio_allows(&owner_fixture.m,bytes,ratio)==expected);
 }
 memset(&owner_fixture.data.stats,0,sizeof(owner_fixture.data.stats));
 for(int round=0;round<100;round++) {
  store(&z);io_action=CLEAN;
  assert(!zram_group_write_slot(&z,&alien_fixture.m,0,&scratch,&done,100) && !done);
  check_owner(1,400,0,0,2);
  z.wb_limit_enable=true;z.bd_wb_limit=0;
  assert(zram_group_write_slot(&z,&owner_fixture.m,0,&scratch,&done,100)==-ENOSPC);
  assert(!zram_test_flag(&z,0,ZRAM_UNDER_WB));check_owner(1,400,0,0,2);
  z.bd_wb_limit=10;io_action=IO_FAIL;
  assert(zram_group_write_slot(&z,&owner_fixture.m,0,&scratch,&done,100)==-EIO);
  assert(z.bd_wb_limit==10 && !atomic_load(&z.stats.bd_count));check_owner(1,400,0,0,2);
  io_action=CLEAN;
  assert(!zram_group_write_slot(&z,&owner_fixture.m,0,&scratch,&done,100) && done==400);
  check_owner(0,0,1,PAGE_SIZE,2);assert(z.bd_wb_limit==9 && atomic_load(&z.stats.bd_count)==1);
  io_action=IO_FAIL;assert(zram_group_read_slot(&z,&owner_fixture.m,0,&scratch,&done)==-EIO);
  check_owner(0,0,1,PAGE_SIZE,2);assert(atomic_load(&z.stats.bd_count)==1);
  io_action=CLEAN;comp_error=-EINVAL;
  assert(zram_group_read_slot(&z,&owner_fixture.m,0,&scratch,&done)==-EINVAL);
  check_owner(0,0,1,PAGE_SIZE,2);comp_error=0;
  assert(!zram_group_read_slot(&z,&owner_fixture.m,0,&scratch,&done) && done==PAGE_SIZE);
  check_owner(1,400,0,0,2);assert(!atomic_load(&z.stats.bd_count));assert(*(unsigned char*)z.table[0].handle==0x5a);
  clear_slot(&z);check_owner(0,0,0,0,1);
  for(int action=ACCESS;action<=REPLACE;action++) {
   store(&z);io_action=action;
   assert(!zram_group_write_slot(&z,&owner_fixture.m,0,&scratch,&done,100) && !done);
   assert(!atomic_load(&z.stats.bd_count));clear_slot(&z);check_owner(0,0,0,0,1);
   store(&z);io_action=CLEAN;
   assert(!zram_group_write_slot(&z,&owner_fixture.m,0,&scratch,&done,100));
   io_action=action;
   assert(!zram_group_read_slot(&z,&owner_fixture.m,0,&scratch,&done) && !done);
   clear_slot(&z);check_owner(0,0,0,0,1);assert(!atomic_load(&z.stats.bd_count));
  }
  assert(!allocs && atomic_load(&alien_fixture.m.css.refs)==1);
  assert(!atomic_load(&z.stats.pages_stored) && !atomic_load(&z.stats.compr_data_size));
 }
 assert(!warnings && !locked);
 puts("PASS: actual slot IO + compression commit: 100 cycles of write/read/errors/quota/foreign-owner/access/discard/ABA replacement; block pins prevent reuse; no leaked objects/CSS/counters");
}
'''
with tempfile.TemporaryDirectory(prefix='venus-group-io-') as tmp:
    c=Path(tmp)/'test.c';binary=Path(tmp)/'test'
    c.write_text(a.env + a.header + '\n' + objects + env + functions + helpers + free_page + write + faults + io_functions + tests)
    subprocess.run([os.environ.get('HOSTCC','clang'),'-std=gnu11','-O1','-g',
        '-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-function','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-no-pie','-pthread',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
