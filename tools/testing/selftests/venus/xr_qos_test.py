#!/usr/bin/env python3
"""Run the actual bounded XR graph functions against a path-enumeration oracle.
Scheduler, object lifetime and allocator stubs are host-only; this is not boot
or lockdep proof. ASan/UBSan catch adjacency/refcount and rollback mistakes.
"""
import os, pathlib, re, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[4]
src=(ROOT/'kernel/sched/xr_qos.c').read_text()
def function(name):
    m=re.search(r'(?m)^(?:static )?[\w *]+\b'+name+r'\([^;]*?\)\s*\{',src)
    assert m,name
    start=src.index('{',m.start());depth=1;end=start+1
    while depth:
        depth+=(src[end]=='{')-(src[end]=='}');end+=1
    return src[m.start():end]
prelude=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
typedef unsigned int kuid_t;
#define uid_eq(a,b) ((a)==(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
static kuid_t caller_uid=10000;
#define current_euid() caller_uid
#define task_uid(p) ((p)->uid)
#define rcu_read_lock() do {}while(0)
#define rcu_read_unlock() do {}while(0)
typedef uint32_t u32;typedef uint64_t u64;typedef uint16_t u16;typedef uint8_t u8;
#define BIT(n) (1U<<(n))
#define MAX_PRIO 140
#define PF_EXITING 1
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define READ_ONCE(a) __atomic_load_n(&(a),__ATOMIC_RELAXED)
#define smp_load_acquire(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define WARN_ON_ONCE(a) assert(!(a))
#define WARN_ON(a) assert(!(a))
#define raw_spin_lock_irqsave(l,f) do {(f)=0;pthread_mutex_lock(l);}while(0)
#define raw_spin_unlock_irqrestore(l,f) do {(void)(f);pthread_mutex_unlock(l);}while(0)
#define pr_warn_ratelimited(...) do {}while(0)
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
struct list_head {struct list_head *next,*prev;};
#define LIST_HEAD(n) struct list_head n={&n,&n}
#define INIT_LIST_HEAD(p) do{(p)->next=(p);(p)->prev=(p);}while(0)
static bool list_empty(struct list_head *p){return p->next==p;}
static void list_add_tail(struct list_head *p,struct list_head*h){p->prev=h->prev;p->next=h;h->prev->next=p;h->prev=p;}
static void list_del_init(struct list_head*p){p->next->prev=p->prev;p->prev->next=p->next;INIT_LIST_HEAD(p);}
#define list_entry(p,t,m) container_of(p,t,m)
#define list_first_entry(h,t,m) list_entry((h)->next,t,m)
#define list_for_each_entry(p,h,m) for(p=list_entry((h)->next,__typeof__(*p),m);&p->m!=(h);p=list_entry(p->m.next,__typeof__(*p),m))
struct task_struct;
struct xr_qos_vip_node {struct list_head entry;struct task_struct *task;};
struct work_struct {int unused;};
struct task_struct {struct xr_qos_task *xr_qos;kuid_t uid;unsigned long flags;u32 xr_qos_dropped;atomic_int refs;};
enum xr_qos_dependency {XR_QOS_BINDER,XR_QOS_MUTEX,XR_QOS_RWSEM,XR_QOS_PERCPU_RWSEM,XR_QOS_FUTEX,XR_QOS_FUSE,XR_QOS_RCU,XR_QOS_DEPENDENCIES};
struct xr_qos_link {struct list_head incoming,outgoing;struct task_struct *from,*to;enum xr_qos_dependency type;bool tracked;};
static void get_task_struct(struct task_struct*p){atomic_fetch_add(&p->refs,1);}
static void put_task_struct(struct task_struct*p){assert(atomic_fetch_sub(&p->refs,1)>1);}
static void flush_work(struct work_struct*w){(void)w;}
'''
structs=src[src.index('#define XR_QOS_DEPTH'):src.index('/*\n * Graph state')]
globals=r'''
static pthread_mutex_t xr_qos_lock=PTHREAD_MUTEX_INITIALIZER;
static LIST_HEAD(xr_qos_tasks);static LIST_HEAD(xr_qos_active);
static unsigned int xr_task_count,xr_active_count,xr_edge_count;
static struct xr_qos_rt_user xr_rt_users[XR_QOS_ACTIVE_MAX];
static unsigned long xr_failures[XR_FAILURES];
static enum xr_qos_failure xr_last_failure;
static u64 xr_epoch;
static struct xr_qos_task *xr_update[XR_QOS_UPDATE_MAX];
static unsigned int xr_updates;
static void xr_queue(struct xr_qos_task*q){(void)q;}
static struct xr_qos_task *xr_get_or_alloc(struct task_struct*p,enum xr_qos_failure*f){(void)f;return p->xr_qos;}
'''
main=r'''
static int set(struct task_struct *p,u32 qos){return xr_qos_set(p,qos,false);}
static int uset(struct task_struct *p,u32 qos){return xr_qos_set(p,qos,true);}
#define N 160
#define E 384
static struct task_struct tasks[N];static struct xr_qos_task states[N];static struct xr_qos_link links[E];
static void init(void){
 memset(xr_rt_users,0,sizeof(xr_rt_users));memset(xr_failures,0,sizeof(xr_failures));caller_uid=10000;memset(tasks,0,sizeof(tasks));memset(states,0,sizeof(states));memset(links,0,sizeof(links));
 INIT_LIST_HEAD(&xr_qos_tasks);INIT_LIST_HEAD(&xr_qos_active);xr_task_count=N;xr_active_count=0;xr_edge_count=0;xr_epoch=0;
 for(int i=0;i<N;i++){struct xr_qos_task*q=&states[i];tasks[i].xr_qos=q;tasks[i].uid=10000;atomic_init(&tasks[i].refs,1);q->task=&tasks[i];
 INIT_LIST_HEAD(&q->all);INIT_LIST_HEAD(&q->active);INIT_LIST_HEAD(&q->incoming);INIT_LIST_HEAD(&q->outgoing);INIT_LIST_HEAD(&q->vip.entry);list_add_tail(&q->all,&xr_qos_tasks);
 for(int d=0;d<4;d++)q->level[d]=xr_value(0);q->effective=xr_value(0);}}
