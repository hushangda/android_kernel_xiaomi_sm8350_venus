#!/usr/bin/env python3
"""Exercise actual policy size writer and snapshot allocation/retry functions.

The policydb tree walk is an environmental serializer stub using real
put_entry(). No permissions or policy decisions are changed by this test.
"""
import pathlib
import subprocess
import tempfile
from scoped_reclaim_test import function

ROOT = pathlib.Path(__file__).resolve().parents[4]
header = (ROOT / "security/selinux/ss/policydb.h").read_text()
services = (ROOT / "security/selinux/ss/services.c").read_text()
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#define check_add_overflow __builtin_add_overflow
#define check_mul_overflow __builtin_mul_overflow
struct policy_file {char *data;size_t len;};
struct policydb {size_t len;};
struct security_server {struct policydb policydb;int policy_rwlock;};
struct selinux_state {bool initialized;struct security_server *ss;};
static int allocations,fail_alloc,walks,mode,serialized_error;
static size_t image_size;
static void read_lock(int *l){assert(!*l);*l=1;}
static void read_unlock(int *l){assert(*l);*l=0;}
static struct security_server server;
static void *vmalloc_user(size_t n){assert(!server.policy_rwlock);if(fail_alloc)return NULL;allocations++;return calloc(1,n);}
static void vfree(void *p){assert(!server.policy_rwlock);if(p){allocations--;free(p);}}
'''
serializer = r'''
/* Output deliberately exceeds the stale loaded length in policydb->len. */
static int policydb_write(struct policydb *p,struct policy_file *fp) {
 (void)p;assert(server.policy_rwlock);walks++;
 if(serialized_error)return serialized_error;
 if(!fp->data) {
  if(mode==1 && walks==2) image_size+=257; /* concurrent growth once */
  if(mode==2 && !(walks%2)) image_size+=257; /* continuous growth */
  if(mode==3 && walks==2) image_size/=2; /* concurrent shrink */
 }
 unsigned char chunk[67];size_t done=0;
 while(done<image_size){size_t n=image_size-done;if(n>sizeof(chunk))n=sizeof(chunk);
  for(size_t i=0;i<n;i++)chunk[i]=(unsigned char)(done+i);
  int rc=put_entry(chunk,1,n,fp);if(rc)return rc;done+=n;}
 return 0;
}
'''
tests = r'''
static void reset(void){assert(!allocations);server.policydb.len=8;image_size=5000;walks=mode=fail_alloc=serialized_error=0;}
int main(void){
 unsigned char source[16];for(int i=0;i<16;i++)source[i]=i;
 struct policy_file fp={0};assert(!put_entry(source,1,16,&fp) && fp.len==16 && !fp.data);
 assert(!put_entry(source,1,3,&fp) && fp.len==19);
 fp.len=SIZE_MAX-1;assert(put_entry(source,1,2,&fp)==-EOVERFLOW);
 fp.len=0;assert(put_entry(source,SIZE_MAX,2,&fp)==-EOVERFLOW);
 assert(put_entry(source,1,-1,&fp)==-EOVERFLOW);
 unsigned char dest[16]={0};fp=(struct policy_file){(char *)dest,15};assert(put_entry(source,1,16,&fp)==-EINVAL);
 assert(fp.data==(char *)dest && fp.len==15);
 fp.len=16;assert(!put_entry(source,1,16,&fp));assert(!memcmp(source,dest,16) && !fp.len);
 struct selinux_state state={true,&server};void *data;size_t len;
 for(int variant=0;variant<7;variant++){
  reset();mode=variant;data=(void *)1;len=999;
  if(variant==4)fail_alloc=1;if(variant==5)serialized_error=-EINVAL;if(variant==6)state.initialized=false;
  int rc=security_read_policy(&state,&data,&len);
  if(variant==2){assert(rc==-EAGAIN && walks==6);}
  else if(variant==4)assert(rc==-ENOMEM);
  else if(variant>=5)assert(rc==-EINVAL);
  else{assert(!rc && len==image_size && data);for(size_t i=0;i<len;i++)assert(((unsigned char *)data)[i]==(unsigned char)i);vfree(data);}
  if(rc)assert(!data && !len);assert(!allocations && !server.policy_rwlock);state.initialized=true;
 }
 for(size_t size=1;size<10000;size+=13){reset();image_size=size;assert(!security_read_policy(&state,&data,&len));
  assert(len==size);for(size_t i=0;i<len;i++)assert(((unsigned char *)data)[i]==(unsigned char)i);vfree(data);}
 puts("PASS actual SELinux snapshot functions: exact sizing, stale length, grow/shrink retries, overflow, errors, allocation cleanup, byte preservation");
}
'''
with tempfile.TemporaryDirectory(prefix="venus-policy-snapshot-") as temporary:
    path = pathlib.Path(temporary)
    (path / "test.c").write_text(prelude + function(header, "put_entry") + serializer +
                                function(services, "security_read_policy") + tests)
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined", "-no-pie",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
