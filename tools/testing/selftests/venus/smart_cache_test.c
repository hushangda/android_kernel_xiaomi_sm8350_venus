// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned long pgoff_t;
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
static unsigned long jiffies;
#define msecs_to_jiffies(x) (x)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define GFP_KERNEL 0
#define GFP_NOWAIT 1
#define __GFP_NORETRY 2
#define OVERLAYFS_SUPER_MAGIC 0x794c7630
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define min(a, b) ((a) < (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define VM_BUG_ON(x) assert(!(x))
#define VM_BUG_ON_PAGE(x, p) assert(!(x))
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define PTR_ERR(x) ((int)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x) >= (uintptr_t)-4095)
#define __init
#define late_initcall(x)
#define DEFINE_SPINLOCK(x) pthread_mutex_t x = PTHREAD_MUTEX_INITIALIZER
#define DEFINE_MUTEX(x) pthread_mutex_t x = PTHREAD_MUTEX_INITIALIZER
#define spin_lock_irqsave(l, f) do { (f) = 0; assert(!pthread_mutex_lock(l)); } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); assert(!pthread_mutex_unlock(l)); } while (0)
#define mutex_lock(l) assert(!pthread_mutex_lock(l))
#define mutex_unlock(l) assert(!pthread_mutex_unlock(l))
typedef long long atomic64_t;
#define ATOMIC64_INIT(x) (x)
#define atomic64_add(n, p) __atomic_add_fetch(p, n, __ATOMIC_RELAXED)
#define atomic64_inc(p) atomic64_add(1, p)
#define atomic64_read(p) READ_ONCE(*(p))
#define atomic_inc(p) (++*(p))
#define atomic_dec(p) (--*(p))
struct rcu_head { int unused; };
static pthread_rwlock_t rcu_lock = PTHREAD_RWLOCK_INITIALIZER;
static int rcu_barriers, irq_depth;
static void rcu_read_lock(void) { assert(!pthread_rwlock_rdlock(&rcu_lock)); }
static void rcu_read_unlock(void) { assert(!pthread_rwlock_unlock(&rcu_lock)); }
static void synchronize_rcu(void) {
    assert(!pthread_rwlock_wrlock(&rcu_lock)); rcu_barriers++;
    assert(!pthread_rwlock_unlock(&rcu_lock));
}
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
static void local_irq_disable(void) { assert(!irq_depth++); }
static void local_irq_enable(void) { assert(!--irq_depth); }
static int alloc_fail, allocations;
static void *kzalloc(size_t n, int g) {
    if (alloc_fail) { alloc_fail--; return NULL; }
    void *p = calloc(1, n); assert(p); allocations++; return p;
}
static void *kcalloc(size_t n, size_t s, int g) { return kzalloc(n * s, g); }
static void kfree(void *p) { if (p) { allocations--; free(p); } }
#define kfree_rcu(p, r) do { synchronize_rcu(); kfree(p); } while (0)
static void *kmemdup(const void *p, size_t n, int g) {
    void *q = kzalloc(n, g); if (q) memcpy(q, p, n); return q;
}
static char *kstrndup(const char *s, size_t n, int g) {
    char *q = kzalloc(n + 1, g); if (q) memcpy(q, s, n); return q;
}
struct hlist_node { struct hlist_node *next, **prev; };
#define DEFINE_HASHTABLE(t, b) struct hlist_node *t[1UL << (b)]
#define hash_slot(t, k) (((uintptr_t)(k) >> 4) % ARRAY_SIZE(t))
#define hash_for_each_possible(t, p, m, k) \
    for (struct hlist_node *_n = READ_ONCE((t)[hash_slot(t, k)]); \
         _n && ((p) = container_of(_n, __typeof__(*(p)), m), 1); _n = READ_ONCE(_n->next))
#define hash_for_each_possible_rcu hash_for_each_possible
#define hash_add_rcu(t, n, k) do { \
    struct hlist_node **_h = &(t)[hash_slot(t, k)]; \
    (n)->next = *_h; (n)->prev = _h; if (*_h) (*_h)->prev = &(n)->next; WRITE_ONCE(*_h, (n)); \
} while (0)
static void hash_del_rcu(struct hlist_node *n) {
    WRITE_ONCE(*n->prev, n->next); if (n->next) n->next->prev = n->prev;
}
static int kstrtouint(const char *s, int base, unsigned int *out) {
    char *end; unsigned long v;
    if (!*s || *s == '-') return -EINVAL;
    errno = 0; v = strtoul(s, &end, base);
    if (*end == '\n') end++;
    if (errno || *end || v > UINT_MAX) return -EINVAL;
    *out = v; return 0;
}
static char *strim(char *s) {
    while (*s == ' ' || *s == '\n' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\n' || s[n-1] == '\t')) s[--n] = 0;
    return s;
}
static int scnprintf(char *s, size_t n, const char *fmt, ...) {
    va_list a; va_start(a, fmt); int r = vsnprintf(s, n, fmt, a); va_end(a);
    return r >= (int)n ? (int)n - 1 : r;
}
enum { NR_FILE_PAGES, NR_ANON_MAPPED, NR_SHMEM, NR_FILE_MAPPED, NR_FILE_DIRTY,
       NR_WRITEBACK, NR_ACTIVE_FILE, NR_INACTIVE_FILE, N_STATS };
struct pglist_data { long stat[N_STATS]; };
static struct pglist_data node;
struct lruvec { long stat[N_STATS]; };
struct cgroup_subsys_state { int refs; bool online; };
struct mi_memcg { bool online, sc_dying; u32 sc_level; unsigned long sc_max_pages; };
struct mem_cgroup {
    struct cgroup_subsys_state css;
    struct mi_memcg data;
    struct mem_cgroup *parent;
    struct lruvec lru;
    int moving_account;
    long charge, events;
};
static struct mem_cgroup root, one, two, three, other;
static struct mem_cgroup *root_mem_cgroup = &root;
static struct mi_memcg *mi_memcg_data(struct mem_cgroup *m) { return &m->data; }
static struct mem_cgroup *mem_cgroup_from_css(struct cgroup_subsys_state *c) {
    return container_of(c, struct mem_cgroup, css);
}
static bool mem_cgroup_is_root(struct mem_cgroup *m) { return m == &root; }
static bool mem_cgroup_is_descendant(struct mem_cgroup *m, struct mem_cgroup *p) {
    for (; m; m = m->parent) if (m == p) return true;
    return false;
}
static bool mem_cgroup_disabled(void) { return false; }
static void css_get(struct cgroup_subsys_state *c) { assert(c->refs > 0); c->refs++; }
static bool css_tryget_online(struct cgroup_subsys_state *c) {
    if (!c->online) return false;
    css_get(c); return true;
}
static void css_put(struct cgroup_subsys_state *c) { assert(c->refs > 1); c->refs--; }
struct super_block { unsigned long s_magic; };
struct inode { struct super_block *i_sb; unsigned int i_mode; };
struct page;
struct address_space { struct inode *host; struct page **pages; unsigned int count; bool shmem; };
struct file { struct address_space *f_mapping; struct file *real; int refs; bool hugetlb; };
struct page {
    struct address_space *mapping;
    struct mem_cgroup *mem_cgroup;
    bool locked, busy, anon, swap, huge, tail, thp, lru, dirty, writeback, mapped;
    int refs, order_pages;
};
static struct page *race_page;
static struct mem_cgroup *race_owner;
#define PageLocked(p) ((p)->locked)
#define PageAnon(p) ((p)->anon)
#define PageSwapBacked(p) ((p)->swap)
#define PageHuge(p) ((p)->huge)
#define PageTail(p) ((p)->tail)
#define PageTransHuge(p) ((p)->thp)
#define PageLRU(p) ((p)->lru)
#define PageDirty(p) ((p)->dirty)
#define PageWriteback(p) ((p)->writeback)
#define IS_DAX(p) false
static struct address_space *page_mapping(struct page *p) { return p->mapping; }
static struct mem_cgroup *page_memcg_rcu(struct page *p) { return p->mem_cgroup; }
static unsigned int hpage_nr_pages(struct page *p) { return p->order_pages; }
static bool page_mapped(struct page *p) { return p->mapped; }
static bool trylock_page(struct page *p) {
    if (race_page == p) { p->mem_cgroup = race_owner; race_page = NULL; }
    if (p->locked || p->busy) return false;
    p->locked = true; return true;
}
static void unlock_page(struct page *p) { assert(p->locked); p->locked = false; }
static struct pglist_data *page_pgdat(struct page *p) { return &node; }
static struct lruvec *mem_cgroup_lruvec(struct mem_cgroup *m, struct pglist_data *n) { return &m->lru; }
static unsigned long lruvec_page_state(struct lruvec *l, int i) { return l->stat[i]; }
static unsigned long node_page_state(struct pglist_data *n, int i) { return n->stat[i]; }
static void __mod_lruvec_state(struct lruvec *l, int i, int n) { l->stat[i] += n; }
static void lock_page_memcg(struct page *p) {
    assert(p->locked && !p->lru && p->mem_cgroup->moving_account > 0);
}
static void __unlock_page_memcg(struct mem_cgroup *m) { }
static bool mapping_cap_account_dirty(struct address_space *m) { return true; }
static void mem_cgroup_charge_statistics(struct mem_cgroup *m, struct page *p, int n) { m->events += n; }
static void memcg_check_events(struct mem_cgroup *m, struct page *p) { }
static int charge_fail, isolate_fail;
static int try_charge(struct mem_cgroup *m, int g, unsigned int n) {
    assert(g == (GFP_NOWAIT | __GFP_NORETRY));
    if (charge_fail) return -ENOMEM;
    if (m != &root) { m->charge += n; m->css.refs += n; } return 0;
}
static void cancel_charge(struct mem_cgroup *m, unsigned int n) {
    if (m == &root) return;
    assert(m->charge >= n && m->css.refs > (int)n);
    m->charge -= n; m->css.refs -= n;
}
static int isolate_lru_page(struct page *p) {
    assert(p->locked);
    if (isolate_fail || !p->lru) return -EBUSY;
    p->lru = false; p->refs++;
    p->mem_cgroup->lru.stat[NR_INACTIVE_FILE] -= p->order_pages; return 0;
}
static void putback_lru_page(struct page *p) {
    assert(!p->locked && !p->lru && p->refs > 1); p->lru = true; p->refs--;
    p->mem_cgroup->lru.stat[NR_INACTIVE_FILE] += p->order_pages;
}
static void put_page(struct page *p) { assert(p->refs > 0); p->refs--; }
static void lru_add_drain_all(void) { }
static unsigned long ticks_per_resched;
static void cond_resched(void) { jiffies += ticks_per_resched; }
static int signal_after = -1, current;
static bool fatal_signal_pending(int c) { return signal_after >= 0 && !signal_after--; }
static unsigned int find_get_pages_range(struct address_space *m, pgoff_t *start,
                                         pgoff_t end, unsigned int n, struct page **p) {
    unsigned int nr = 0;
    while (*start < m->count && nr < n) {
        if (m->pages[*start]) { p[nr] = m->pages[*start]; p[nr++]->refs++; }
        (*start)++;
    }
    if (*start == m->count) *start = (pgoff_t)-1;
    return nr;
}
static struct file *fds[256];
static struct file *fget(int fd) {
    if (fd < 0 || fd >= (int)ARRAY_SIZE(fds) || !fds[fd]) return NULL;
    fds[fd]->refs++; return fds[fd];
}
static void fput(struct file *f) { assert(f->refs > 0); f->refs--; }
static struct inode *file_inode(struct file *f) { return f->f_mapping->host; }
static bool is_file_hugepages(struct file *f) { return f->hugetlb; }
static bool shmem_mapping(struct address_space *m) { return m->shmem; }
static struct file *ovl_smart_cache_real_file(struct file *f) {
    if (!f->real) return ERR_PTR(-EOPNOTSUPP);
    f->real->refs++; return f->real;
}
struct kobject { int unused; };
struct attribute { int unused; };
struct kobj_attribute { struct attribute attr; };
struct attribute_group { struct attribute **attrs; };
#define __ATTR_WO(n) { { 0 } }
#define __ATTR_RO(n) { { 0 } }
#define __ATTR_RW(n) { { 0 } }
struct cftype {
    const char *name;
    u64 (*read_u64)(struct cgroup_subsys_state *, struct cftype *);
    int (*write_u64)(struct cgroup_subsys_state *, struct cftype *, u64);
};
static struct kobject *kernel_kobj;
static int memory_cgrp_subsys, register_fail, registered, removed, registration_step;
static struct kobject *kobject_create_and_add(const char *n, struct kobject *k) { return kzalloc(sizeof(*k), 0); }
static void kobject_put(struct kobject *k) { kfree(k); }
static int cgroup_add_legacy_cftypes(int *s, struct cftype *f) {
    if (++registration_step == register_fail) return -ENOMEM;
    registered++; return 0;
}
static int cgroup_add_dfl_cftypes(int *s, struct cftype *f) { return cgroup_add_legacy_cftypes(s, f); }
static void cgroup_rm_cftypes(struct cftype *f) { removed++; registered--; }
static int sysfs_create_group(struct kobject *k, const struct attribute_group *g) {
    return ++registration_step == register_fail ? -ENOMEM : 0;
}

#include "move.h"
#include "policy.h"

static struct super_block fs = { 0 };
static struct inode inode = { &fs, S_IFREG | 0644 };
static struct address_space mapping = { .host = &inode };
static struct file file = { .f_mapping = &mapping };

static void reset_groups(void) {
    struct mem_cgroup *groups[] = { &root, &one, &two, &three, &other };
    memset(sc_groups, 0, sizeof(sc_groups));
    for (unsigned int i = 0; i < ARRAY_SIZE(groups); i++) {
        memset(groups[i], 0, sizeof(*groups[i]));
        groups[i]->css.refs = 1; groups[i]->css.online = true; groups[i]->data.online = true;
    }
    sc_enabled = true; sc_ratio = 50; node.stat[NR_INACTIVE_FILE] = 1000;
    charge_fail = isolate_fail = 0; signal_after = -1;
}
static struct page new_page(struct mem_cgroup *m, int nr) {
    struct page p = { .mapping = &mapping, .mem_cgroup = m, .lru = true,
                     .order_pages = nr, .refs = 1, .thp = nr > 1,
                     .dirty = true, .writeback = true, .mapped = true };
    if (m != &root) { m->charge += nr; m->css.refs += nr; }
    m->lru.stat[NR_FILE_PAGES] += nr; m->lru.stat[NR_INACTIVE_FILE] += nr;
    m->lru.stat[NR_FILE_DIRTY] += nr; m->lru.stat[NR_WRITEBACK] += nr;
    m->lru.stat[NR_FILE_MAPPED] += nr;
    return p;
}
static void test_move(void) {
    struct mem_cgroup *source = NULL;
    reset_groups();
    struct page p = new_page(&other, 512);
    assert(mi_smart_cache_move_page(&p, &one, &source) == 512);
    assert(source == &other && other.moving_account == 1);
    assert(p.mem_cgroup == &one && p.lru && !p.locked && p.refs == 1);
    assert(one.charge == 512 && !other.charge && one.css.refs == 513 && other.css.refs == 2);
    assert(one.lru.stat[NR_FILE_PAGES] == 512 && !other.lru.stat[NR_FILE_PAGES]);
    assert(one.lru.stat[NR_FILE_DIRTY] == 512 && !other.lru.stat[NR_FILE_DIRTY]);
    assert(one.lru.stat[NR_FILE_MAPPED] == 512 && one.lru.stat[NR_WRITEBACK] == 512);
    int barriers = rcu_barriers;
    assert(!mi_smart_cache_move_page(&p, &one, &source));
    assert(rcu_barriers == barriers);
    assert(mi_smart_cache_move_page(&p, &root, &source) == 512);
    assert(!one.charge && root.charge == 0 && root.css.refs == 1);
    mi_smart_cache_move_end(&source);
    assert(!source && !other.moving_account && !one.moving_account);
    assert(other.css.refs == 1 && one.css.refs == 1);

    for (int failure = 0; failure < 4; failure++) {
        reset_groups(); p = new_page(&other, 1);
        charge_fail = failure == 0; isolate_fail = failure == 1;
        p.busy = failure == 2;
        if (failure == 3) { race_page = &p; race_owner = &root; }
        assert(mi_smart_cache_move_page(&p, &one, &source) < 0);
        assert(p.refs == 1 && p.lru && !p.locked);
        assert(other.charge == 1 && one.charge == 0 && one.css.refs == 1);
        mi_smart_cache_move_end(&source);
        assert(other.css.refs == 2 && !other.moving_account);
    }
    reset_groups(); p = new_page(&other, 1);
    p.anon = true; assert(mi_smart_cache_move_page(&p, &one, &source) == -EINVAL);
    p.anon = false; p.mem_cgroup = NULL;
    assert(mi_smart_cache_move_page(&p, &one, &source) == -EAGAIN);
    assert(!source);
}

static void test_policy(void) {
    reset_groups();
    assert(!sc_level_write(&one.css, NULL, 1));
    assert(!sc_level_write(&two.css, NULL, 2));
    assert(!sc_level_write(&three.css, NULL, 3));
    assert(sc_level_write(&root.css, NULL, 1) == -EINVAL);
    assert(sc_level_write(&other.css, NULL, 1) == -EBUSY);
    assert(sc_level_write(&one.css, NULL, 4) == -EINVAL);
    assert(!sc_level_write(&one.css, NULL, 1));
    assert(sc_max_write(&one.css, NULL, 4097) == -EINVAL);
    assert(sc_max_write(&root.css, NULL, 0) == -EINVAL);
    assert(!sc_max_write(&one.css, NULL, 200 * PAGE_SIZE));
    assert(sc_max_read(&one.css, NULL) == 200 * PAGE_SIZE);
    assert(!sc_max_write(&two.css, NULL, 400 * PAGE_SIZE));
    assert(!sc_max_write(&three.css, NULL, 400 * PAGE_SIZE));
    assert(!sc_tag_inode(&inode, 1));
    struct page p = new_page(&one, 1); p.locked = true;
    one.lru.stat[NR_INACTIVE_FILE] = 200;
    assert(mi_smart_cache_protect(&p, 12));
    assert(!mi_smart_cache_protect(&p, 1));
    one.lru.stat[NR_INACTIVE_FILE]++;
    assert(!mi_smart_cache_protect(&p, 12));
    one.lru.stat[NR_INACTIVE_FILE] = 200;
    assert(mi_smart_cache_charge_target(&p) == &one); css_put(&one.css);
    p.swap = true; assert(!mi_smart_cache_charge_target(&p)); p.swap = false;
    p.locked = false; assert(!mi_smart_cache_charge_target(&p)); p.locked = true;
    sc_enabled = false; assert(!mi_smart_cache_protect(&p, 12));
    assert(!mi_smart_cache_charge_target(&p)); sc_enabled = true;
    assert(!sc_tag_inode(&inode, 2)); p.mem_cgroup = &two;
    two.lru.stat[NR_INACTIVE_FILE] = 300;
    assert(mi_smart_cache_protect(&p, 12));
    two.lru.stat[NR_INACTIVE_FILE] = 301;
    assert(!mi_smart_cache_protect(&p, 12));
    assert(!sc_level_write(&two.css, NULL, 0));
    assert(!sc_groups[1]); assert(!mi_smart_cache_charge_target(&p));
    assert(!sc_level_write(&two.css, NULL, 2));
    mi_smart_cache_offline(&two);
    assert(!sc_groups[1] && !two.data.sc_level);
    assert(sc_level_write(&two.css, NULL, 2) == -ENODEV);
    assert(!mi_smart_cache_charge_target(&p));
    assert(!sc_level_write(&three.css, NULL, 0));
    assert(!sc_level_write(&one.css, NULL, 3));
    assert(!sc_groups[0] && sc_groups[2] == &one);
    other.parent = &one;
    assert(sc_level_write(&other.css, NULL, 1) == -EINVAL);
    mi_smart_cache_inode_free(&inode); assert(!sc_inode_level(&inode) && !sc_nr_tags);
    assert(ratio_store(NULL, NULL, "101", 3) == -EINVAL);
    assert(ratio_store(NULL, NULL, "0", 1) == 1 && !sc_ratio);
    assert(enable_store(NULL, NULL, "2", 1) == -EINVAL);
    assert(enable_store(NULL, NULL, "0", 1) == 1 && !sc_enabled);
    unsigned long usage[] = { ULONG_MAX, ULONG_MAX, ULONG_MAX };
    unsigned long max[] = { ULONG_MAX, ULONG_MAX, ULONG_MAX };
    /* Real page counts are <= ULONG_MAX / PAGE_SIZE; no product overflow. */
    assert(sc_budget(ULONG_MAX / PAGE_SIZE, 100, usage, max, 1) == ULONG_MAX / PAGE_SIZE);
    assert(!sc_budget(ULONG_MAX / PAGE_SIZE, 100, usage, max, 2));
}

static void test_file_transaction(void) {
    reset_groups(); assert(!sc_level_write(&one.css, NULL, 1));
    fds[3] = &file;
    const char *invalid[] = { "", ",", "3-1,", "3-1,,3-2", "-1-1", "3--1", "3-4",
        "3-", "3-+1", "+3-1", "3\n-1", "3 -1", "2147483648-1", "3-4294967296", "3-1x" };
    for (unsigned int i = 0; i < ARRAY_SIZE(invalid); i++) {
        assert(file_store(NULL, NULL, invalid[i], strlen(invalid[i])) == -EINVAL);
        assert(!sc_inode_level(&inode) && !file.refs);
    }
    assert(file_store(NULL, NULL, "3-1,4-1", 7) == -EBADF);
    assert(!sc_inode_level(&inode) && !file.refs);
    assert(file_store(NULL, NULL, "3-2", 3) == -ENODEV);
    assert(!sc_inode_level(&inode));
    file.hugetlb = true;
    assert(file_store(NULL, NULL, "3-1", 3) == -EOPNOTSUPP);
    file.hugetlb = false;
    struct page pages[70], *pointers[70];
    for (unsigned int i = 0; i < ARRAY_SIZE(pages); i++) { pages[i] = new_page(&other, 1); pointers[i] = &pages[i]; }
    mapping.pages = pointers; mapping.count = ARRAY_SIZE(pages);
    long long before = sc_moved;
    assert(file_store(NULL, NULL, "3-1\n", 4) == 4);
    assert(sc_inode_level(&inode) == 1 && one.charge == 70 && !other.charge);
    assert(sc_moved - before == 70 && !file.refs);
    assert(file_store(NULL, NULL, "3-1", 3) == 3);
    assert(sc_moved - before == 70); /* No fictitious duplicate migrations. */
    pages[2].busy = true;
    assert(file_store(NULL, NULL, "3-0", 3) == -EBUSY);
    assert(!sc_inode_level(&inode) && one.charge == 1 && root.charge == 0);
    assert(!mi_smart_cache_protect(&pages[2], 12));
    pages[2].busy = false;
    assert(file_store(NULL, NULL, "3-0", 3) == 3);
    assert(!one.charge && one.css.refs == 1);
    for (unsigned int i = 0; i < ARRAY_SIZE(pages); i++) assert(pages[i].refs == 1 && pages[i].lru);
    signal_after = 3;
    assert(file_store(NULL, NULL, "3-1", 3) == -EINTR);
    for (unsigned int i = 0; i < ARRAY_SIZE(pages); i++) assert(pages[i].refs == 1 && !pages[i].locked);
    assert(!other.moving_account && !root.moving_account && !file.refs);
    signal_after = -1;
    assert(file_store(NULL, NULL, "3-1", 3) == 3);
    assert(one.charge == 70);
    assert(file_store(NULL, NULL, "3-0", 3) == 3);
    assert(!sc_nr_tags && !one.charge);
    ticks_per_resched = 5001;
    assert(file_store(NULL, NULL, "3-1", 3) == -ETIMEDOUT);
    assert(one.charge == SC_BATCH);
    for (unsigned int i = 0; i < ARRAY_SIZE(pages); i++) assert(pages[i].refs == 1);
    ticks_per_resched = 0;
    assert(file_store(NULL, NULL, "3-1", 3) == 3);
    assert(one.charge == 70);
    assert(file_store(NULL, NULL, "3-0", 3) == 3);
    mapping.pages = NULL; mapping.count = 0;
    alloc_fail = 1; assert(file_store(NULL, NULL, "3-1", 3) == -ENOMEM);
    assert(!file.refs && !sc_nr_tags);
    struct super_block overlay_fs = { OVERLAYFS_SUPER_MAGIC };
    struct inode overlay_inode = { &overlay_fs, S_IFREG | 0644 };
    struct address_space overlay_mapping = { .host = &overlay_inode };
    struct file overlay = { .f_mapping = &overlay_mapping, .real = &file };
    fds[4] = &overlay;
    assert(file_store(NULL, NULL, "4-1", 3) == 3);
    assert(sc_inode_level(&inode) == 1 && !sc_inode_level(&overlay_inode));
    assert(!overlay.refs && !file.refs);
    assert(file_store(NULL, NULL, "4-0", 3) == 3);
    overlay.real = &overlay;
    assert(file_store(NULL, NULL, "4-1", 3) == -ELOOP && !overlay.refs);
    fds[4] = NULL;
}

static void *tag_reader(void *unused) {
    for (int i = 0; i < 50000; i++) assert(sc_inode_level(&inode) <= 3);
    return NULL;
}
static void test_tag_lifetime(void) {
    pthread_t readers[3];
    for (unsigned int i = 0; i < ARRAY_SIZE(readers); i++)
        assert(!pthread_create(&readers[i], NULL, tag_reader, NULL));
    for (int i = 0; i < 2000; i++) {
        assert(!sc_tag_inode(&inode, 1));
        assert(!sc_tag_inode(&inode, 3));
        mi_smart_cache_inode_free(&inode);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(readers); i++)
        assert(!pthread_join(readers[i], NULL));
    assert(!sc_nr_tags);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("main-entry: SmartCache actual-code ASan/UBSan harness");
    test_move(); test_policy(); test_file_transaction(); test_tag_lifetime();
    struct inode *many = calloc(SC_MAX_TAGS + 1, sizeof(*many)); assert(many);
    for (unsigned int i = 0; i < SC_MAX_TAGS; i++) assert(!sc_tag_inode(&many[i], 1));
    assert(sc_tag_inode(&many[SC_MAX_TAGS], 1) == -ENOSPC);
    for (unsigned int i = 0; i < SC_MAX_TAGS; i++) mi_smart_cache_inode_free(&many[i]);
    free(many); assert(!sc_nr_tags);
    for (int failure = 1; failure <= 3; failure++) {
        register_fail = failure; registration_step = registered = removed = 0;
        assert(mi_smart_cache_init() == -ENOMEM);
        assert(!registered);
    }
    assert(!allocations && !irq_depth && !file.refs);
    puts("PASS: real charging/stat migration, THP, rollback, source barriers, caps/priority, offline, fd parser, partial failure/retry, signals, overlay, 150000 concurrent tag lookups/inode teardown and init unwind");
    return 0;
}
