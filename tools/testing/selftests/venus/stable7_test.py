#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise seven backported fixes using production C and controlled races.

Locks, CPU state and I/O are bounded host models. The pre-fix commit is a
negative control; this is not an on-device filesystem stress test.
The ZRAM test extracts the actual locked setup up to the table scan, replacing
only the I/O loop with a bounds observer. ASan checks the EROFS lifetime case.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
BASELINE = os.environ.get("STABLE7_BASELINE", "1a61ec64eaf9958919a411ceef244d5a90333458")


def function(source, name):
    match = re.search(r"(?m)^[\w *\n]+\b" + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(name)
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#define unlikely(x) (x)
#define likely(x) (x)
#define BUG_ON(x) assert(!(x))
#define IS_ENABLED(x) (x)
#define NOOP(...) ((void)0)
'''

RT = r'''
#define RT_MAX_TRIES 3
#define nr_cpu_ids 4
typedef unsigned cpumask_t;
struct task_struct { int cpu, prio, nr_cpus_allowed, queued, rt, running;
    const cpumask_t *cpus_ptr; };
struct rq { int cpu; struct { struct { int curr; } highest_prio;
    struct task_struct *pushable_tasks; } rt; };
static struct rq queues[4];
static cpumask_t mask = 15;
static int destination, dropped, interleave, unlocks, this_cpu;
static bool has_pushable_tasks(struct rq *r) { return r->rt.pushable_tasks; }
#define plist_first_entry(p, type, field) (*(p))
static int task_cpu(struct task_struct *p) { return p->cpu; }
static bool task_current(struct rq *r, struct task_struct *p) { return p->running; }
static bool task_on_rq_queued(struct task_struct *p) { return p->queued; }
static bool rt_task(struct task_struct *p) { return p->rt; }
static bool task_running(struct rq *r, struct task_struct *p) { return p->running; }
static struct rq *task_rq(struct task_struct *p)
{
    struct rq *old = &queues[p->cpu];
    /* Wakeup changes CPU and on_rq between the old independent observations. */
    if (interleave) { p->cpu = 2; p->queued = 1; }
    return old;
}
static bool cpumask_test_cpu(int cpu, const cpumask_t *m) { return *m & (1U << cpu); }
static int find_lowest_rq(struct task_struct *p) { return destination; }
static struct rq *cpu_rq(int cpu) { return &queues[cpu]; }
static int double_lock_balance(struct rq *a, struct rq *b)
{
    if (interleave) { a->rt.pushable_tasks->queued = 0; a->rt.pushable_tasks = NULL; }
    return dropped;
}
static void double_unlock_balance(struct rq *a, struct rq *b) { ++unlocks; }
typedef int atomic_t;
struct root_domain { int rto_cpu, rto_loop; atomic_t rto_loop_next;
    const cpumask_t *rto_mask; };
static int smp_processor_id(void) { return this_cpu; }
static int cpumask_next(int cpu, const cpumask_t *m)
{ while (++cpu < nr_cpu_ids) if (*m & (1U << cpu)) return cpu; return nr_cpu_ids; }
static int atomic_read_acquire(atomic_t *v) { return *v; }
@FUNCTIONS@
int main(int argc, char **argv)
{
    if (!strcmp(argv[1], "push")) {
        struct task_struct p = { .cpu=1, .prio=10, .nr_cpus_allowed=4,
            .queued=1, .rt=1, .cpus_ptr=&mask };
        for (int i=0; i<4; ++i) { queues[i].cpu=i; queues[i].rt.highest_prio.curr=50; }
        destination=3; dropped=1; queues[1].rt.pushable_tasks=&p;
        assert(find_lock_lowest_rq(&p, &queues[1]) == &queues[3]);
        mask=3; assert(!find_lock_lowest_rq(&p, &queues[1])); mask=15;
        destination=-1; assert(!find_lock_lowest_rq(&p, &queues[1])); destination=3;
        interleave=1;
        assert(!find_lock_lowest_rq(&p, &queues[1]));
        assert(unlocks == 2);
    } else {
        /* Exhaust all masks/start CPUs, including wrap and a new scan round. */
        for (this_cpu=0; this_cpu<4; ++this_cpu)
            for (mask=0; mask<16; ++mask)
                for (int start=-1; start<4; ++start) {
                    struct root_domain rd = { .rto_cpu=start, .rto_loop=1,
                        .rto_loop_next=2, .rto_mask=&mask };
                    int cpu=rto_next_cpu(&rd);
                    assert(cpu == -1 || (cpu != this_cpu && cpumask_test_cpu(cpu, &mask)));
                    if (mask == (1U << this_cpu)) assert(cpu == -1);
                }
    }
    return 0;
}
'''

EROFS = r'''
#define CONFIG_PREEMPTION 1
#define CONFIG_PREEMPT_COUNT 1
typedef int atomic_t;
struct erofs_sb_info { bool readahead_sync_decompress; };
struct z_erofs_decompressqueue { struct erofs_sb_info *sb;
    atomic_t pending_bios; union { struct { int lock; } wait; } u; };
#define EROFS_SB(sb) (sb)
static int depth, pc, irq, queued, direct, woken, unmount_now;
static int rcu_preempt_depth(void) { return depth; }
static bool preemptible(void) { return !pc && !irq; }
static bool in_atomic(void) { return pc; }
static bool irqs_disabled(void) { return irq; }
static int atomic_add_return(int n, atomic_t *p) { return *p += n; }
#define spin_lock_irqsave(lock, flags) ((flags)=0)
#define spin_unlock_irqrestore(lock, flags) ((void)(flags))
#define wake_up_locked(wait) (++woken)
static void z_erofs_queue_decompress_work(struct z_erofs_decompressqueue *io)
{
    ++queued;
    /* Worker releases the last page; concurrent unmount can now free sbi. */
    if (unmount_now) { free(io->sb); free(io); }
}
static void z_erofs_decompressqueue_bg(struct z_erofs_decompressqueue *io)
{ ++direct; assert(!depth && !pc && !irq); }
@FUNCTIONS@
int main(int argc, char **argv)
{
    if (!strcmp(argv[1], "lifetime")) {
        struct z_erofs_decompressqueue *io=calloc(1, sizeof(*io));
        io->sb=calloc(1, sizeof(*io->sb));
        pc=1; unmount_now=1;
        z_erofs_decompress_kickoff(io, false, 0);
        assert(queued == 1);
    } else {
        struct erofs_sb_info sbi={0};
        struct z_erofs_decompressqueue io={.sb=&sbi};
        for (depth=0; depth<3; ++depth)
            for (pc=0; pc<2; ++pc)
                for (irq=0; irq<2; ++irq) {
                    queued=direct=0; sbi.readahead_sync_decompress=false;
                    z_erofs_decompress_kickoff(&io, false, 0);
                    bool async=depth || pc || irq;
                    assert(queued == async && direct == !async);
                    assert(sbi.readahead_sync_decompress == async);
                }
        queued=direct=0; io.pending_bios=2;
        z_erofs_decompress_kickoff(&io, false, -1);
        assert(!queued && !direct);
        z_erofs_decompress_kickoff(&io, true, -1);
        assert(woken == 1 && !queued && !direct);
    }
    return 0;
}
'''

ZRAM = r'''
#define PAGE_SHIFT 12
#define GFP_KERNEL 0
#define HUGE_WRITEBACK 1
#define IDLE_WRITEBACK 2
struct page { int dummy; };
struct bio { int dummy; };
struct bio_vec { int dummy; };
struct device_attribute { int dummy; };
struct zram { unsigned long disksize; int init_lock, initialized, backing_dev; };
struct device { struct zram *zram; };
static struct zram z;
static struct page scratch;
static int reset_between_snapshot_and_lock, held;
static struct zram *dev_to_zram(struct device *d) { return d->zram; }
static bool sysfs_streq(const char *a,const char *b) { return !strcmp(a,b); }
static void down_read(int *lock)
{
    assert(!held); held=1;
    if (reset_between_snapshot_and_lock) z.disksize=2UL<<PAGE_SHIFT;
}
static void up_read(int *lock) { assert(held); held=0; }
static bool init_done(struct zram *zram) { return zram->initialized; }
static struct page *alloc_page(int flags) { assert(held); return &scratch; }
@FUNCTIONS@
int main(void)
{
    struct device dev={.zram=&z};
    z=(struct zram){.disksize=16UL<<PAGE_SHIFT, .initialized=1, .backing_dev=1};
    assert(writeback_store(&dev, NULL, "invalid", 7) == -EINVAL && !held);
    assert(writeback_store(&dev, NULL, "idle", 4) == 4 && !held);
    reset_between_snapshot_and_lock=1;
    assert(writeback_store(&dev, NULL, "huge", 4) == 4 && !held);
    z.initialized=0;
    assert(writeback_store(&dev, NULL, "idle", 4) == -EINVAL && !held);
    return 0;
}
'''

NAT = r'''
typedef unsigned nid_t;
struct nat_entry { unsigned flags; };
struct f2fs_nm_info { int nat_tree_lock; };
struct f2fs_sb_info { int node_write; struct f2fs_nm_info nm; };
#define NM_I(sbi) (&(sbi)->nm)
#define HAS_LAST_FSYNC 1
#define IS_CHECKPOINTED 2
#define HAS_FSYNCED_INODE 4
static struct f2fs_sb_info s;
static struct nat_entry entry;
static int cp_pending, checkpoint_committed, node_held, nat_held, present=1;
static void f2fs_down_read(int *lock)
{
    if (lock == &s.node_write) {
        assert(!node_held && !nat_held);
        /* A reader waits for the writer through checkpoint completion. */
        if (cp_pending) { cp_pending=0; checkpoint_committed=1; }
        node_held=1;
    } else { assert(!nat_held); nat_held=1; }
}
static void f2fs_up_read(int *lock)
{ if (lock == &s.node_write) { assert(node_held && !nat_held); node_held=0; }
  else { assert(nat_held); nat_held=0; } }
static struct nat_entry *__lookup_nat_cache(struct f2fs_nm_info *nm,nid_t ino)
{ assert(nat_held); return present ? &entry : NULL; }
static bool get_nat_flag(struct nat_entry *e,unsigned f) { return e->flags & f; }
@FUNCTIONS@
int main(void)
{
    for (entry.flags=0; entry.flags<8; ++entry.flags) {
        bool expected=!(entry.flags & HAS_LAST_FSYNC) ||
            !(entry.flags & (IS_CHECKPOINTED | HAS_FSYNCED_INODE));
        assert(f2fs_need_inode_block_update(&s,42) == expected);
        assert(!node_held && !nat_held);
    }
    present=0; assert(f2fs_need_inode_block_update(&s,42)); present=1;
    entry.flags=HAS_LAST_FSYNC | IS_CHECKPOINTED;
    cp_pending=1; checkpoint_committed=0;
    assert(!f2fs_need_inode_block_update(&s,42));
    assert(checkpoint_committed && !cp_pending && !node_held && !nat_held);
    return 0;
}
'''

FSYNC = r'''
typedef unsigned long pgoff_t;
typedef unsigned nid_t;
struct page { void *mapping; unsigned long index; int refs, locked, dirty; };
struct pagevec { struct page *pages[1]; };
struct inode { unsigned i_ino; };
struct writeback_control { int dummy; };
struct f2fs_sb_info { int mapping, rf_node_block_count; };
static struct page last;
static int empty, scans, locks, detached;
#define NODE_MAPPING(sbi) (&(sbi)->mapping)
#define IS_ERR_OR_NULL(p) (!(p))
#define PTR_ERR_OR_ZERO(p) 0
#define PAGECACHE_TAG_DIRTY 1
#define NODE 1
#define FS_NODE_IO 1
#define FI_DIRTY_INODE 1
#define IS_DNODE(p) 1
#define is_cold_node(p) 1
#define IS_INODE(p) 0
#define ino_of_node(p) 42
#define f2fs_cp_error(sbi) false
#define PageDirty(p) ((p)->dirty)
#define is_inode_flag_set(inode,flag) 0
#define f2fs_update_inode(...) NOOP()
#define f2fs_need_dentry_mark(...) 0
#define f2fs_debug(...) NOOP()
#define f2fs_wait_on_page_writeback(...) NOOP()
#define f2fs_submit_merged_write_cond(...) NOOP()
#define cond_resched() NOOP()
#define pagevec_release(p) NOOP()
#define pagevec_init(p) NOOP()
#define set_fsync_mark(p,v) NOOP()
#define set_dentry_mark(p,v) NOOP()
#define percpu_counter_inc(p) (++*(p))
static struct page *last_fsync_dnode(struct f2fs_sb_info *s,nid_t ino)
{ return empty ? NULL : &last; }
static int pagevec_lookup_tag(struct pagevec *v,void *m,pgoff_t *idx,int tag)
{ if (++scans > 4) exit(77); if (detached || empty || scans>1) return 0;
  v->pages[0]=&last; return 1; }
static void lock_page(struct page *p) { assert(!p->locked); p->locked=1; ++locks; }
static void unlock_page(struct page *p) { assert(p->locked); p->locked=0; }
static void f2fs_put_page(struct page *p,int unlock)
{ if (!p) return; if (unlock) unlock_page(p); assert(p->refs>0); --p->refs; }
static void set_page_dirty(struct page *p) { p->dirty=1; }
static bool clear_page_dirty_for_io(struct page *p) { p->dirty=0; return true; }
static int __write_node_page(struct page *p,bool atomic,bool *submitted,
        struct writeback_control *w,bool sync,int type,unsigned *seq)
{ *submitted=true; unlock_page(p); return 0; }
@FUNCTIONS@
int main(void)
{
    struct f2fs_sb_info s={0}; struct inode ino={42};
    struct writeback_control w={0}; unsigned seq=0;
    empty=1; assert(!f2fs_fsync_node_pages(&s,&ino,&w,true,&seq));
    empty=0; last=(struct page){.mapping=NODE_MAPPING(&s),.refs=1,.dirty=1};
    assert(!f2fs_fsync_node_pages(&s,&ino,&w,true,&seq));
    assert(!last.refs && !last.locked);
    /* truncate invalidated the referenced last dnode before fsync scans it. */
    detached=1; scans=locks=0; last=(struct page){.refs=1};
    assert(f2fs_fsync_node_pages(&s,&ino,&w,true,&seq) == -EIO);
    assert(scans==1 && locks==1 && !last.refs && !last.locked);
    return 0;
}
'''


def make_sources(read):
    rt = read("kernel/sched/rt.c")
    erofs = read("fs/erofs/zdata.c")
    zram = function(read("drivers/block/zram/zram_drv.c"), "writeback_store")
    node = read("fs/f2fs/node.c")
    # Observe the bounds at entry to the actual table scan; no I/O is modelled.
    zram = zram[:zram.index("\tfor (index = 0; index < nr_pages; index++)")]
    zram += "(void)mode; (void)page;\nassert(nr_pages == zram->disksize >> PAGE_SHIFT);\n" \
            "release_init_lock: up_read(&zram->init_lock); return ret; }\n"
    atomic = function(erofs, "z_erofs_in_atomic") if "bool z_erofs_in_atomic" in erofs else ""
    return {
        "rt": RT.replace("@FUNCTIONS@", "\n".join(function(rt, n) for n in (
            "pick_next_pushable_task", "find_lock_lowest_rq", "rto_next_cpu"))),
        "erofs": EROFS.replace("@FUNCTIONS@", atomic + function(erofs, "z_erofs_decompress_kickoff")),
        "zram": ZRAM.replace("@FUNCTIONS@", zram),
        "nat": NAT.replace("@FUNCTIONS@", function(node, "f2fs_need_inode_block_update")),
        "fsync": FSYNC.replace("@FUNCTIONS@", function(node, "f2fs_fsync_node_pages")),
    }


def main():
    cases = [("rt", "push"), ("rt", "ipi"), ("erofs", "context"),
             ("erofs", "lifetime"), ("zram", "bounds"), ("nat", "checkpoint"),
             ("fsync", "truncate")]
    with tempfile.TemporaryDirectory(prefix="venus-stable7-") as tmp:
        tmp = Path(tmp)
        for version, read in (
            ("fixed", lambda path: (ROOT / path).read_text()),
            ("baseline", lambda path: subprocess.check_output(
                ["git", "show", BASELINE + ":" + path], cwd=ROOT, text=True)),
        ):
            for name, body in make_sources(read).items():
                src = tmp / (version + "-" + name + ".c")
                exe = src.with_suffix("")
                src.write_text(COMMON + body)
                subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-O1",
                                "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                                "-Wno-unused-function", "-Wno-unused-variable",
                                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                                str(src), "-o", str(exe)], check=True)
                for group, case in cases:
                    if group != name:
                        continue
                    result = subprocess.run([str(exe), case], capture_output=True,
                                            text=True, timeout=5)
                    if version == "fixed" and result.returncode:
                        raise RuntimeError(name + "/" + case + "\n" + result.stderr)
                    if version == "baseline":
                        if not result.returncode:
                            raise RuntimeError("Negative control unexpectedly passed: " + name + "/" + case)
                        if case == "lifetime" and "heap-use-after-free" not in result.stderr:
                            raise RuntimeError("Expected ASan UAF, got: " + result.stderr)
                        if case == "truncate" and result.returncode != 77:
                            raise RuntimeError("Expected bounded retry-loop detector: " + result.stderr)
                    print(f"PASS {version}: {name}/{case}", flush=True)
    print("Seven fixed paths passed; all seven baseline defects reproduced.")


if __name__ == "__main__":
    main()
