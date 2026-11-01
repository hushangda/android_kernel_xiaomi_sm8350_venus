#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the actual EROFS worker lifecycle with bounded host API mocks.

This checks dispatch, allocation failures and teardown ordering. It does not
emulate the scheduler or prove concurrent RCU/hotplug behavior on a device.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / "fs/erofs/zdata.c"


def region(source, start, end):
    return source[source.index(start):source.index(end)]


source = SOURCE.read_text()
workers = region(source, "static struct workqueue_struct *z_erofs_workqueue",
                 "enum z_erofs_collectmode")
dispatch = region(source,
                  "static void z_erofs_decompressqueue_bg(struct z_erofs_decompressqueue *bgq);",
                  "static void z_erofs_decompress_kickoff")
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NR_CPUS 4
#define CPUHP_INVALID (-1)
#define CPUHP_AP_ONLINE_DYN 42
#define __rcu
#define __read_mostly
#define __init
#define DEFINE_PER_CPU(type, name) type name[NR_CPUS]
#define per_cpu(name, cpu) name[cpu]
#define rcu_assign_pointer(dst, ptr) ((dst) = (ptr))
#define rcu_dereference(ptr) (ptr)
#define rcu_dereference_protected(ptr, cond) (ptr)
#define IS_ERR(ptr) ((uintptr_t)(ptr) > (uintptr_t)-4096)
#define PTR_ERR(ptr) ((long)(intptr_t)(ptr))
#define IS_ENABLED(config) (config)
#define pr_warn(...) ((void)0)
#define WQ_UNBOUND 1
#define WQ_HIGHPRI 2
#define INIT_WORK(work, callback) ((work)->func = (callback))
#define kthread_init_work(work, callback) ((work)->func = (callback))

struct work_struct { void (*func)(struct work_struct *); };
struct kthread_work { void (*func)(struct kthread_work *); };
struct kthread_worker {
    void *task;
    unsigned int cpu;
    struct kthread_work *pending;
};
struct workqueue_struct { struct work_struct *pending[32]; int count; };
struct z_erofs_decompressqueue {
    union { struct work_struct work; struct kthread_work kthread_work; } u;
};

static unsigned int this_cpu;
static int pool_fail, wq_fail, setup_fail, fail_cpu = -1;
static int pool_live, wq_live, live_workers, destroyed, highpri;
static int pcpu_queued, wq_queued, completed, grace_count, pins, readers;
static int setup_calls, remove_calls, teardown_cpu = -1;
static int (*startup_callback)(unsigned int);
static int (*teardown_callback)(unsigned int);
static bool inject_enqueue;
static struct z_erofs_decompressqueue late_io;

static int num_possible_cpus(void) { return 2; } /* Sparse CPU IDs 0 and 3. */
static int get_cpu(void) { ++pins; return this_cpu; }
static void put_cpu(void) { assert(pins > 0); --pins; }
static void rcu_read_lock(void) { ++readers; }
static void rcu_read_unlock(void) { assert(readers > 0); --readers; }
static int z_erofs_create_pcluster_pool(void)
{
    if (pool_fail) return -ENOMEM;
    assert(!pool_live); pool_live = 1; return 0;
}
static void z_erofs_destroy_pcluster_pool(void)
{
    assert(pool_live && !wq_live && !live_workers); pool_live = 0;
}
static struct workqueue_struct *alloc_workqueue(const char *name, int flags, int nr)
{
    (void)name; (void)flags; (void)nr;
    if (wq_fail) return NULL;
    ++wq_live; return calloc(1, sizeof(struct workqueue_struct));
}
static void destroy_workqueue(struct workqueue_struct *wq)
{
    int i;
    assert(!live_workers && wq_live && wq);
    for (i = 0; i < wq->count; ++i) wq->pending[i]->func(wq->pending[i]);
    free(wq); --wq_live;
}
static bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
    assert(wq && wq_live && work->func && wq->count < 32);
    wq->pending[wq->count++] = work; ++wq_queued; return true;
}
static struct kthread_worker *kthread_create_worker_on_cpu(unsigned int cpu,
        int flags, const char *name, unsigned int arg)
{
    struct kthread_worker *worker;
    (void)flags; (void)name; (void)arg;
    if ((int)cpu == fail_cpu) return (void *)(intptr_t)-ENOMEM;
    worker = calloc(1, sizeof(*worker)); assert(worker);
    worker->cpu = cpu; worker->task = worker; ++live_workers;
    return worker;
}
static void sched_set_fifo_low(void *task) { assert(task); ++highpri; }
static bool kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work)
{
    assert(pins && readers && worker && !worker->pending && work->func);
    worker->pending = work; ++pcpu_queued; return true;
}
static void synchronize_rcu(void);
static void kthread_destroy_worker(struct kthread_worker *worker);
static int cpuhp_setup_state(int state, const char *name,
        int (*startup)(unsigned int), int (*teardown)(unsigned int))
{
    (void)name; assert(state == CPUHP_AP_ONLINE_DYN); ++setup_calls;
    if (setup_fail == 1) return -ENOSPC;
    startup_callback = startup; teardown_callback = teardown;
    assert(!startup(0));
    if (setup_fail == 2) {
        /* Model cpuhp_setup_state's rollback of completed startup callbacks. */
        teardown_cpu = 0; assert(!teardown(0)); teardown_cpu = -1;
        return -EINVAL;
    }
    assert(!startup(3)); return state;
}
static void cpuhp_remove_state(int state)
{
    assert(state == CPUHP_AP_ONLINE_DYN); ++remove_calls;
    teardown_cpu = 0; assert(!teardown_callback(0));
    teardown_cpu = 3; assert(!teardown_callback(3)); teardown_cpu = -1;
}

