#!/usr/bin/env python3
"""Run the actual native lock donation helpers with deterministic task stubs.

This validates bookkeeping and cleanup under host concurrency, not scheduling
latency, real RCU grace periods, or booted-kernel lockdep.
"""
import pathlib
import subprocess
import tempfile
from scoped_reclaim_test import function

ROOT = pathlib.Path(__file__).resolve().parents[4]
RWSEM = (ROOT / "kernel/locking/rwsem.c").read_text()
MUTEX = (ROOT / "kernel/locking/mutex.c").read_text()

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <errno.h>
#define CONFIG_XIAOMI_XR_QOS 1
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define GFP_KERNEL 0
#define EXPORT_SYMBOL_GPL(x)
#define WARN_ON_ONCE(x) assert(!(x))
#define pr_warn_ratelimited(...) do {} while (0)
#define rcu_read_lock() do {} while (0)
#define rcu_read_unlock() do {} while (0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct list_head { struct list_head *next,*prev; };
static void init_list(struct list_head *h) { h->next=h->prev=h; }
static void add_tail(struct list_head *n, struct list_head *h) {
 n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;
}
static void del(struct list_head *n) { n->prev->next=n->next;n->next->prev=n->prev; }
#define list_for_each_entry(p,h,m) for (p=container_of((h)->next,__typeof__(*p),m); &(p)->m!=(h); p=container_of((p)->m.next,__typeof__(*p),m))
struct task_struct { int id,refs; };
static _Thread_local struct task_struct *current;
typedef struct { pthread_mutex_t mutex; int held; } raw_spinlock_t;
static void lock(raw_spinlock_t *l) { pthread_mutex_lock(&l->mutex);assert(!l->held);l->held=1; }
static void unlock(raw_spinlock_t *l) { assert(l->held);l->held=0;pthread_mutex_unlock(&l->mutex); }
#define raw_spin_lock_irqsave(l,f) do { (f)=0;lock(l); } while(0)
#define raw_spin_unlock_irqrestore(l,f) do { (void)(f);unlock(l); } while(0)
#define lockdep_assert_held(l) assert((l)->held)
static void get_task_struct(struct task_struct *p) { assert(p->refs>0);p->refs++; }
static void put_task_struct(struct task_struct *p) { assert(p->refs>1);p->refs--; }
static int fail_alloc;
static void *kzalloc(size_t n,int gfp) { (void)gfp;return fail_alloc?NULL:calloc(1,n); }
#define kfree free
enum xr_qos_dependency { XR_QOS_MUTEX,XR_QOS_RWSEM,XR_QOS_PERCPU_RWSEM };
struct xr_qos_link { struct task_struct *from,*to;enum xr_qos_dependency type;bool tracked; };
static bool fail_link;
static int xr_qos_link_add(struct xr_qos_link *l,struct task_struct *a,struct task_struct *b,enum xr_qos_dependency t) {
 assert(a && b && a!=b && !l->from);l->from=a;l->to=b;l->type=t;get_task_struct(a);get_task_struct(b);l->tracked=!fail_link;return fail_link?-ENOMEM:0;
}
static void xr_qos_link_del(struct xr_qos_link *l) {
 if(l->from){put_task_struct(l->from);put_task_struct(l->to);l->from=l->to=NULL;l->tracked=false;}
}
#define XR_RWSEM_READERS 64
struct xr_rwsem_state {bool mmap,static_state;struct {struct task_struct *task;unsigned int depth;} readers[XR_RWSEM_READERS];struct task_struct *drain_task;struct xr_qos_link drain_link;};
struct rw_semaphore {unsigned long owner,count;raw_spinlock_t wait_lock;struct list_head wait_list;struct xr_rwsem_state *xr_qos;};
enum rwsem_waiter_type {RWSEM_WAITING_FOR_WRITE,RWSEM_WAITING_FOR_READ};
struct rwsem_waiter {struct list_head list;struct task_struct *task;enum rwsem_waiter_type type;struct xr_qos_link xr_qos;};
struct mutex {unsigned long owner;raw_spinlock_t wait_lock;struct list_head wait_list;};
struct mutex_waiter {struct list_head list;struct task_struct *task;struct xr_qos_link xr_qos;};
#define RWSEM_READER_OWNED 1UL
#define RWSEM_NONSPINNABLE 6UL
#define RWSEM_OWNER_FLAGS_MASK 7UL
#define RWSEM_OWNER_UNKNOWN (-2L)
#define RWSEM_FLAG_WAITERS 2UL
#define atomic_long_read(p) (*(p))
static struct task_struct *__owner_task(unsigned long p) {return (void *)(p & ~7UL);}
'''

TESTS = r'''
static struct task_struct tasks[80];
static struct rw_semaphore sem;
static void init_lock(raw_spinlock_t *l) {pthread_mutex_init(&l->mutex,NULL);l->held=0;}
static void reset(void) {
 memset(&sem,0,sizeof(sem));init_list(&sem.wait_list);init_lock(&sem.wait_lock);
 for(int i=0;i<80;i++){tasks[i].id=i;tasks[i].refs=1;}
 current=&tasks[0];rwsem_xr_qos_init(&sem,true);
}
static void finish(void) {
 rwsem_xr_qos_destroy(&sem);for(int i=0;i<80;i++) assert(tasks[i].refs==1);
 pthread_mutex_destroy(&sem.wait_lock.mutex);
}
static void enqueue(struct rwsem_waiter *w,int tid,int type) {
 memset(w,0,sizeof(*w));w->task=&tasks[tid];w->type=type;add_tail(&w->list,&sem.wait_list);sem.count|=RWSEM_FLAG_WAITERS;
}
static void dequeue(struct rwsem_waiter *w) {xr_qos_link_del(&w->xr_qos);del(&w->list);}
static void *reader_thread(void *arg) {
 current=&tasks[(intptr_t)arg];
 for(int i=0;i<5000;i++){rwsem_xr_qos_read_acquired(&sem);rwsem_xr_qos_read_acquired(&sem);
  rwsem_xr_qos_forget_reader(&sem);rwsem_xr_qos_forget_reader(&sem);}
 return NULL;
}
int main(void) {
 struct rwsem_waiter a,b,c;
 reset();sem.owner=(uintptr_t)&tasks[1];enqueue(&a,2,RWSEM_WAITING_FOR_WRITE);
 enqueue(&b,3,RWSEM_WAITING_FOR_READ);enqueue(&c,4,RWSEM_WAITING_FOR_WRITE);
 lock(&sem.wait_lock);rwsem_xr_qos_refresh(&sem);
 assert(a.xr_qos.to==&tasks[1] && b.xr_qos.to==&tasks[2] && c.xr_qos.to==&tasks[2]);
 /* writer cancellation rewires both queued readers and the next writer */
 dequeue(&a);rwsem_xr_qos_refresh(&sem);
 assert(b.xr_qos.to==&tasks[1] && c.xr_qos.to==&tasks[1]);
 sem.owner=RWSEM_OWNER_UNKNOWN;rwsem_xr_qos_refresh(&sem);
 assert(!b.xr_qos.to && !c.xr_qos.to);
 dequeue(&b);dequeue(&c);unlock(&sem.wait_lock);finish();

 reset();sem.owner=RWSEM_READER_OWNED;current=&tasks[1];rwsem_xr_qos_read_acquired(&sem);
 rwsem_xr_qos_read_acquired(&sem);current=&tasks[2];rwsem_xr_qos_read_acquired(&sem);
 enqueue(&a,3,RWSEM_WAITING_FOR_WRITE);lock(&sem.wait_lock);rwsem_xr_qos_refresh(&sem);unlock(&sem.wait_lock);
 assert(a.xr_qos.to==&tasks[1]);current=&tasks[1];rwsem_xr_qos_forget_reader(&sem);
 assert(a.xr_qos.to==&tasks[1]);rwsem_xr_qos_forget_reader(&sem);
 assert(a.xr_qos.to==&tasks[2]);current=&tasks[2];rwsem_xr_qos_forget_reader(&sem);assert(!a.xr_qos.to);
 dequeue(&a);finish();

 /* Bound and overflow: no out-of-bounds entry or invented reader reference. */
 reset();sem.owner=RWSEM_READER_OWNED;
 for(int i=0;i<70;i++){current=&tasks[i];rwsem_xr_qos_read_acquired(&sem);}
 for(int i=0;i<70;i++) assert(tasks[i].refs==(i<64?2:1));
 for(int i=69;i>=0;i--){current=&tasks[i];rwsem_xr_qos_forget_reader(&sem);}
 finish();

 reset();rwsem_xr_qos_destroy(&sem);rwsem_xr_qos_init(&sem,false);
 sem.owner=(uintptr_t)&tasks[1];enqueue(&a,2,RWSEM_WAITING_FOR_READ);
 lock(&sem.wait_lock);rwsem_xr_qos_refresh(&sem);unlock(&sem.wait_lock);
 assert(a.xr_qos.type==XR_QOS_PERCPU_RWSEM && a.xr_qos.to==&tasks[1]);
 dequeue(&a);finish();

 reset();rwsem_xr_qos_destroy(&sem);fail_alloc=1;rwsem_xr_qos_init(&sem,true);assert(!sem.xr_qos);
 rwsem_xr_qos_read_acquired(&sem);rwsem_xr_qos_forget_reader(&sem);fail_alloc=0;finish();

 reset();sem.owner=RWSEM_READER_OWNED;pthread_t workers[8];
 for(int i=0;i<8;i++) assert(!pthread_create(&workers[i],NULL,reader_thread,(void *)(intptr_t)i));
 for(int i=0;i<8;i++) pthread_join(workers[i],NULL);finish();

 reset();struct mutex m={.owner=(uintptr_t)&tasks[1]};init_list(&m.wait_list);init_lock(&m.wait_lock);
 struct mutex_waiter x={.task=&tasks[2]},y={.task=&tasks[3]};add_tail(&x.list,&m.wait_list);add_tail(&y.list,&m.wait_list);
 lock(&m.wait_lock);mutex_xr_qos_refresh(&m);assert(x.xr_qos.to==&tasks[1] && y.xr_qos.to==&tasks[1]);
 m.owner=(uintptr_t)&tasks[2];mutex_xr_qos_refresh(&m);assert(!x.xr_qos.to && y.xr_qos.to==&tasks[2]);
 m.owner=0;mutex_xr_qos_refresh(&m);assert(!x.xr_qos.to && !y.xr_qos.to);
 /* Rejected desired-owner markers must be retried and released too. */
 fail_link=true;m.owner=(uintptr_t)&tasks[1];mutex_xr_qos_refresh(&m);
 assert(x.xr_qos.to==&tasks[1] && !x.xr_qos.tracked);
 fail_link=false;mutex_xr_qos_refresh(&m);assert(x.xr_qos.tracked && y.xr_qos.tracked);
 m.owner=0;mutex_xr_qos_refresh(&m);assert(!x.xr_qos.from && !y.xr_qos.from);
 unlock(&m.wait_lock);pthread_mutex_destroy(&m.wait_lock.mutex);finish();
 puts("PASS actual mutex/rwsem/percpu helpers: owner handoff, cancellation, nested readers, 64-reader bound, allocation failure, 40000 concurrent lifetimes");
}
'''


def main():
    pieces = [PRELUDE, function((ROOT / "include/linux/xr_qos.h").read_text(), "xr_qos_link_matches")]
    for name in ("rwsem_xr_qos_init", "rwsem_xr_qos_destroy", "rwsem_xr_qos_refresh",
                 "rwsem_xr_qos_record_reader", "rwsem_xr_qos_read_acquired", "rwsem_xr_qos_forget_reader"):
        pieces.append(function(RWSEM, name))
    pieces.extend([function(MUTEX, "mutex_xr_qos_refresh"), TESTS])
    with tempfile.TemporaryDirectory(prefix="venus-qos-locks-") as temporary:
        path = pathlib.Path(temporary)
        (path / "test.c").write_text("\n".join(pieces))
        subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-pthread", "-fsanitize=address,undefined",
                        "-no-pie", str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
