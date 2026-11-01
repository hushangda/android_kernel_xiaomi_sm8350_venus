#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the production MILLET predicate and cgroup-v2 lookup on the host.

The task state and legacy freezer are mocked.  This verifies v1/v2 dispatch
and balanced RCU lookup, not netlink delivery or Greeze's thaw policy.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def function(source, signature, start=0):
    pos = source.index(signature, start)
    brace = source.index("{", pos)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[pos:end]


millet = (ROOT / "drivers/mihw/millet.h").read_text()
cgroup = (ROOT / "include/linux/cgroup.h").read_text()
predicate = function(millet, "static inline int frozen_task_group")
lookup_sig = "static inline bool cgroup_task_freeze("
lookup = function(cgroup, lookup_sig)
stub = function(cgroup, lookup_sig, cgroup.index(lookup_sig) + len(lookup_sig))
harness = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define PF_KTHREAD 1
#define CGRP_FREEZE 0
struct cgroup { unsigned long flags; };
struct task_struct {
    unsigned long flags;
    bool legacy_request, legacy_frozen;
    struct cgroup *cgroup;
};
static int readers, lookups;
static void rcu_read_lock(void) { ++readers; }
static void rcu_read_unlock(void) { if (--readers) abort(); }
static struct cgroup *task_dfl_cgroup(struct task_struct *t)
{
    if (readers != 1) abort();
    ++lookups;
    return t->cgroup;
}
static bool test_bit(int bit, unsigned long *flags)
{ return !!(*flags & (1UL << bit)); }
static bool freezing(struct task_struct *t)
{ return LEGACY && t->legacy_request; }
static bool frozen(struct task_struct *t) { return t->legacy_frozen; }
#include "predicate.h"
static int failures;
static void check(const char *name, struct task_struct *t, bool want)
{
    bool got = frozen_task_group(t);
    if (got != want) {
        fprintf(stderr, "%s: expected %d, got %d\n", name, want, got);
        ++failures;
    }
    if (readers) abort();
}
int main(void)
{
    struct cgroup cg = {0};
    struct task_struct t = {.cgroup = &cg};
    check("running", &t, false);
    t.legacy_request = true;
    check("legacy requested", &t, LEGACY);
    t.legacy_request = false;
    t.legacy_frozen = true;
    check("legacy frozen", &t, true);
    t.legacy_frozen = false;
    cg.flags = 1UL << CGRP_FREEZE;
    check("unified freeze request without PF_FROZEN", &t, UNIFIED);
    /* Same state while in do_freezer_trap, which never sets PF_FROZEN. */
    check("unified fully frozen without PF_FROZEN", &t, UNIFIED);
    cg.flags = 0;
    check("unified thawed", &t, false);
    cg.flags = 1UL << CGRP_FREEZE;
    t.flags = PF_KTHREAD;
    check("unified kernel thread exempt", &t, false);
    if (UNIFIED && !lookups && !failures) abort();
    return failures ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory(prefix="venus-millet-test-") as tmp:
    tmp = Path(tmp)
    (tmp / "test.c").write_text(harness)
    cc = os.environ.get("HOSTCC", "cc")
    for unified, legacy in [(0, 0), (1, 0), (1, 1)]:
        (tmp / "predicate.h").write_text((lookup if unified else stub) + "\n" + predicate)
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Wno-unused-function",
                        "-Wno-unused-parameter", f"-DUNIFIED={unified}",
                        f"-DLEGACY={legacy}", str(tmp / "test.c"),
                        "-o", str(tmp / "test")], check=True)
        subprocess.run([str(tmp / "test")], check=True)
        print(f"PASS CONFIG_CGROUPS={unified} CONFIG_CGROUP_FREEZER={legacy}: 7 cases", flush=True)
    old = "static inline int frozen_task_group(struct task_struct *t) { return freezing(t) || frozen(t); }"
    (tmp / "predicate.h").write_text(lookup + "\n" + old)
    subprocess.run([cc, "-std=c11", "-DUNIFIED=1", "-DLEGACY=1",
                    str(tmp / "test.c"), "-o", str(tmp / "old")], check=True)
    result = subprocess.run([str(tmp / "old")], capture_output=True, text=True)
    # The old predicate never calls the lookup and fails the two v2 cases.
    assert result.returncode != 0
    assert result.stderr.count("expected 1, got 0") == 2, result.stderr
    print("PASS negative control: old predicate misses both unified-freezer cases")