#include "erofs_workers_under_test.h"

static void z_erofs_decompressqueue_bg(struct z_erofs_decompressqueue *bgq)
{ (void)bgq; ++completed; }
static void z_erofs_decompressqueue_work(struct work_struct *work)
{ assert(work); ++completed; }
#ifdef CONFIG_EROFS_FS_PCPU_KTHREAD
static void z_erofs_decompressqueue_kthread_work(struct kthread_work *work)
{ assert(work); ++completed; }
#endif
static void synchronize_rcu(void)
{
    assert(!readers && teardown_cpu >= 0);
#ifdef CONFIG_EROFS_FS_PCPU_KTHREAD
    assert(!per_cpu(z_erofs_pcpu_workers, teardown_cpu));
#endif
    ++grace_count;
    if (inject_enqueue) {
        this_cpu = teardown_cpu;
        z_erofs_queue_decompress_work(&late_io);
        inject_enqueue = false;
    }
}
static void kthread_destroy_worker(struct kthread_worker *worker)
{
    assert(!readers && grace_count > destroyed && pool_live && wq_live);
#ifdef CONFIG_EROFS_FS_PCPU_KTHREAD
    assert(!per_cpu(z_erofs_pcpu_workers, worker->cpu));
#endif
    if (worker->pending) worker->pending->func(worker->pending);
    free(worker); --live_workers; ++destroyed;
}
static void reset(void)
{
    assert(!pool_live && !wq_live && !live_workers && !readers && !pins);
    this_cpu = 0; pool_fail = wq_fail = setup_fail = 0; fail_cpu = -1;
    destroyed = highpri = pcpu_queued = wq_queued = completed = grace_count = 0;
    setup_calls = remove_calls = 0; inject_enqueue = false;
}
int main(void)
{
    struct z_erofs_decompressqueue io = {0};
    reset(); pool_fail = 1;
    assert(z_erofs_init_zip_subsystem() == -ENOMEM && !setup_calls);
    reset(); wq_fail = 1;
    assert(z_erofs_init_zip_subsystem() == -ENOMEM && !pool_live && !setup_calls);
    reset(); assert(!z_erofs_init_zip_subsystem());
    this_cpu = 3; z_erofs_queue_decompress_work(&io);
#ifdef CONFIG_EROFS_FS_PCPU_KTHREAD
    assert(live_workers == 2 && pcpu_queued == 1 && !wq_queued);
    assert(highpri == 2 * CONFIG_EROFS_FS_PCPU_KTHREAD_HIPRI);
    teardown_cpu = 3; inject_enqueue = true;
    assert(!z_erofs_cpu_offline(3)); teardown_cpu = -1;
    assert(live_workers == 1 && wq_queued == 1 && completed == 1);
    assert(!z_erofs_cpu_online(3) && live_workers == 2);
#else
    assert(!live_workers && wq_queued == 1);
#endif
    z_erofs_exit_zip_subsystem();
    assert(completed == pcpu_queued + wq_queued);
#ifdef CONFIG_EROFS_FS_PCPU_KTHREAD
    assert(remove_calls == 1 && z_erofs_cpuhp_state == CPUHP_INVALID);
    reset(); fail_cpu = 3; assert(!z_erofs_init_zip_subsystem());
    assert(live_workers == 1 && !per_cpu(z_erofs_pcpu_workers, 3));
    this_cpu = 3; z_erofs_queue_decompress_work(&io);
    assert(wq_queued == 1 && !pcpu_queued);
    z_erofs_exit_zip_subsystem(); assert(completed == 1);
    reset(); setup_fail = 1; assert(!z_erofs_init_zip_subsystem());
    assert(!live_workers && z_erofs_cpuhp_state == CPUHP_INVALID);
    z_erofs_queue_decompress_work(&io); z_erofs_exit_zip_subsystem();
    assert(completed == 1 && !remove_calls);
    reset(); setup_fail = 2; assert(!z_erofs_init_zip_subsystem());
    assert(!live_workers && destroyed == 1 && z_erofs_cpuhp_state == CPUHP_INVALID);
    z_erofs_queue_decompress_work(&io); z_erofs_exit_zip_subsystem();
    assert(completed == 1 && !remove_calls);
#endif
    reset(); puts("EROFS worker lifecycle tests: PASS"); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="erofs-pcpu-test-") as temp:
    temp = Path(temp)
    (temp / "erofs_workers_under_test.h").write_text(workers + dispatch)
    (temp / "test.c").write_text(harness)
    for name, defines in (
        ("workqueue", []),
        ("pcpu", ["-DCONFIG_EROFS_FS_PCPU_KTHREAD=1",
                  "-DCONFIG_EROFS_FS_PCPU_KTHREAD_HIPRI=0"]),
        ("pcpu-hipri", ["-DCONFIG_EROFS_FS_PCPU_KTHREAD=1",
                        "-DCONFIG_EROFS_FS_PCPU_KTHREAD_HIPRI=1"]),
    ):
        binary = temp / name
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O2",
                        "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        *defines, str(temp / "test.c"), "-o", str(binary)],
                       check=True)
        print(name, flush=True)
        subprocess.run([str(binary)], check=True)