static struct xr_qos_value oracle(int t,int depth){
 struct xr_qos_value v=xr_value(states[t].exiting?0:states[t].static_qos);
 if(depth && !states[t].exiting)for(int e=0;e<E;e++)if(links[e].tracked && links[e].to==&tasks[t]) v=xr_merge(v,oracle(links[e].from-tasks,depth-1));
 return v;}
static void verify(void){unsigned int count=0;for(int i=0;i<N;i++){
 for(int d=0;d<4;d++)assert(xr_equal(states[i].level[d],oracle(i,d)));
 assert(xr_equal(states[i].effective,oracle(i,3)));count+=!!(states[i].effective.qos&15);}
 assert(count==xr_active_count && count<=XR_QOS_ACTIVE_MAX);
 unsigned charged=0;for(int u=0;u<XR_QOS_ACTIVE_MAX;u++){unsigned expect=0;for(int i=0;i<N;i++)expect+=states[i].rt_slot==u+1;
 assert(expect==xr_rt_users[u].count && expect<=XR_QOS_RT_UID_MAX);charged+=expect;}
 for(int i=0;i<N;i++)if(states[i].rt_slot)assert((states[i].static_qos&15) && ((states[i].static_qos>>4)&3)==1);assert(charged<=xr_active_count);}
static int add(int e,int a,int b){return xr_qos_link_add(&links[e],&tasks[a],&tasks[b],XR_QOS_MUTEX);}
static void clear(void){for(int e=0;e<E;e++)xr_qos_link_del(&links[e]);for(int i=0;i<N;i++)if(!states[i].exiting)assert(set(&tasks[i],0)==0);verify();assert(!xr_edge_count);for(int i=0;i<N;i++)assert(atomic_load(&tasks[i].refs)==1 && !tasks[i].xr_qos_dropped);}
static uint32_t randstate=1;
static uint32_t next(void){randstate^=randstate<<13;randstate^=randstate>>17;randstate^=randstate<<5;return randstate;}
static void *stress(void*arg){int id=(intptr_t)arg;for(int n=0;n<10000;n++){
 int a=(n+id)%32,b=(a+1+id)%32,e=300+id;
 int ret=add(e,a,b);assert(!ret||ret==-ENOSPC||ret==-E2BIG);
 ret=set(&tasks[a],(n%2)?0x29:0);assert(!ret||ret==-ENOSPC||ret==-E2BIG);
 xr_qos_link_del(&links[e]);}return NULL;}
