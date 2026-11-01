#!/usr/bin/env python3
"""Exercise actual percpu rwsem outer APIs and native QoS helpers on the host.

Per-CPU sums, RCU grace periods and the inner rwsem are deterministic host stubs.
This checks hook placement/lifetimes, not kernel memory-ordering or scheduler latency.
"""
import pathlib
import subprocess
import tempfile
import qos_locks_test as locks
from scoped_reclaim_test import function

ROOT = pathlib.Path(__file__).resolve().parents[4]
HEADER = (ROOT / "include/linux/percpu-rwsem.h").read_text()
SOURCE = (ROOT / "kernel/locking/percpu-rwsem.c").read_text()
PRELUDE = locks.PRELUDE.replace(
    "struct rw_semaphore {unsigned long owner,count;",
    "struct rw_semaphore {_Atomic unsigned long owner,count;pthread_rwlock_t gate;")
SUPPORT = r'''
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>
#define unlikely(x) (x)
#define likely(x) (x)
#define might_sleep() do {} while(0)
#define rwsem_acquire_read(...) do {} while(0)
#define rwsem_release(...) do {} while(0)
#define lock_acquire(...) do {} while(0)
#define lock_release(...) do {} while(0)
#define atomic_long_set(p,v) (*(p)=(v))
#define _RET_IP_ 0
#define smp_mb() atomic_thread_fence(memory_order_seq_cst)
#define smp_load_acquire(p) atomic_load(p)
#define smp_store_release(p,v) atomic_store(p,v)
#define __this_cpu_inc(v) atomic_fetch_add(&(v),1)
#define this_cpu_inc(v) __this_cpu_inc(v)
#define __this_cpu_dec(v) atomic_fetch_sub(&(v),1)
#define per_cpu_sum(v) atomic_load(&(v))
#define alloc_percpu(t) calloc(1,sizeof(t))
#define free_percpu(p) free(p)
#define TASK_UNINTERRUPTIBLE 0
struct lock_class_key {int unused;};
struct rcu_sync {_Atomic int idle;};
struct rcuwait {_Atomic int waiting;};
struct percpu_rw_semaphore {struct rcu_sync rss;_Atomic unsigned *read_count;struct rw_semaphore rw_sem;struct rcuwait writer;_Atomic int readers_block;};
static _Thread_local int preempt_count;
static void preempt_disable(void){preempt_count++;}
static void preempt_enable(void){assert(preempt_count>0);preempt_count--;}
#define preempt_enable_no_resched preempt_enable
static int rcu_sync_is_idle(struct rcu_sync *s){return s->idle;}
static void rcu_sync_init(struct rcu_sync *s){s->idle=1;}
static void rcu_sync_enter(struct rcu_sync *s){s->idle=0;}
static void rcu_sync_exit(struct rcu_sync *s){s->idle=1;}
static void rcu_sync_dtor(struct rcu_sync *s){assert(s->idle);}
static void rcuwait_init(struct rcuwait *w){w->waiting=0;}
static void rcuwait_wake_up(struct rcuwait *w){(void)w;}
#define rcuwait_wait_event(w,condition,state) do { (w)->waiting=1;while(!(condition))sched_yield();(w)->waiting=0;}while(0)
static void __init_rwsem(struct rw_semaphore *s,const char *n,struct lock_class_key *k){
 (void)n;(void)k;memset(s,0,sizeof(*s));pthread_mutex_init(&s->wait_lock.mutex,NULL);
 pthread_rwlock_init(&s->gate,NULL);init_list(&s->wait_list);
}
static void __down_read(struct rw_semaphore *s){pthread_rwlock_rdlock(&s->gate);}
static void __up_read(struct rw_semaphore *s){pthread_rwlock_unlock(&s->gate);}
static void down_write(struct rw_semaphore *s){pthread_rwlock_wrlock(&s->gate);s->owner=(uintptr_t)current;}
static void up_write(struct rw_semaphore *s){s->owner=0;pthread_rwlock_unlock(&s->gate);}
static void __percpu_up_read(struct percpu_rw_semaphore *);
static int __percpu_down_read(struct percpu_rw_semaphore *,int);
'''
TESTS = r'''
static struct task_struct tasks[80];
static struct percpu_rw_semaphore sem;
static void init(void){
 memset(&sem,0,sizeof(sem));for(int i=0;i<80;i++){tasks[i].refs=1;tasks[i].id=i;}
 current=&tasks[0];assert(!__percpu_init_rwsem(&sem,"test",NULL));assert(!preempt_count);
}
static void finish(void){
 assert(!*sem.read_count);percpu_free_rwsem(&sem);assert(!sem.read_count);
 for(int i=0;i<80;i++)assert(tasks[i].refs==1);
 pthread_mutex_destroy(&sem.rw_sem.wait_lock.mutex);pthread_rwlock_destroy(&sem.rw_sem.gate);
 assert(!preempt_count);
}
static unsigned depth(int task){
 unsigned n=0;lock(&sem.rw_sem.wait_lock);
 for(int i=0;i<64;i++)if(sem.rw_sem.xr_qos->readers[i].task==&tasks[task])n=sem.rw_sem.xr_qos->readers[i].depth;
 unlock(&sem.rw_sem.wait_lock);return n;
}
static void assert_target(int task){
 lock(&sem.rw_sem.wait_lock);assert(sem.rw_sem.xr_qos->drain_link.to==(task<0?NULL:&tasks[task]));unlock(&sem.rw_sem.wait_lock);
}
static void wait_drain(void){while(!sem.writer.waiting)sched_yield();}
static void *writer(void *arg){
 (void)arg;current=&tasks[79];percpu_down_write(&sem);
 assert(!sem.rw_sem.xr_qos->drain_task && !sem.rw_sem.xr_qos->drain_link.from);
 assert(!*sem.read_count);percpu_up_write(&sem);return NULL;
}
static void *reader(void *arg){
 current=&tasks[(intptr_t)arg];for(int i=0;i<5000;i++){
  percpu_down_read(&sem);assert(percpu_down_read_trylock(&sem));
  percpu_up_read(&sem);percpu_up_read(&sem);
 }assert(!preempt_count);return NULL;
}
int main(void){
 alarm(20);pthread_t w;
 /* Actual fast outer APIs, nesting and failed trylock: no phantom metadata. */
 init();current=&tasks[1];percpu_down_read(&sem);assert(depth(1)==1);
 assert(percpu_down_read_trylock(&sem));assert(depth(1)==2 && *sem.read_count==2);
 percpu_up_read(&sem);assert(depth(1)==1);percpu_up_read(&sem);assert(!depth(1));
 sem.rss.idle=0;sem.readers_block=1;assert(!percpu_down_read_trylock(&sem));
 assert(!*sem.read_count && !depth(1));sem.readers_block=0;sem.rss.idle=1;finish();

 /* Writer already owns the inner rwsem but must donate to real outer readers. */
 init();current=&tasks[1];percpu_down_read(&sem);percpu_down_read(&sem);
 current=&tasks[2];percpu_down_read(&sem);assert(!pthread_create(&w,NULL,writer,NULL));wait_drain();
 assert_target(1);current=&tasks[1];percpu_up_read(&sem);assert_target(1);
 percpu_up_read(&sem);assert_target(2);current=&tasks[2];percpu_up_read(&sem);
 pthread_join(w,NULL);assert_target(-1);finish();

 /* An async I/O hold has no task owner between release and acquire. */
 init();current=&tasks[1];percpu_down_read(&sem);percpu_rwsem_release(&sem,true,0);
 assert(!depth(1) && tasks[1].refs==1 && *sem.read_count==1);
 assert(!pthread_create(&w,NULL,writer,NULL));wait_drain();assert_target(-1);
 current=&tasks[2];percpu_rwsem_acquire(&sem,true,0);assert_target(2);assert(depth(2)==1);
 percpu_up_read(&sem);pthread_join(w,NULL);finish();

 /* Read release does not discard another nested hold. */
 init();current=&tasks[1];percpu_down_read(&sem);percpu_down_read(&sem);
 percpu_rwsem_release(&sem,true,0);assert(depth(1)==1);
 current=&tasks[2];percpu_rwsem_acquire(&sem,true,0);percpu_up_read(&sem);
 current=&tasks[1];percpu_up_read(&sem);finish();

 /* Writer ownership transfer updates queued edges, including unknown owner. */
 init();current=&tasks[1];percpu_down_write(&sem);
 struct rwsem_waiter waiter={.task=&tasks[3],.type=RWSEM_WAITING_FOR_READ};
 add_tail(&waiter.list,&sem.rw_sem.wait_list);sem.rw_sem.count=RWSEM_FLAG_WAITERS;
 rwsem_xr_qos_owner_changed(&sem.rw_sem);assert(waiter.xr_qos.to==current);
 percpu_rwsem_release(&sem,false,0);assert(!waiter.xr_qos.from);
 current=&tasks[2];percpu_rwsem_acquire(&sem,false,0);assert(waiter.xr_qos.to==current);
 xr_qos_link_del(&waiter.xr_qos);del(&waiter.list);sem.rw_sem.count=0;percpu_up_write(&sem);finish();

 /* 64 readers, overflow, rejected-link retry and exact reference cleanup. */
 init();for(int i=0;i<70;i++){current=&tasks[i];percpu_down_read(&sem);}
 for(int i=0;i<70;i++)assert(depth(i)==(i<64));
 current=&tasks[79];fail_link=true;rwsem_xr_qos_percpu_wait(&sem.rw_sem,true);
 assert_target(0);assert(!sem.rw_sem.xr_qos->drain_link.tracked);
 fail_link=false;current=&tasks[0];percpu_up_read(&sem);assert_target(1);
 assert(sem.rw_sem.xr_qos->drain_link.tracked);
 for(int i=1;i<70;i++){current=&tasks[i];percpu_up_read(&sem);}
 assert_target(-1);current=&tasks[79];rwsem_xr_qos_percpu_wait(&sem.rw_sem,false);finish();

 /* Dynamic allocation failure preserves the actual counter and lock semantics. */
 fail_alloc=1;init();assert(!sem.rw_sem.xr_qos);percpu_down_read(&sem);
 assert(*sem.read_count==1);percpu_up_read(&sem);percpu_down_write(&sem);percpu_up_write(&sem);
 fail_alloc=0;finish();

 /* Static storage is tracked and never passed to kfree at teardown. */
 init();rwsem_xr_qos_destroy(&sem.rw_sem);
 static struct xr_rwsem_state static_state={.static_state=true};sem.rw_sem.xr_qos=&static_state;
 current=&tasks[1];percpu_down_read(&sem);assert(depth(1)==1);percpu_up_read(&sem);finish();
 assert(static_state.static_state && !static_state.readers[0].task);

 /* Real outer reader/trylock/up calls under concurrent host bookkeeping. */
 init();pthread_t r[8];for(int i=0;i<8;i++)assert(!pthread_create(&r[i],NULL,reader,(void *)(intptr_t)i));
 for(int i=0;i<8;i++)pthread_join(r[i],NULL);finish();
 puts("PASS actual percpu outer APIs: fast/try/failed read, drain handoff, async ownership transfer, writer transfer, 64-reader bound, rejected edge, allocation failure, static lifetime, 40000 concurrent nested lifetimes");
}
'''


