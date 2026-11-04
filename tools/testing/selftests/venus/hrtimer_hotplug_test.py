#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the real hrtimer enqueue/hotplug code and rbtrees on the host.

CPU state, locks, clocks and IPIs are deterministic stubs. This reproduces
late enqueue after CPU teardown and queue loss on bringup, not hardware PM.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
SOURCE = "kernel/time/hrtimer.c"


def function(source, name):
    match = re.search(r"(?m)^[\w *\n]+\b" + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise RuntimeError(name)
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]

PRELUDE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int64_t ktime_t;
typedef uint64_t u64;
typedef uint8_t u8;
#define CONFIG_SMP 1
#define CONFIG_NO_HZ_COMMON 1
#define CONFIG_HOTPLUG_CPU 1
#define IS_ENABLED(x) (x)
#define likely(x) (x)
#define unlikely(x) (x)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define smp_load_acquire(p) (*(p))
#define rcu_assign_pointer(p, v) ((p) = (v))
#define container_of(p, type, field) ((type *)((char *)(p) - offsetof(type, field)))
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define __init
#define HRTIMER_MAX_CLOCK_BASES 8
#define HRTIMER_ACTIVE_SOFT 0xf0
#define HRTIMER_STATE_ENQUEUED 1
#define HRTIMER_STATE_PINNED 2
#define HRTIMER_STATE_INACTIVE 0
#define HRTIMER_PINNED_SHIFT 1
#define KTIME_MAX INT64_MAX
#define SINGLE_DEPTH_NESTING 1
#define HK_FLAG_TIMER 1
enum hrtimer_mode { HRTIMER_MODE_ABS, HRTIMER_MODE_REL,
    HRTIMER_MODE_PINNED, HRTIMER_MODE_ABS_PINNED_HARD = 0x12 };
