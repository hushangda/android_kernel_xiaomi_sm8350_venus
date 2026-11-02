#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real ExtM quota ABI and ZRAM budget with lock doubles."""
import importlib.util, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[4]
spec=importlib.util.spec_from_file_location('fixture',root/'tools/testing/selftests/venus/mi_memcg_test.py')
f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)
s=(root/'drivers/block/zram/zram_group_io.c').read_text()
env=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t u64;
#define U64_MAX UINT64_MAX
#define PAGE_SHIFT 12
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
struct zram {int init_lock,wb_limit_lock;bool wb_limit_enable;u64 bd_wb_limit;};
static struct zram dev;
static int zram_index_mutex,zram_index_idr;
static bool registry,init,spin,present=true,exclusive;
static void mutex_lock(int*p){(void)p;assert(!registry&&!init&&!spin);registry=true;}
static void mutex_unlock(int*p){(void)p;assert(registry&&!init&&!spin);registry=false;}
static struct zram*idr_find(int*p,int id){assert(p==&zram_index_idr&&id==0&&registry);return present?&dev:0;}
static void down_read(int*p){assert(p==&dev.init_lock&&registry&&!init);init=true;}
static void up_read(int*p){(void)p;assert(init&&!spin&&!exclusive);init=false;}
static void down_write(int*p){down_read(p);exclusive=true;}
static void up_write(int*p){exclusive=false;up_read(p);}
static void spin_lock(int*p){assert(p==&dev.wb_limit_lock&&!spin);spin=true;}
static void spin_unlock(int*p){(void)p;assert(spin);spin=false;}
'''
body='\n'.join(f.function(s,n) for n in ['mi_zram_quota_read','mi_zram_quota_write','zram_group_reserve_quota','zram_group_release_quota'])
tests=r'''
int main(void){
 assert(mi_zram_quota_read()==0);
 present=false;assert(mi_zram_quota_write(4096)==-ENODEV);assert(mi_zram_quota_read()==0);present=true;
 assert(!mi_zram_quota_write(8192));assert(mi_zram_quota_read()==8192&&dev.wb_limit_enable);
 assert(zram_group_reserve_quota(&dev)==1);assert(mi_zram_quota_read()==4096);
 assert(zram_group_reserve_quota(&dev)==1);assert(mi_zram_quota_read()==0);
 assert(zram_group_reserve_quota(&dev)==-ENOSPC);
 zram_group_release_quota(&dev,1);assert(mi_zram_quota_read()==4096);
 assert(!mi_zram_quota_write(0));assert(zram_group_reserve_quota(&dev)==-ENOSPC);
 assert(!mi_zram_quota_write(4095));assert(mi_zram_quota_read()==0);
 assert(!mi_zram_quota_write(4097));assert(mi_zram_quota_read()==4096);
 assert(!mi_zram_quota_write(1835008ULL*4096));assert(mi_zram_quota_read()==7516192768ULL);
 dev.bd_wb_limit=UINT64_MAX;assert(mi_zram_quota_read()==(UINT64_MAX>>12<<12));
 assert(!mi_zram_quota_write(UINT64_MAX));assert(mi_zram_quota_read()==(UINT64_MAX>>12<<12));
 puts("PASS: native quota callbacks, units, zero/exhaustion, real reservation/refund, reset, overflow, missing device, lock order");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(env+body+tests)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-o',str(p/'test'),str(p/'test.c')],check=True)
 subprocess.run([str(p/'test')],check=True)