def main():
    pieces = [PRELUDE, SUPPORT, function((ROOT / "include/linux/xr_qos.h").read_text(), "xr_qos_link_matches")]
    for name in ("rwsem_xr_qos_init", "rwsem_xr_qos_destroy", "rwsem_xr_qos_refresh",
                 "rwsem_xr_qos_record_reader", "rwsem_xr_qos_percpu_read_acquired",
                 "rwsem_xr_qos_percpu_wait", "rwsem_xr_qos_forget_reader", "rwsem_xr_qos_owner_changed"):
        pieces.append(function(locks.RWSEM, name))
    for name in ("__percpu_init_rwsem", "percpu_free_rwsem", "__percpu_up_read", "__percpu_down_read",
                 "readers_active_check", "percpu_down_write", "percpu_up_write"):
        pieces.append(function(SOURCE, name))
    for name in ("percpu_down_read", "percpu_down_read_trylock", "percpu_up_read", "percpu_rwsem_release", "percpu_rwsem_acquire"):
        pieces.append(function(HEADER, name))
    pieces.append(TESTS)
    with tempfile.TemporaryDirectory(prefix="venus-percpu-qos-") as temporary:
        path = pathlib.Path(temporary)
        (path / "test.c").write_text("\n".join(pieces))
        subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-pthread", "-fsanitize=address,undefined",
                        "-no-pie", str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