static unsigned warnings;
#define WARN_ON_ONCE(x) ({ bool bad = !!(x); warnings += bad; bad; })
#define BUG_ON(x) assert(!(x))
#define debug_activate(t, m) ((void)0)
#define debug_deactivate(t) ((void)0)
"""

SUPPORT = r"""
struct hrtimer;
struct hrtimer_cpu_base;
struct hrtimer_clock_base {
    struct hrtimer_cpu_base *cpu_base;
    unsigned index, seq;
    struct hrtimer *running;
    struct timerqueue_head active;
    ktime_t (*get_time)(void);
    ktime_t offset;
};
struct hrtimer {
    struct timerqueue_node node;
    ktime_t _softexpires;
    struct hrtimer_clock_base *base;
    u8 state;
};
struct hrtimer_cpu_base {
    int lock;
    unsigned cpu, active_bases;
    bool online, hres_active, hang_detected;
    struct hrtimer *next_timer, *softirq_next_timer;
    ktime_t expires_next, softirq_expires_next;
    struct hrtimer_clock_base clock_base[HRTIMER_MAX_CLOCK_BASES];
};
typedef struct { unsigned flags; } call_single_data_t;
static struct hrtimer_cpu_base hrtimer_bases[4], migration_cpu_base;
static call_single_data_t hrtimer_retrigger_csd[4];
#define migration_base migration_cpu_base.clock_base[0]
static unsigned current_cpu, online_mask, kicks, forced, nohz_cpu;
static bool timers_migration_enabled;
#define this_cpu_ptr(p) (&(*(p))[current_cpu])
#define per_cpu(p, cpu) ((p)[cpu])
#define static_branch_likely(p) (*(p))
#define cpu_online_mask (&online_mask)
#define cpu_active_mask (&online_mask)
static int cpu_online(unsigned cpu) { return !!(online_mask & (1U << cpu)); }
static unsigned cpumask_first(const unsigned *m) { assert(*m); return __builtin_ctz(*m); }
static unsigned cpumask_any_and(const unsigned *a, const unsigned *b)
{ unsigned m = *a & *b; return cpumask_first(&m); }
static const unsigned *housekeeping_cpumask(int flag) { return &online_mask; }
static unsigned get_nohz_timer_target(void) { return nohz_cpu; }
static ktime_t clock_now(void) { return 1000; }
static ktime_t ktime_sub(ktime_t a, ktime_t b) { return a - b; }
static ktime_t ktime_add_safe(ktime_t a, ktime_t b) { return a + b; }
static ktime_t hrtimer_get_expires(struct hrtimer *t) { return t->node.expires; }
static bool hrtimer_callback_running(struct hrtimer *t) { return t->base->running == t; }
static void raw_spin_lock(int *l) { assert(!*l); *l = 1; }
static void raw_spin_unlock(int *l) { assert(*l); *l = 0; }
#define raw_spin_lock_nested(l, n) raw_spin_lock(l)
#define seqcount_raw_spinlock_init(s, l) (*(s) = 0)
static void hrtimer_cpu_base_init_expiry_lock(struct hrtimer_cpu_base *b) {}
static void tick_cancel_sched_timer(unsigned cpu) {}
static void retrigger_next_event(void *arg) {}
static int smp_call_function_single(unsigned cpu, void (*fn)(void *), void *arg, int wait)
{ return 0; }
static int smp_call_function_single_async(unsigned cpu, call_single_data_t *csd)
{
    assert(cpu_online(cpu));
    assert(hrtimer_bases[cpu].lock);
    /* A busy CSD would make the old 5.4 API wait with IRQs disabled. */
    assert(!csd->flags);
    csd->flags = 1;
    kicks++;
    return 0;
}
static void hrtimer_force_reprogram(struct hrtimer_cpu_base *b, int skip)
{ assert(b->lock); forced++; }
static ktime_t __hrtimer_get_next_event(struct hrtimer_cpu_base *b, unsigned mask)
{ return KTIME_MAX; }
static ktime_t hrtimer_update_lowres(struct hrtimer *t, ktime_t tim, enum hrtimer_mode mode)
{ return tim; }
static void hrtimer_set_expires_range_ns(struct hrtimer *t, ktime_t tim, u64 delta)
{ t->_softexpires = tim; t->node.expires = tim + delta; }
"""

TESTS = r"""
static void reset(void)
{
    memset(hrtimer_bases, 0, sizeof(hrtimer_bases));
    memset(hrtimer_retrigger_csd, 0, sizeof(hrtimer_retrigger_csd));
    online_mask = 0xf; kicks = forced = warnings = 0;
    nohz_cpu = 0; timers_migration_enabled = false;
    for (current_cpu = 0; current_cpu < 4; current_cpu++) {
        hrtimers_prepare_cpu(current_cpu);
        hrtimers_cpu_starting(current_cpu);
        for (unsigned i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++) {
            hrtimer_bases[current_cpu].clock_base[i].index = i;
            hrtimer_bases[current_cpu].clock_base[i].get_time = clock_now;
        }
    }
    current_cpu = 3;
}
static void init_timer(struct hrtimer *t, unsigned cpu, unsigned index)
{
    memset(t, 0, sizeof(*t));
    timerqueue_init(&t->node);
    t->base = &hrtimer_bases[cpu].clock_base[index];
}
static int start(struct hrtimer *t, ktime_t expires, enum hrtimer_mode mode)
{
    raw_spin_lock(&t->base->cpu_base->lock);
    int ret = __hrtimer_start_range_ns(t, expires, 0, mode, t->base);
    raw_spin_unlock(&t->base->cpu_base->lock);
    return ret;
}
static bool reachable(struct hrtimer *t)
{
    for (struct timerqueue_node *n = timerqueue_getnext(&t->base->active);
         n; n = timerqueue_iterate_next(n))
        if (n == &t->node) return true;
    return false;
}
static void late_enqueue(unsigned index, bool stale_next, bool migrating)
{
    struct hrtimer rt, tick;
    reset(); timers_migration_enabled = migrating;
    init_timer(&rt, 3, index);
    online_mask = 1;
    assert(!hrtimers_cpu_dying(3));
    assert(!hrtimer_bases[3].online);
    if (stale_next) hrtimer_bases[3].next_timer = &rt;
    start(&rt, 2000, HRTIMER_MODE_ABS_PINNED_HARD);
    assert(reachable(&rt));
    /* This is the real CPU bringup path that discards the late timer. */
    hrtimers_prepare_cpu(3);
    hrtimers_cpu_starting(3);
    online_mask |= 8;
    init_timer(&tick, 3, index);
    start(&tick, 3000, HRTIMER_MODE_ABS_PINNED_HARD);
    if (EXPECT_OLD) {
        assert(!reachable(&rt));
        assert(rt.state == (HRTIMER_STATE_ENQUEUED | HRTIMER_STATE_PINNED));
        assert(!rb_parent(&rt.node.node));
        assert(!rt.node.node.rb_left && !rt.node.node.rb_right);
        assert(warnings);
    } else {
        assert(rt.base->cpu_base->cpu == 0);
        assert(reachable(&rt));
        assert(kicks == 1 && !warnings);
    }
}
static void normal_online(void)
{
    struct hrtimer t;
    reset(); timers_migration_enabled = true;
    init_timer(&t, 3, 0);
    start(&t, 2000, HRTIMER_MODE_ABS_PINNED_HARD);
    assert(t.base->cpu_base->cpu == 3 && !kicks && reachable(&t));
    reset(); timers_migration_enabled = true;
    init_timer(&t, 3, 0);
    start(&t, 2000, HRTIMER_MODE_ABS);
    assert(t.base->cpu_base->cpu == 3 && !kicks && reachable(&t));
    reset(); timers_migration_enabled = true;
    hrtimer_bases[0].expires_next = 100;
    init_timer(&t, 3, 0);
    start(&t, 2000, HRTIMER_MODE_ABS);
    assert(t.base->cpu_base->cpu == 0 && !kicks && reachable(&t));
}
static void pending_ipi(void)
{
    struct hrtimer timers[32];
    reset(); online_mask = 1; hrtimers_cpu_dying(3);
    for (unsigned i = 0; i < 32; i++) {
        init_timer(&timers[i], 3, 0);
        start(&timers[i], 5000 - i, HRTIMER_MODE_ABS_PINNED_HARD);
    }
    assert(kicks == 1 && !warnings);
    for (unsigned i = 0; i < 32; i++) {
        assert(timers[i].base->cpu_base->cpu == 0);
        assert(reachable(&timers[i]));
    }
    /* Model async consumption: it unlocks the CSD before its callback. */
    hrtimer_retrigger_csd[0].flags = 0;
    start(&timers[31], 1000, HRTIMER_MODE_ABS_PINNED_HARD);
    assert(kicks == 2);
}
static void not_first(void)
{
    struct hrtimer early, late;
    reset(); current_cpu = 0;
    init_timer(&early, 0, 0);
    start(&early, 100, HRTIMER_MODE_ABS_PINNED_HARD);
    current_cpu = 3; online_mask = 1; hrtimers_cpu_dying(3);
    init_timer(&late, 3, 0);
    assert(!start(&late, 200, HRTIMER_MODE_ABS_PINNED_HARD));
    assert(!kicks && !warnings && reachable(&early) && reachable(&late));
}
static void existing_timer_migration(void)
{
    struct hrtimer t;
    reset(); init_timer(&t, 3, 0);
    start(&t, 2000, HRTIMER_MODE_ABS_PINNED_HARD);
    online_mask = 1; hrtimers_cpu_dying(3);
    hrtimers_prepare_cpu(3);
    assert(t.base->cpu_base->cpu == 0 && reachable(&t));
}
int main(void)
{
    for (unsigned index = 0; index < HRTIMER_MAX_CLOCK_BASES; index++)
        for (unsigned flags = 0; flags < 4; flags++)
            late_enqueue(index, flags & 1, flags & 2);
    normal_online(); existing_timer_migration();
    if (!EXPECT_OLD) { pending_ipi(); not_first(); }
    puts(EXPECT_OLD ? "PASS: old code reproduces ENQUEUED|PINNED orphan after CPU bringup (32 cases)"
                    : "PASS: late timers survive hotplug (32 cases); online policy, migration and IPI coalescing pass");
    return 0;
}
"""


def without_includes(source):
    return re.sub(r"^#include[^\n]*", "", source, flags=re.M)


def build_and_run(source, out, name, old):
    parts = [PRELUDE]
    for path in ("include/linux/rbtree.h", "include/linux/rbtree_augmented.h",
                 "include/linux/timerqueue.h", "lib/rbtree.c", "lib/timerqueue.c"):
        parts.append(without_includes((ROOT / path).read_text()))
    parts.append(SUPPORT)
    names = ["hrtimer_base_is_online", "hrtimer_check_target", "hrtimer_suitable_target",
             "get_target_base", "switch_hrtimer_base", "enqueue_hrtimer",
             "__remove_hrtimer", "remove_hrtimer", "__hrtimer_start_range_ns",
             "hrtimers_prepare_cpu", "hrtimers_cpu_starting",
             "migrate_hrtimer_list", "hrtimers_cpu_dying"]
    for name_func in names:
        if name_func + "(" in source:
            parts.append(function(source, name_func))
    parts.append(TESTS)
    cfile, binary = out / (name + ".c"), out / name
    cfile.write_text("\n".join(parts))
    subprocess.run(["gcc", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-parameter", "-Wno-unused-function",
                    "-Wno-unused-variable", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-no-pie",
                    f"-DEXPECT_OLD={int(old)}", str(cfile), "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    (out / (name + ".log")).write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-revision")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    out = args.output_dir or Path(tempfile.mkdtemp(prefix="venus-hrtimer-test-"))
    out.mkdir(parents=True, exist_ok=True)
    if args.old_revision:
        old = subprocess.check_output(["git", "show", f"{args.old_revision}:{SOURCE}"],
                                      cwd=ROOT, text=True)
        build_and_run(old, out, "original", True)
    build_and_run((ROOT / SOURCE).read_text(), out, "fixed", False)
    print(f"Evidence: {out}")


if __name__ == "__main__":
    main()
