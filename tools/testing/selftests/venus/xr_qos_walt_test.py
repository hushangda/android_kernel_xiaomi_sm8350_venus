#!/usr/bin/env python3
"""Exercise actual WALT ownership/move helpers; accounting is a stub."""
import pathlib,re,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[4]
src=(ROOT/'kernel/sched/walt/walt.c').read_text()
def function(name):
 m=re.search(r'(?m)^(?:static )?[\w *]+\b'+name+r'\([^;]*?\)\s*\{',src);assert m,name
 b=src.index('{',m.start());e=b+1;d=1
 while d:d+=(src[e]=='{')-(src[e]=='}');e+=1
 return src[m.start():e]
pre=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#define CONFIG_XIAOMI_XR_QOS 1
#define MAX_NUM_CGROUP_COLOC_ID 20
#define DEFAULT_CGROUP_COLOC_ID 1
#define PF_EXITING 1
#define SINGLE_DEPTH_NESTING 1
#define rcu_assign_pointer(x,y) ((x)=(y))
struct list_head {struct list_head *next,*prev;};
static void init(struct list_head*h){h->next=h->prev=h;}
static int list_empty(struct list_head*h){return h->next==h;}
static void list_add(struct list_head*n,struct list_head*h){n->next=h->next;n->prev=h;h->next->prev=n;h->next=n;}
static void list_del_init(struct list_head*n){n->prev->next=n->next;n->next->prev=n->prev;init(n);}
static void list_move(struct list_head*n,struct list_head*h){list_del_init(n);list_add(n,h);}
struct rq {pthread_mutex_t lock;unsigned long grouped,ungrouped;};struct rq_flags{int unused;};
struct walt_related_thread_group {unsigned int id;pthread_mutex_t lock;struct list_head list,tasks;};
struct task_struct {pthread_mutex_t pi_lock;unsigned long flags;bool xr_qos_rtg_owned,xr_qos_rtg_blocked,colocated,requested;struct{struct walt_related_thread_group*grp;struct list_head grp_list;}wts;};
static pthread_mutex_t related_thread_group_lock=PTHREAD_MUTEX_INITIALIZER;
static struct list_head active_related_thread_groups;
static struct rq rq={.lock=PTHREAD_MUTEX_INITIALIZER,.grouped=99,.ungrouped=87};
static struct walt_related_thread_group groups[20];
static struct task_struct task={.pi_lock=PTHREAD_MUTEX_INITIALIZER};
static int adds,removes;
#define raw_spin_lock(l) assert(!pthread_mutex_lock(l))
#define raw_spin_unlock(l) assert(!pthread_mutex_unlock(l))
#define write_lock(l) raw_spin_lock(l)
#define write_unlock(l) raw_spin_unlock(l)
#define raw_spin_lock_irqsave(l,f) do{(f)=0;raw_spin_lock(l);}while(0)
#define raw_spin_unlock_irqrestore(l,f) do{(void)(f);raw_spin_unlock(l);}while(0)
#define raw_spin_lock_nested(l,c) raw_spin_lock(l)
static struct walt_related_thread_group*lookup_related_thread_group(unsigned int id){return id<20?&groups[id]:NULL;}
static int uclamp_task_colocated(struct task_struct*p){return p->colocated;}
static bool xr_qos_group_requested(struct task_struct*p){return p->requested;}
static struct rq*__task_rq_lock(struct task_struct*p,struct rq_flags*f){(void)p;(void)f;raw_spin_lock(&rq.lock);return &rq;}
static void __task_rq_unlock(struct rq*r,struct rq_flags*f){(void)f;raw_spin_unlock(&r->lock);}
static void _set_preferred_cluster(struct walt_related_thread_group*g){(void)g;}
static int add_task_to_group(struct task_struct*p,struct walt_related_thread_group*g){assert(!p->wts.grp);list_add(&p->wts.grp_list,&g->tasks);p->wts.grp=g;adds++;return 0;}
static void remove_task_from_group(struct task_struct*p){struct walt_related_thread_group*g=p->wts.grp;list_del_init(&p->wts.grp_list);p->wts.grp=NULL;if(list_empty(&g->tasks)&&g->id!=1)list_del_init(&g->list);removes++;}
'''
main=r'''
static void *move(void*arg){uintptr_t id=(uintptr_t)arg;for(unsigned n=0;n<10000;n++)assert(!walt_xr_qos_set_group(&task,2+(n+id)%18));return NULL;}
int main(void){init(&active_related_thread_groups);init(&task.wts.grp_list);for(int i=0;i<20;i++){groups[i].id=i;init(&groups[i].list);init(&groups[i].tasks);pthread_mutex_init(&groups[i].lock,NULL);}
 assert(walt_xr_qos_set_group(&task,1)==-EINVAL);assert(walt_xr_qos_set_group(&task,20)==-EINVAL);
 assert(!walt_xr_qos_set_group(&task,11));assert(task.xr_qos_rtg_owned&&adds==1);
 assert(!walt_xr_qos_set_group(&task,12));assert(task.wts.grp==&groups[12]&&adds==1&&!removes);
 pthread_t th[4];for(int i=0;i<4;i++)assert(!pthread_create(&th[i],NULL,move,(void*)(uintptr_t)i));for(int i=0;i<4;i++)pthread_join(th[i],NULL);
 assert(rq.grouped==99&&rq.ungrouped==87&&adds==1&&!removes);
 task.requested=true;assert(!__sched_set_group_id(&task,5));assert(!task.xr_qos_rtg_owned&&task.xr_qos_rtg_blocked&&task.wts.grp==&groups[5]);
 assert(walt_xr_qos_set_group(&task,12)==-EBUSY);assert(!walt_xr_qos_set_group(&task,0));assert(task.wts.grp==&groups[5]);assert(walt_xr_qos_set_group(&task,12)==-EBUSY);
 task.requested=false;assert(!__sched_set_group_id(&task,0));
 /* Explicit group 0 wins even before the queued QoS worker attached. */
 task.requested=true;assert(!__sched_set_group_id(&task,0));assert(task.xr_qos_rtg_blocked&&walt_xr_qos_set_group(&task,12)==-EBUSY);
 assert(!walt_xr_qos_set_group(&task,0));assert(!walt_xr_qos_set_group(&task,12));task.flags=PF_EXITING;assert(walt_xr_qos_set_group(&task,11)==-ESRCH);assert(!walt_xr_qos_set_group(&task,0));assert(!task.wts.grp);
 task.flags=0;task.colocated=true;assert(walt_xr_qos_set_group(&task,12)==-EBUSY);
 puts("PASS: actual WALT helpers, 40000 concurrent moves, unchanged transfer counts/history, explicit ownership, queued-worker race, cleanup and colocation; ASan+UBSan");}
'''
with tempfile.TemporaryDirectory(prefix='xr-walt-') as d:
 c=pathlib.Path(d)/'test.c';e=pathlib.Path(d)/'test';c.write_text(pre+function('walt_xr_qos_set_group')+function('__sched_set_group_id')+main)
 subprocess.run(['clang','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-no-pie',str(c),'-o',str(e)],check=True)
 subprocess.run([str(e)],check=True)
