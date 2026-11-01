#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract native extent and encryption functions; use real OpenSSL AES-XTS.

Block-crypto/keyring are deterministic capability/lifetime doubles. This checks
bounds, mapping, key failures and real XTS round trips, not UFS hardware behavior.
"""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('extract',HERE/'mi_memcg_test.py')
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
source=(a.ROOT/'drivers/block/zram/zram_extm.c').read_text()
uapi='\n'.join(line for line in (a.ROOT/'include/uapi/linux/zram_extm.h').read_text().splitlines() if not line.startswith('#include'))
start=source.index('struct zram_extm {');end=source.index('\n};',start)+3
structure=source[start:end]
env=r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <openssl/evp.h>
typedef uint64_t u64,sector_t,__u64,__le64;
typedef uint32_t u32,__u32;
typedef uint8_t u8;
typedef unsigned int gfp_t;
#define PAGE_SHIFT 12
#define PAGE_SIZE 4096
#define U64_MAX UINT64_MAX
#define GFP_KERNEL 0
#define GFP_NOIO 0
#define KEY_NEED_SEARCH 1
#define FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER 2
#define BLK_CRYPTO_MAX_WRAPPED_KEY_SIZE 128
#define BLK_ENCRYPTION_MODE_AES_256_XTS 1
#define BLK_CRYPTO_DUN_ARRAY_SIZE 4
#define CRYPTO_TFM_REQ_MAY_BACKLOG 1
#define CRYPTO_TFM_REQ_MAY_SLEEP 2
#define ZRAM_EXTM_MAX_TARGETS 16384U
#define ZRAM_EXTM_SEG_SIZE 32768
#define DECLARE_CRYPTO_WAIT(w) int w=0
#define cpu_to_le64(v) (v)
#define WARN_ON_ONCE(v) ((v)?(warnings++,true):false)
#define IS_ERR(p) ((intptr_t)(p)<0 && (intptr_t)(p)>-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
static unsigned warnings,live_allocs,key_refs,key_free,evictions;
static int allocation_error,key_lookup_error,key_valid_error,capability_error,key_init_error,cipher_error,context_error;
static void *kvmalloc_array(size_t n,size_t size,int flags) { (void)flags;if(allocation_error)return NULL;assert(n<=SIZE_MAX/size);void*p=calloc(n,size);assert(p);live_allocs++;return p; }
static void *kzalloc(size_t n,int flags) { return kvmalloc_array(1,n,flags); }
static void kvfree(void*p) { if(p){assert(live_allocs);live_allocs--;free(p);} }
#define kfree kvfree
static void kzfree(void*p) { key_free++;kvfree(p); }
static void sort(void*p,size_t n,size_t size,int(*cmp)(const void*,const void*),void*unused) { (void)unused;qsort(p,n,size,cmp); }
static void memzero_explicit(void*p,size_t n) { memset(p,0,n); }
struct block_device { int unused; };
struct crypto_skcipher { unsigned char raw[64]; };
struct blk_crypto_key { unsigned char raw[128]; bool wrapped; };
struct zram_extm;
struct zram { struct zram_extm *extm; };
struct key_type { const char *name; };
struct key { int sem; struct key_type *type; size_t datalen; struct {void*data[1];} payload; };
struct fscrypt_provisioning_key_payload { u32 type,__reserved; u8 raw[]; };
static struct key testkey;
typedef struct key *key_ref_t;
static key_ref_t lookup_user_key(u32 id,unsigned long flags,int need) { assert(id && !flags && need==1);if(key_lookup_error)return (void*)(intptr_t)key_lookup_error;key_refs++;return &testkey; }
static struct key *key_ref_to_ptr(key_ref_t ref) { return ref; }
static void key_ref_put(key_ref_t ref) { assert(ref==&testkey && key_refs);key_refs--; }
static bool key_locked;
static void down_read(int*sem) { (void)sem;assert(!key_locked);key_locked=true; }
static void up_read(int*sem) { (void)sem;assert(key_locked);key_locked=false; }
static int key_validate(struct key*k) { assert(key_locked && k==&testkey);return key_valid_error; }
static void *bdev_get_queue(struct block_device*d) { return d; }
static int blk_crypto_init_key(struct blk_crypto_key*k,const u8*raw,size_t n,bool wrapped,int mode,int dun,int unit)
{ assert(mode==1 && dun==8 && unit==4096);if(key_init_error)return key_init_error;if((!wrapped && n!=64)||n>128)return -EINVAL;memcpy(k->raw,raw,n);k->wrapped=wrapped;return 0; }
static int blk_crypto_start_using_mode(int mode,int dun,int unit,bool wrapped,void*q)
{ (void)wrapped;assert(mode==1 && dun==8 && unit==4096 && q);return capability_error; }
static int blk_crypto_evict_key(void*q,struct blk_crypto_key*k) { assert(q && k);evictions++;return 0; }
static struct crypto_skcipher *crypto_alloc_skcipher(const char*name,int type,int mask)
{ assert(!strcmp(name,"xts(aes)") && !type && !mask);if(cipher_error)return (void*)(intptr_t)cipher_error;return kzalloc(sizeof(struct crypto_skcipher),0); }
static int crypto_skcipher_setkey(struct crypto_skcipher*c,const u8*raw,size_t n) { assert(n==64);memcpy(c->raw,raw,n);return 0; }
static void crypto_free_skcipher(struct crypto_skcipher*c) { kvfree(c); }
static void get_random_bytes(u8*raw,size_t n) { for(size_t i=0;i<n;i++)raw[i]=(i*7+3)&255; }
struct bio_crypt_ctx { const struct blk_crypto_key*bc_key;u64 bc_dun[4];void*bc_ksm;int bc_keyslot; };
struct bio { struct {sector_t bi_sector;} bi_iter;struct bio_crypt_ctx*bi_crypt_context; };
static struct bio_crypt_ctx *bio_crypt_alloc_ctx(gfp_t gfp) { (void)gfp;if(context_error)return NULL;return kzalloc(sizeof(struct bio_crypt_ctx),0); }
struct page { unsigned char data[4096]; };
struct scatterlist { struct page*page;size_t length,offset; };
struct skcipher_request { struct crypto_skcipher*cipher;struct scatterlist*src,*dst;size_t len;unsigned char iv[16]; };
static struct skcipher_request*skcipher_request_alloc(struct crypto_skcipher*c,int gfp) { (void)gfp;struct skcipher_request*r=kzalloc(sizeof(*r),0);if(r)r->cipher=c;return r; }
static void sg_init_table(struct scatterlist*sg,int n) { assert(n==1);memset(sg,0,sizeof(*sg)); }
static void sg_set_page(struct scatterlist*sg,struct page*p,size_t n,size_t offset) { sg->page=p;sg->length=n;sg->offset=offset; }
static void crypto_req_done(void) { }
static void skcipher_request_set_callback(struct skcipher_request*r,int flags,void(*cb)(void),int*wait) { (void)r;(void)flags;(void)cb;(void)wait; }
static void skcipher_request_set_crypt(struct skcipher_request*r,struct scatterlist*src,struct scatterlist*dst,size_t n,u8*iv) { r->src=src;r->dst=dst;r->len=n;memcpy(r->iv,iv,16); }
static int cipher_operation(struct skcipher_request*r,bool encrypt)
{
 EVP_CIPHER_CTX*ctx=EVP_CIPHER_CTX_new();int n=0,tail=0;assert(ctx);
 assert(EVP_CipherInit_ex(ctx,EVP_aes_256_xts(),NULL,r->cipher->raw,r->iv,encrypt));
 assert(EVP_CipherUpdate(ctx,r->dst->page->data,&n,r->src->page->data,r->len));
 assert(EVP_CipherFinal_ex(ctx,r->dst->page->data+n,&tail));assert((size_t)(n+tail)==r->len);
 EVP_CIPHER_CTX_free(ctx);return 0;
}
static int crypto_skcipher_encrypt(struct skcipher_request*r) { return cipher_operation(r,true); }
static int crypto_skcipher_decrypt(struct skcipher_request*r) { return cipher_operation(r,false); }
static int crypto_wait_req(int ret,int*wait) { (void)wait;return ret; }
static void skcipher_request_free(struct skcipher_request*r) { kvfree(r); }
'''
functions=''.join(a.function(source,n) for n in ['zram_extm_compare','zram_extm_validate','zram_extm_sector','zram_extm_free','zram_extm_key','zram_extm_crypto_init','zram_extm_prepare_bio','zram_extm_crypt'])
tests=r'''
static struct zram_extm *fixture(void)
{
 struct zram_extm*x=kzalloc(sizeof(*x),0);x->info.nr_targets=2;x->info.bdev_size=98304;
 x->targets=kvmalloc_array(2,sizeof(*x->targets),0);x->ends=kvmalloc_array(2,sizeof(*x->ends),0);
 x->targets[0]=(struct zram_extm_target){524288,65536};x->targets[1]=(struct zram_extm_target){131072,32768};return x;
}
int main(void)
{
 _Static_assert(sizeof(struct zram_extm_bdev)==160,"native header");
 _Static_assert(offsetof(struct zram_extm_bdev,targets)==160,"native extent offset");
 _Static_assert(ZRAM_EXTM_SET_BDEV==0x40a05c01,"native ioctl");
 _Static_assert(ZRAM_EXTM_GET_BDEV==0xc0a05c02,"native ioctl");
 struct zram_extm*x=fixture();struct zram z={x};struct block_device device={0};
 assert(!zram_extm_validate(x,1048576));
 for(unsigned int i=0;i<24;i++)assert(zram_extm_sector(&z,i)==(i<16?(524288+4096*i):(131072+4096*(i-16)))/512);
 for(int fail=0;fail<7;fail++) {
  u64 p=x->targets[1].physical,n=x->targets[1].length,total=x->info.bdev_size;
  if(fail==0)x->targets[1].physical=524288;
  if(fail==1)x->targets[1].physical=1;
  if(fail==2)x->targets[1].length=1;
  if(fail==3)x->targets[1].length=0;
  if(fail==4)x->targets[1].physical=1048576;
  if(fail==5)x->targets[1].length=UINT64_MAX-32767;
  if(fail==6)x->info.bdev_size+=32768;
  assert(zram_extm_validate(x,1048576)==-EINVAL);
  x->targets[1].physical=p;x->targets[1].length=n;x->info.bdev_size=total;
 }
 allocation_error=1;assert(zram_extm_validate(x,1048576)==-ENOMEM);allocation_error=0;
 assert(!zram_extm_validate(x,1048576));
 struct page plain,encrypted,other;for(unsigned i=0;i<PAGE_SIZE;i++)plain.data[i]=(i*13+5)&255;
 x->info.crypto_info=1;assert(!zram_extm_crypto_init(x,&device));
 for(unsigned int i=0;i<24;i++) {
  encrypted=plain;assert(!zram_extm_crypt(&z,&encrypted,i,true));assert(memcmp(encrypted.data,plain.data,PAGE_SIZE));
  other=plain;assert(!zram_extm_crypt(&z,&other,(i+1)%24,true));assert(memcmp(encrypted.data,other.data,PAGE_SIZE));
  assert(!zram_extm_crypt(&z,&encrypted,i,false));assert(!memcmp(encrypted.data,plain.data,PAGE_SIZE));
 }
 zram_extm_free(x,&device);
 struct {u32 type,reserved;u8 raw[128];} payload={.type=2};struct key_type type={"fscrypt-provisioning"};
 get_random_bytes(payload.raw,128);testkey.type=&type;testkey.payload.data[0]=&payload;testkey.datalen=72;
 for(int wrapped=0;wrapped<2;wrapped++)for(int fail=0;fail<9;fail++) {
  x=fixture();z.extm=x;x->info.crypto_info=3U|(1U<<24)|((wrapped?2U:1U)<<16);x->info.key_id=10;
  key_lookup_error=key_valid_error=key_init_error=capability_error=0;payload.type=2;payload.reserved=0;testkey.datalen=wrapped?136:72;
  if(fail==1)key_lookup_error=-ENOKEY;
  if(fail==2)key_valid_error=-EKEYREVOKED;
  if(fail==3)payload.type=1;
  if(fail==4)testkey.datalen=137;
  if(fail==5)payload.reserved=1;
  if(fail==6)key_init_error=-EINVAL;
  if(fail==7)capability_error=-EOPNOTSUPP;
  if(fail==8)testkey.datalen=7;
  int ret=zram_extm_crypto_init(x,&device);assert((ret!=0)==(fail!=0));assert(!key_refs && !key_locked && !x->cipher);
  if(!fail) {
   assert(x->key && x->key->wrapped==(bool)wrapped);
   assert(!zram_extm_validate(x,1048576));struct bio bio={0};
   assert(!zram_extm_prepare_bio(&z,&bio,17,0));assert(bio.bi_iter.bi_sector==(131072+4096)/512);
   assert(bio.bi_crypt_context->bc_key==x->key && bio.bi_crypt_context->bc_dun[0]==33);
   kvfree(bio.bi_crypt_context);bio.bi_crypt_context=NULL;context_error=1;
   assert(zram_extm_prepare_bio(&z,&bio,17,0)==-ENOMEM && !bio.bi_crypt_context);context_error=0;
  }
  zram_extm_free(x,&device);
 }
 assert(evictions==2 && !live_allocs && !warnings);
 puts("PASS: native 160-byte ioctl ABI, extent bounds/alignment/overlap/overflow/map, keyring and inline capability failures, key cleanup, OpenSSL AES-256-XTS 24 physical-DUN roundtrips");
}
'''
with tempfile.TemporaryDirectory(prefix='venus-extm-') as tmp:
    c=Path(tmp)/'test.c';binary=Path(tmp)/'test'
    c.write_text(env+uapi+'\n'+structure+'\n'+functions+tests)
    subprocess.run([os.environ.get('HOSTCC','clang'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-no-pie',str(c),'-lcrypto','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
