#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual native ioctl GET function, with deterministic user-copy/lock doubles."""
from pathlib import Path
import importlib.util,os,subprocess,tempfile
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('extract',HERE/'mi_memcg_test.py')
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
s=(a.ROOT/'drivers/block/zram/zram_extm.c').read_text()
uapi='\n'.join(x for x in (a.ROOT/'include/uapi/linux/zram_extm.h').read_text().splitlines() if not x.startswith('#include'))
env=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
typedef uint32_t __u32;typedef uint64_t __u64;
#define __user
#define CAP_SYS_ADMIN 21
#define MAJOR(d) ((d)>>20)
#define MINOR(d) ((d)&((1U<<20)-1))
#define scnprintf snprintf
struct file {};
struct block_device {unsigned bd_dev;};
static bool admin=true,fail_from,index_locked,init_locked,found=true;
static unsigned to_calls,fail_to,set_calls;
static int zram_index_mutex,zram_index_idr;
static int capable(int c){assert(c==CAP_SYS_ADMIN);return admin;}
static int copy_from_user(void*d,const void*s,size_t n){assert(!index_locked&&!init_locked);if(fail_from)return 1;memcpy(d,s,n);return 0;}
static int copy_to_user(void*d,const void*s,size_t n){assert(index_locked&&init_locked);if(++to_calls==fail_to)return 1;memcpy(d,s,n);return 0;}
static void mutex_lock(int*p){assert(p==&zram_index_mutex&&!index_locked);index_locked=true;}
static void mutex_unlock(int*p){assert(p==&zram_index_mutex&&index_locked&&!init_locked);index_locked=false;}
static void down_read(int*p){(void)p;assert(index_locked&&!init_locked);init_locked=true;}
static void up_read(int*p){(void)p;assert(index_locked&&init_locked);init_locked=false;}
'''
structs=r'''
struct zram_extm {struct zram_extm_target *targets;struct zram_extm_bdev info;};
struct zram {struct zram_extm*extm;struct block_device*bdev;int init_lock;};
static struct zram device;
static struct zram*idr_find(int*p,unsigned id){assert(index_locked&&p==&zram_index_idr);return found&&id==2?&device:NULL;}
static int zram_extm_set(struct zram*z,const struct zram_extm_bdev*i,struct zram_extm_target*t){assert(index_locked&&z==&device&&i->dev_id==2&&t);set_calls++;return -EBUSY;}
'''
tests=r'''
struct request {struct zram_extm_bdev info;struct zram_extm_target targets[3];};
static struct request input(void)
{
 struct request r;memset(&r,0xa5,sizeof(r));r.info.dev_id=2;r.info.nr_targets=0;
 r.info.bdev_size=0x1234567890ULL;r.info.key_id=0xdeadbeef;r.info.crypto_info=0xabcdef00;
 return r;
}
static long call(struct request*r,unsigned cmd)
{
 long ret=zram_extm_ioctl(NULL,cmd,(unsigned long)&r->info);
 assert(!index_locked&&!init_locked);return ret;
}
int main(void)
{
 struct zram_extm_target extents[2]={{131072,65536},{327680,32768}};
 struct block_device bdev={.bd_dev=(259<<20)|7};
 struct zram_extm extm={.targets=extents};device.extm=&extm;device.bdev=&bdev;
 extm.info.nr_targets=2;extm.info.crypto_info=0x01020003;extm.info.key_id=51;extm.info.bdev_size=98304;
 for(unsigned n=0;n<=2;n++)for(unsigned flags=0;flags<=3;flags++)if(flags!=2) {
  struct request r=input(),before=r;extm.info.crypto_info=flags|(flags==3?0x01020000:0);r.info.nr_targets=n;
  to_calls=fail_to=0;assert(!call(&r,ZRAM_EXTM_GET_BDEV));
  assert(r.info.nr_targets==2&&r.info.crypto_info==flags&&!strcmp(r.info.bdev_path,"dev=259:7"));
  assert(r.info.key_id==before.info.key_id&&r.info.bdev_size==before.info.bdev_size&&r.info.dev_id==2);
  assert(r.info.reserved1==before.info.reserved1&&r.info.reserved2==before.info.reserved2);
  assert(!memcmp(r.targets,extents,n*sizeof(*extents)));
  assert(!memcmp(r.targets+n,before.targets+n,(3-n)*sizeof(*extents)));
 }
 struct request r=input(),before;r.info.nr_targets=3;before=r;to_calls=0;
 assert(call(&r,ZRAM_EXTM_GET_BDEV)==-EINVAL&&!to_calls&&!memcmp(&r,&before,sizeof(r)));
 r=input();device.extm=NULL;assert(call(&r,ZRAM_EXTM_GET_BDEV)==-EINVAL);device.extm=&extm;
 found=false;assert(call(&r,ZRAM_EXTM_GET_BDEV)==-ENODEV);found=true;
 admin=false;assert(call(&r,ZRAM_EXTM_GET_BDEV)==-EPERM);admin=true;
 assert(call(&r,123)==-ENOTTY);
 fail_from=true;assert(call(&r,ZRAM_EXTM_GET_BDEV)==-EFAULT);fail_from=false;
 for(unsigned fail=1;fail<=2;fail++) {
  r=input();r.info.nr_targets=1;before=r;to_calls=0;fail_to=fail;
  assert(call(&r,ZRAM_EXTM_GET_BDEV)==-EFAULT&&to_calls==fail);
  assert(!memcmp(&r.info,&before.info,sizeof(r.info)));
  if(fail==1)assert(!memcmp(r.targets,before.targets,sizeof(r.targets)));
  else assert(!memcmp(r.targets,extents,sizeof(*extents)));
 }
 fail_to=0;r=input();assert(call(&r,ZRAM_EXTM_SET_BDEV)==-EBUSY&&set_calls==1);
 puts("PASS: actual GET ioctl count0/subsets/excess, crypto low flags, caller field preservation, target bytes, device identity, CAP_SYS_ADMIN/device/header/target faults, lock release and SET dispatch");
}
'''
with tempfile.TemporaryDirectory(prefix='zram-extm-ioctl-') as d:
 p=Path(d);(p/'test.c').write_text(env+uapi+structs+a.function(s,'zram_extm_ioctl')+tests)
 subprocess.run([os.environ.get('HOSTCC','clang'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Wno-unused-parameter','-Wno-gnu-variable-sized-type-not-at-end','-Werror','-fsanitize=address,undefined','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
