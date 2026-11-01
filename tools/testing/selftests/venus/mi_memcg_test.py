#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual memcg accounting and ZRAM ownership helpers under sanitizers.

Only the kernel environment and object compressor are substituted. Source is
extracted from the current tree, so replacement/refcount code is not mirrored.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]

def function(text, name):
    start = text.index(name + '(')
    start = text.rfind('\n', 0, start) + 1
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'

header = '\n'.join(x for x in (ROOT / 'include/linux/mi_memcg.h').read_text().splitlines()
                   if not x.startswith('#include'))
account = (ROOT / 'mm/mi_memcg.c').read_text()
zram = (ROOT / 'drivers/block/zram/zram_drv.c').read_text()

env = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stddef.h>
#define CONFIG_XIAOMI_MEMCG_RECLAIM 1
#define PAGE_SIZE 4096
#define U64_MAX UINT64_MAX
typedef uint64_t u64;
typedef uint32_t u32;
typedef pthread_mutex_t spinlock_t;
#define spin_lock_init(p) assert(!pthread_mutex_init(p, NULL))
#define spin_lock_irqsave(p, f) do { (f) = 0; assert(!pthread_mutex_lock(p)); } while (0)
#define spin_unlock_irqrestore(p, f) do { (void)(f); assert(!pthread_mutex_unlock(p)); } while (0)
#define spin_lock(p) assert(!pthread_mutex_lock(p))
#define spin_unlock(p) assert(!pthread_mutex_unlock(p))
#define DEFINE_SPINLOCK(n) spinlock_t n = PTHREAD_MUTEX_INITIALIZER
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(n) struct list_head n = { &n, &n }
static void INIT_LIST_HEAD(struct list_head *h) { h->next = h->prev = h; }
static bool list_empty(struct list_head *h) { return h->next == h; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{ n->next = h; n->prev = h->prev; h->prev->next = n; h->prev = n; }
static void list_del_init(struct list_head *n)
{ n->next->prev = n->prev; n->prev->next = n->next; INIT_LIST_HEAD(n); }
#define list_entry(p, t, m) container_of(p, t, m)
#define list_for_each(p, h) for ((p) = (h)->next; (p) != (h); (p) = (p)->next)
#define WRITE_ONCE(x, v) ((x) = (v))
static unsigned int warnings;
#define WARN_ON_ONCE(x) ((x) ? (warnings++, true) : false)
'''
objects = r'''
struct cgroup_subsys_state { atomic_uint refs; };
static int nr_node_ids = 1;
struct mem_cgroup { struct cgroup_subsys_state css; struct mem_cgroup *ancestor; void *nodeinfo[0]; };
struct memcg_fixture { struct mem_cgroup m; void *nodeinfo[1]; struct mi_memcg data; };
static struct mem_cgroup *parent_mem_cgroup(struct mem_cgroup *m) { return m->ancestor; }
static LIST_HEAD(mi_memcg_list);
static DEFINE_SPINLOCK(mi_memcg_list_lock);
struct page { struct mem_cgroup *memcg; bool swapcache; };
static _Thread_local bool in_rcu;
static bool disabled;
static bool mem_cgroup_disabled(void) { return disabled; }
static void rcu_read_lock(void) { assert(!in_rcu); in_rcu = true; }
static void rcu_read_unlock(void) { assert(in_rcu); in_rcu = false; }
static struct page *compound_head(struct page *p) { return p; }
static bool PageSwapCache(struct page *p) { return p->swapcache; }
static struct mem_cgroup *page_memcg_rcu(struct page *p) { assert(in_rcu); return p->memcg; }
static bool css_tryget(struct cgroup_subsys_state *css)
{
    unsigned int refs = atomic_load(&css->refs);
    while (refs && !atomic_compare_exchange_weak(&css->refs, &refs, refs + 1)) { }
    return refs != 0;
}
static void css_put(struct cgroup_subsys_state *css)
{ assert(atomic_fetch_sub(&css->refs, 1) > 0); }
static bool css_tryget_online(struct cgroup_subsys_state *css)
{ return mi_memcg_data(container_of(css, struct mem_cgroup, css))->online && css_tryget(css); }
enum { ZRAM_WB = 0 };
struct zram_table_entry { struct mem_cgroup *memcg; u32 bytes, flags; };
struct zram { struct zram_table_entry table[4]; };
static bool zram_test_flag(struct zram *z, u32 i, int flag)
{ return z->table[i].flags & (1U << flag); }
static u32 zram_get_obj_size(struct zram *z, u32 i) { return z->table[i].bytes; }
'''
tests = r'''
static struct memcg_fixture group_fixture, parent_fixture;
#define group group_fixture.m
#define parent parent_fixture.m
static void expect(u64 pages, u64 bytes, u64 wbpages, u64 wbbytes, unsigned int refs)
{
    struct mi_memcg_stats s;
    mi_memcg_stats(&group, &s);
    assert(s.compressed_pages == pages && s.compressed_bytes == bytes);
    assert(s.writeback_pages == wbpages && s.writeback_bytes == wbbytes);
    assert(atomic_load(&group.css.refs) == refs);
    mi_memcg_stats(&parent, &s);
    assert(s.compressed_pages == pages && s.compressed_bytes == bytes);
    assert(s.writeback_pages == wbpages && s.writeback_bytes == wbbytes);
}
static void attach(struct zram *z, unsigned int i, struct page *p, u32 bytes)
{
    z->table[i].memcg = mi_memcg_get_swap_page(p);
    z->table[i].bytes = bytes;
    mi_memcg_zram_add(z->table[i].memcg, bytes, false);
}
static void *stress(void *unused)
{
    struct page p = { &group, true };
    struct zram z = {0};
    struct mem_cgroup *owner;
    (void)unused;
    for (unsigned int i = 0; i < 10000; i++) {
        attach(&z, 0, &p, 1000);
        owner = zram_memcg_preserve(&z, 0);
        zram_memcg_clear(&z, 0); /* physical free must not lose owner */
        z.table[0].bytes = 400;
        zram_memcg_restore(&z, 0, owner, 1000, 400, false, false);
        mi_memcg_zram_fault(owner);
        zram_memcg_clear(&z, 0);
    }
    return NULL;
}
int main(void)
{
    struct zram z = {0};
    struct page p = { &group, true };
    struct mem_cgroup *owner;
    struct mi_memcg_stats s;
    pthread_t threads[4];
    struct memcg_fixture other_fixture = {0};
#define other other_fixture.m
    struct mem_cgroup *cursor;
    atomic_init(&parent.css.refs, 1);
    mi_memcg_init(&parent);
    group.ancestor = &parent;
    atomic_init(&group.css.refs, 1);
    mi_memcg_init(&group);
    mi_memcg_online(&group);
    assert(mi_memcg_data(&group)->online && mi_memcg_data(&group)->refault_ratio == 50);
    atomic_init(&other.css.refs, 1); mi_memcg_init(&other); mi_memcg_online(&other);
    mi_memcg_set_score(&group, 200); mi_memcg_set_score(&other, 800);
    cursor = mi_memcg_next(NULL); assert(cursor == &other && atomic_load(&other.css.refs) == 2);
    cursor = mi_memcg_next(cursor); assert(cursor == &group && atomic_load(&other.css.refs) == 1);
    assert(!mi_memcg_next(cursor) && atomic_load(&group.css.refs) == 1);
    mi_memcg_set_score(&group, 900);
    cursor = mi_memcg_next(NULL); assert(cursor == &group);
    mi_memcg_offline(cursor); assert(!mi_memcg_next(cursor));
    mi_memcg_online(&group); mi_memcg_set_score(&group, 900);
    mi_memcg_offline(&other); css_put(&other.css);
    disabled = true; assert(!mi_memcg_get_swap_page(&p)); disabled = false;
    p.swapcache = false; assert(!mi_memcg_get_swap_page(&p)); p.swapcache = true;
    attach(&z, 0, &p, 1000); expect(1, 1000, 0, 0, 2);
    owner = zram_memcg_preserve(&z, 0);
    zram_memcg_clear(&z, 0); expect(1, 1000, 0, 0, 2);
    z.table[0].bytes = 300;
    zram_memcg_restore(&z, 0, owner, 1000, 300, false, false);
    expect(1, 300, 0, 0, 2);
    attach(&z, 1, &p, 0); /* SAME page has a real logical page and zero bytes */
    attach(&z, 2, &p, PAGE_SIZE); /* HUGE uncompressed page */
    expect(3, 4396, 0, 0, 4);
    owner = zram_memcg_preserve(&z, 0);
    zram_memcg_clear(&z, 0);
    z.table[0].bytes = 0; z.table[0].flags = 1U << ZRAM_WB;
    zram_memcg_restore(&z, 0, owner, 300, PAGE_SIZE, false, true);
    expect(2, PAGE_SIZE, 1, PAGE_SIZE, 4);
    mi_memcg_offline(&group);
    assert(!mi_memcg_data(&group)->online); /* slot references survive offline */
    zram_memcg_clear(&z, 0); zram_memcg_clear(&z, 1); zram_memcg_clear(&z, 2);
    expect(0, 0, 0, 0, 1);
    zram_memcg_clear(&z, 0); expect(0, 0, 0, 0, 1);
    mi_memcg_online(&group);
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, stress, NULL));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    expect(0, 0, 0, 0, 1);
    mi_memcg_stats(&group, &s); assert(s.faults == 40000);
    mi_memcg_stats(&parent, &s); assert(s.faults == 40000);
    mi_memcg_data(&group)->stats.faults = U64_MAX;
    mi_memcg_zram_fault(&group); assert(mi_memcg_data(&group)->stats.faults == U64_MAX);
    css_put(&group.css);
    assert(!mi_memcg_get_swap_page(&p));
    assert(!warnings);
    puts("PASS: real accounting/slot helpers: CSS lifetime, offline, SAME/HUGE, recompress, writeback tier, 40000 concurrent cycles, fault saturation");
    return 0;
}
'''
functions = ''.join(function(account, name) for name in [
    'mi_memcg_data', 'mi_memcg_init', 'mi_memcg_online', 'mi_memcg_offline',
    'mi_memcg_next', 'mi_memcg_set_score',
    'mi_memcg_get_swap_page', 'mi_memcg_account', 'mi_memcg_zram_add',
    'mi_memcg_zram_remove', 'mi_memcg_zram_replace', 'mi_memcg_zram_fault',
    'mi_memcg_stats'])
functions += ''.join(function(zram, name) for name in [
    'zram_memcg_clear', 'zram_memcg_preserve', 'zram_memcg_restore'])
if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='venus-mi-memcg-') as tmp:
        source = Path(tmp) / 'test.c'
        binary = Path(tmp) / 'test'
        source.write_text(env + header + '\n' + objects + functions + tests)
        subprocess.run([os.environ.get('HOSTCC', 'clang'), '-std=gnu11', '-O1', '-g',
            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-no-pie',
            '-pthread', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