static void *rt_admit(void *arg){return (void*)(intptr_t)uset(&tasks[(intptr_t)arg],0x19);}
int main(void){
 init();assert(set(&tasks[0],1)==-EINVAL);assert(set(&tasks[0],2)==-EINVAL);assert(set(&tasks[0],0x39)==-EINVAL);assert(set(&tasks[0],0x100)==-EINVAL);assert(set(&tasks[0],0x16)==-EINVAL);assert(set(&tasks[0],7)==-EINVAL);
 assert(!xr_qos_async_arm(&tasks[0],true));assert(xr_qos_async_consume(&tasks[0]));assert(!xr_qos_async_consume(&tasks[0]));assert(!xr_qos_async_arm(&tasks[0],false));assert(!xr_qos_async_consume(&tasks[0]));
 for(int i=0;i<5;i++)assert(!add(i,i,i+1));assert(!set(&tasks[0],0x29));verify();assert(states[3].effective.qos==0x29 && !states[4].effective.qos);
 assert(!add(5,2,0));verify();assert(!set(&tasks[0],0));verify();clear();
 assert(!add(0,0,2)&&!add(1,1,2)&&!add(2,2,3));assert(!set(&tasks[0],0x29));assert(!set(&tasks[1],0x17));verify();xr_qos_link_del(&links[0]);verify();assert(states[3].effective.qos==0x17);clear();
 /* A normal edge is recorded before a static QoS request arrives. */
 assert(!add(0,0,1)&&!add(1,1,2));assert(!set(&tasks[0],0x29));verify();tasks[0].flags=PF_EXITING;xr_qos_exit(&tasks[0]);verify();tasks[0].flags=0;clear();init();
 /* No partial update at active capacity; deboost and late retry remain valid. */
 for(int i=0;i<128;i++)assert(!set(&tasks[i],0x29));assert(set(&tasks[128],0x29)==-ENOSPC);assert(!states[128].static_qos);verify();assert(!set(&tasks[0],0));assert(!set(&tasks[128],0x29));clear();
 /* Legitimate dormant high-degree topology must support late activation. */
 for(int i=0;i<17;i++)assert(!add(i,0,i+1));
 assert(!tasks[0].xr_qos_dropped && !set(&tasks[0],0x29));verify();clear();
 /* A shared server accepts 65 distinct zero-QoS donors; boost one donor. */
 for(int i=0;i<65;i++)assert(!add(i,i,100));
 assert(!set(&tasks[64],0x29));verify();assert(xr_active_count==2);clear();
 /* Parallel object-owned edges have separate removal lifetimes. */
 for(int i=0;i<129;i++)assert(!add(i,0,1));
 assert(!set(&tasks[0],0x29));verify();
 for(int i=0;i<128;i++)xr_qos_link_del(&links[i]);verify();assert(states[1].effective.qos==0x29);
 xr_qos_link_del(&links[128]);verify();assert(!states[1].effective.qos);clear();
 /* Saturate the real global budget without changing graph values. */
 {struct xr_qos_link *cap=calloc(XR_QOS_EDGE_MAX+1,sizeof(*cap));assert(cap);
 for(int i=0;i<XR_QOS_EDGE_MAX;i++)assert(!xr_qos_link_add(&cap[i],&tasks[0],&tasks[1],XR_QOS_MUTEX));
 assert(xr_edge_count==XR_QOS_EDGE_MAX);assert(!set(&tasks[0],0x29));
 assert(states[0].effective.qos==0x29 && states[1].effective.qos==0x29 && xr_active_count==2);
 assert(xr_qos_link_add(&cap[XR_QOS_EDGE_MAX],&tasks[2],&tasks[3],XR_QOS_BINDER)==-ENOSPC);
 assert(tasks[2].xr_qos_dropped==1 && set(&tasks[2],0x29)==-ENOSPC);
 xr_qos_link_del(&cap[0]);assert(xr_edge_count==XR_QOS_EDGE_MAX-1 && states[1].effective.qos==0x29);
 xr_qos_link_del(&cap[XR_QOS_EDGE_MAX]);assert(!tasks[2].xr_qos_dropped);
 assert(!xr_qos_link_add(&cap[XR_QOS_EDGE_MAX],&tasks[2],&tasks[3],XR_QOS_BINDER));
 assert(!set(&tasks[2],0x29) && xr_active_count==4);
 assert(!set(&tasks[0],0) && !set(&tasks[2],0));
 for(int i=0;i<=XR_QOS_EDGE_MAX;i++)xr_qos_link_del(&cap[i]);
 free(cap);assert(!xr_edge_count);clear();}

 /* Own-UID RT priority 1 admission is bounded, repeatable, and transactional. */
 for(int i=0;i<4;i++)assert(!uset(&tasks[i],0x19));assert(!uset(&tasks[0],0x19));verify();
 assert(states[0].effective.rt_prio==98 && !states[0].effective.vip);
 assert(uset(&tasks[4],0x19)==-EDQUOT && !states[4].static_qos && !states[4].rt_slot);
 assert(!set(&tasks[8],0x29));assert(uset(&tasks[8],0x19)==-EDQUOT && states[8].static_qos==0x29 && !states[8].rt_slot);verify();
 assert(!uset(&tasks[0],0x29));assert(!uset(&tasks[4],0x19));verify();
 assert(!uset(&tasks[1],0));assert(!uset(&tasks[5],0x19));verify();
 /* An explicitly privileged replacement bypasses the quota and releases its charge. */
 assert(!set(&tasks[2],0x19));assert(!states[2].rt_slot);assert(!uset(&tasks[6],0x19));verify();
 tasks[3].flags=PF_EXITING;xr_qos_exit(&tasks[3]);tasks[3].flags=0;
 assert(!states[3].rt_slot);assert(!uset(&tasks[7],0x19));verify();clear();init();
 /* A different UID cannot consume the target's own-UID API. */
 tasks[0].uid=10001;assert(uset(&tasks[0],0x19)==-EPERM && !states[0].rt_slot);
 caller_uid=10001;assert(!uset(&tasks[0],0x19));tasks[0].uid=10002;caller_uid=10002;
 assert(!uset(&tasks[0],0x19));verify();assert(!uset(&tasks[0],0));verify();clear();init();
 /* Failed graph admission neither consumes a UID slot nor replaces the old value. */
 for(int i=0;i<128;i++)assert(!set(&tasks[i],0x29));
 assert(uset(&tasks[128],0x19)==-ENOSPC && !states[128].rt_slot && !states[128].static_qos);
 assert(!uset(&tasks[0],0x19));assert(add(0,0,128)==-ENOSPC);
 assert(uset(&tasks[0],0x1a)==-ENOSPC && states[0].static_qos==0x19 && states[0].rt_slot);
 xr_qos_link_del(&links[0]);verify();clear();init();
 /* All 128 slots can be occupied; replacing a last charge can reuse its slot. */
 for(int i=0;i<128;i++){tasks[i].uid=caller_uid=20000+i;assert(!uset(&tasks[i],0x19));}
 tasks[0].uid=caller_uid=40000;assert(!uset(&tasks[0],0x19));verify();clear();init();

 /* Simultaneous unprivileged admissions cannot overrun a UID quota. */
 {pthread_t admissions[8];int accepted=0;for(int i=0;i<8;i++)assert(!pthread_create(&admissions[i],NULL,rt_admit,(void*)(intptr_t)i));
 for(int i=0;i<8;i++){void *result;assert(!pthread_join(admissions[i],&result));int ret=(intptr_t)result;assert(!ret||ret==-EDQUOT);accepted+=!ret;}
 assert(accepted==XR_QOS_RT_UID_MAX);verify();clear();}
 for(int n=0;n<12000;n++){int e=next()%256,a=next()%32,b=next()%32,ret;
 if(next()&1){xr_qos_link_del(&links[e]);if(a!=b){ret=add(e,a,b);assert(!ret||ret==-ENOSPC||ret==-E2BIG);}}
 else{ret=set(&tasks[a],(next()%3)?0x29:0);assert(!ret||ret==-ENOSPC||ret==-E2BIG);}
 if(n%19==0)verify();}verify();clear();
 pthread_t th[4];for(int i=0;i<4;i++)assert(!pthread_create(&th[i],NULL,stress,(void*)(intptr_t)i));for(int i=0;i<4;i++)assert(!pthread_join(th[i],NULL));verify();clear();
 puts("PASS: real XR graph, global edge budget, UID RT quota/rollback/exit, bounded paths/cycles/late activation, 12000 oracle mutations, 40000 concurrent lifecycles, ASan+UBSan");return 0;}
'''
functions='\n'.join(function(n) for n in ['xr_fail','xr_warn_failure','xr_rt_uncharge','xr_rt_slot','xr_value','xr_merge','xr_equal','xr_mark','xr_recompute','xr_qos_set','xr_qos_link_add','xr_unlink','xr_qos_link_del','xr_qos_exit','xr_qos_async_arm','xr_qos_async_consume'])
with tempfile.TemporaryDirectory(prefix='xr-qos-test-') as d:
    c=pathlib.Path(d)/'test.c';exe=pathlib.Path(d)/'test';c.write_text(prelude+structs+globals+functions+main)
    subprocess.run([os.environ.get('HOSTCC','clang'),'-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-no-pie',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
