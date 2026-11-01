#!/usr/bin/env python3
"""Actual fair VIP helpers with deterministic rq clocks and CFS throttling."""
import pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[4]
src=(ROOT/'kernel/sched/xr_qos_fair.h').read_text()
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
typedef uint64_t u64;
#define CONFIG_XIAOMI_XR_QOS 1
#define NSEC_PER_MSEC 1000000UL
struct list_head {struct list_head *next,*prev;};
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define INIT_LIST_HEAD(h) do{(h)->next=(h);(h)->prev=(h);}while(0)
static bool list_empty(struct list_head*h){return h->next==h;}
static void list_add_tail(struct list_head*n,struct list_head*h){n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
static void list_del_init(struct list_head*n){n->next->prev=n->prev;n->prev->next=n->next;INIT_LIST_HEAD(n);}
static void list_move_tail(struct list_head*n,struct list_head*h){list_del_init(n);list_add_tail(n,h);}
#define list_entry(p,t,m) container_of(p,t,m)
#define list_for_each_entry(p,h,m) for(p=list_entry((h)->next,__typeof__(*p),m);&p->m!=(h);p=list_entry(p->m.next,__typeof__(*p),m))
struct task_struct;struct xr_qos_vip_node{struct list_head entry;struct task_struct*task;};
struct cfs_rq{bool throttled,exhausted;};struct sched_entity {bool on_rq;struct cfs_rq*cfs;struct sched_entity*parent;};
struct task_struct{unsigned xr_qos_vip;bool queued;void*sched_class;struct sched_entity se;struct xr_qos_vip_node node;};
struct rq{u64 xr_vip_period,xr_vip_runtime,now;struct list_head xr_vip_tasks;unsigned resched;};
static int fair_sched_class;
static u64 rq_clock_task(struct rq*r){return r->now;}
static bool task_on_rq_queued(struct task_struct*p){return p->queued;}
static struct cfs_rq*task_cfs_rq(struct task_struct*p){return p->se.cfs;}
static struct cfs_rq*cfs_rq_of(struct sched_entity*s){return s->cfs;}
#define for_each_sched_entity(s) for(;(s);(s)=(s)->parent)
static bool throttled_hierarchy(struct cfs_rq*c){return c->throttled;}
static bool check_cfs_rq_runtime(struct cfs_rq*c){if(c->exhausted)c->throttled=true;return c->throttled;}
static void resched_curr(struct rq*r){r->resched++;}
static struct xr_qos_vip_node*xr_qos_vip_node(struct task_struct*p){return &p->node;}
'''
main=r'''
int main(void){struct rq r={0};struct task_struct t[3]={0};struct cfs_rq c[3]={0};INIT_LIST_HEAD(&r.xr_vip_tasks);
 for(int i=0;i<3;i++){t[i].node.task=&t[i];INIT_LIST_HEAD(&t[i].node.entry);t[i].se.on_rq=true;t[i].se.cfs=&c[i];t[i].queued=true;t[i].sched_class=&fair_sched_class;}
 xr_qos_vip_changed(&r,&t[0],5);xr_qos_vip_changed(&r,&t[1],5);xr_qos_vip_changed(&r,&t[2],3);
 assert(xr_vip_pick(&r)==&t[0]);assert(xr_vip_pick(&r)==&t[1]);assert(xr_vip_pick(&r)==&t[0]);
 c[0].throttled=true;assert(xr_vip_pick(&r)==&t[1]);c[1].exhausted=true;assert(xr_vip_pick(&r)==&t[2]);assert(c[1].throttled);
 r.now=500000;xr_qos_vip_account(&r,&t[2],500000);assert(xr_vip_pick(&r)==&t[2]);r.now=1000000;unsigned old=r.resched;xr_qos_vip_account(&r,&t[2],500000);assert(r.resched==old+1);assert(!xr_vip_pick(&r));
 r.now=7999999;assert(!xr_vip_pick(&r));r.now=8000000;assert(xr_vip_pick(&r)==&t[2]);assert(!r.xr_vip_runtime);
 xr_qos_vip_changed(&r,&t[2],0);assert(list_empty(&t[2].node.entry));assert(!xr_vip_pick(&r));
 c[0].throttled=false;c[1].throttled=false;c[1].exhausted=false;xr_vip_dequeue(&t[0]);assert(xr_vip_pick(&r)==&t[1]);
 t[1].se.on_rq=false;assert(!xr_vip_pick(&r));xr_vip_dequeue(&t[1]);assert(list_empty(&r.xr_vip_tasks));
 puts("PASS: actual VIP helpers, 1ms/8ms budget, priority and round robin, CFS throttling, disable/dequeue and unrunnable tasks; ASan+UBSan");}
'''
with tempfile.TemporaryDirectory(prefix='xr-vip-') as d:
 c=pathlib.Path(d)/'test.c';e=pathlib.Path(d)/'test';c.write_text(pre+src+main)
 subprocess.run(['clang','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(c),'-o',str(e)],check=True)
 subprocess.run([str(e)],check=True)
