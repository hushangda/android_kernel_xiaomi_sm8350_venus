// SPDX-License-Identifier: GPL-2.0-only
/* Driver functions and the real next_arg() are appended by zram_test.sh. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>

typedef unsigned int u32;
typedef unsigned long long u64;
#define PAGE_SIZE 4096U
#define PAGE_SHIFT 12
#define BIT(n) (1U << (n))
#define GFP_KERNEL 0
#define __GFP_KSWAPD_RECLAIM 0
#define __GFP_NOWARN 0
#define __GFP_HIGHMEM 0
#define __GFP_MOVABLE 0
#define __GFP_CMA 0
#define __GFP_OFFLINABLE 0
#define ZRAM_PRIMARY_COMP 0U
#define ZRAM_SECONDARY_COMP 1U
#define ZRAM_MAX_COMPS 4U
#define ZRAM_IDLE 0
#define ZRAM_HUGE 1
#define ZRAM_WB 2
#define ZRAM_UNDER_WB 3
#define ZRAM_SAME 4
#define ZRAM_INCOMPRESSIBLE 5
#define ZS_MM_WO 0
#define RECOMPRESS_IDLE BIT(0)
#define RECOMPRESS_HUGE BIT(1)
#define max(a, b) ((a) > (b) ? (a) : (b))
#define kfree(p) free((void *)(p))
#define kstrndup(s, n, gfp) strndup(s, n)
#define atomic64_add(n, p) (*(p) += (n))
#define atomic64_inc(p) (++*(p))
#define cond_resched() ((void)0)

struct page { char data[PAGE_SIZE]; };
struct zcomp_strm { char buffer[PAGE_SIZE * 2]; unsigned int len; int error; };
struct zcomp { struct zcomp_strm strm; };
struct slot { unsigned long handle; u32 len, prio, flags; };
struct zram {
	struct slot slots[8];
	const char *comp_algs[4];
	int comp_levels[4];
	struct zcomp *comps[4];
	int num_active_comps, init_lock, mem_pool;
	u64 disksize;
	struct { unsigned long compr_data_size, pages_stored; } stats;
};
struct device { struct zram *zram; };
struct device_attribute { int unused; };
static const unsigned int huge_class_size = 3900;
static unsigned int attempts, reads, held_streams, allocated_pages;
static bool fail_alloc;
static char mapped[PAGE_SIZE];
static struct zcomp compressors[4];
static struct zram zr;
static struct device dev = { .zram = &zr };
static const char *default_compressor = "lz4";

static bool init_done(struct zram *z) { return z->disksize; }
static struct zram *dev_to_zram(struct device *d) { return d->zram; }
static void down_write(int *lock) { assert(!*lock); *lock = 1; }
static void up_write(int *lock) { assert(*lock == 1); *lock = 0; }
#define down_read down_write
#define up_read up_write
static char *skip_spaces(const char *s)
{
	while (isspace((unsigned char)*s)) s++;
	return (char *)s;
}
static char *next_arg(char *args, char **param, char **val);
static int kstrtoull(const char *s, int base, u64 *v)
{
	char *end;
	if (*s == '-') return -EINVAL;
	errno = 0;
	*v = strtoull(s, &end, base);
	return errno || end == s || *end ? -EINVAL : 0;
}
static int kstrtouint(const char *s, int base, u32 *v)
{
	u64 n;
	if (kstrtoull(s, base, &n) || n > UINT_MAX) return -EINVAL;
	*v = n;
	return 0;
}
static int kstrtoint(const char *s, int base, int *v)
{
	char *end;
	long n;
	errno = 0;
	n = strtol(s, &end, base);
	if (errno || end == s || *end || n < INT_MIN || n > INT_MAX)
		return -EINVAL;
	*v = n;
	return 0;
}
static bool zcomp_zstd_level_valid(int level) { return level >= 1 && level <= 22; }
static unsigned long zram_get_handle(struct zram *z, u32 i) { return z->slots[i].handle; }
static u32 zram_get_obj_size(struct zram *z, u32 i) { return z->slots[i].len; }
static u32 zram_get_priority(struct zram *z, u32 i) { return z->slots[i].prio; }
static bool zram_allocated(struct zram *z, u32 i) { return z->slots[i].handle; }
static bool zram_test_flag(struct zram *z, u32 i, int b) { return z->slots[i].flags & BIT(b); }
static void zram_clear_flag(struct zram *z, u32 i, int b) { z->slots[i].flags &= ~BIT(b); }
static void zram_set_flag(struct zram *z, u32 i, int b) { z->slots[i].flags |= BIT(b); }
static void zram_set_handle(struct zram *z, u32 i, unsigned long h) { z->slots[i].handle = h; }
static void zram_set_obj_size(struct zram *z, u32 i, u32 n) { z->slots[i].len = n; }
static void zram_set_priority(struct zram *z, u32 i, u32 p) { z->slots[i].prio = p; }
static void zram_slot_lock(struct zram *z, u32 i) { assert(z->init_lock); }
static void zram_slot_unlock(struct zram *z, u32 i) { assert(z->init_lock); }
static int zram_read_from_zspool(struct zram *z, struct page *p, u32 i)
{ reads++; return 0; }
static unsigned int zs_lookup_class_index(int p, unsigned int n) { return (n + 255) / 256; }
static unsigned long zs_malloc(int p, unsigned int n, unsigned int flags)
{ return fail_alloc ? 0 : 42; }
static void *zs_map_object(int p, unsigned long h, int mode) { return mapped; }
static void zs_unmap_object(int p, unsigned long h) { }
static struct zcomp_strm *zcomp_stream_get(struct zcomp *c)
{ held_streams++; return &c->strm; }
static void zcomp_stream_put(struct zcomp *c) { assert(held_streams); held_streams--; }
static void *kmap_atomic(struct page *p) { return p->data; }
static void kunmap_atomic(void *p) { }
static int zcomp_compress(struct zcomp_strm *s, const void *src, unsigned int *n)
{ attempts++; *n = s->len; return s->error; }
static struct page *alloc_page(int flags) { allocated_pages++; return calloc(1, sizeof(struct page)); }
static void __free_page(struct page *p) { allocated_pages--; free(p); }
static void zram_free_page(struct zram *z, u32 i)
{
	z->stats.compr_data_size -= z->slots[i].len;
	z->stats.pages_stored--;
	memset(&z->slots[i], 0, sizeof(z->slots[i]));
}

static void comp_algorithm_set(struct zram *, u32, const char *);
static int comp_algorithm_lookup(struct zram *, const char *, u32);
static ssize_t algorithm_params_store(struct device *, struct device_attribute *, const char *, size_t);
static int zram_recompress(struct zram *, u32, struct page *, u64 *, u32, u32, u32);
static ssize_t recompress_store(struct device *, struct device_attribute *, const char *, size_t);

static void reset_fixture(void)
{
	unsigned int i;
	assert(!held_streams && !allocated_pages && !zr.init_lock);
	memset(&zr, 0, sizeof(zr));
	memset(compressors, 0, sizeof(compressors));
	zr.comp_algs[0] = default_compressor;
	zr.comp_algs[1] = "zstd";
	zr.comp_algs[3] = "zstd";
	zr.comps[0] = &compressors[0];
	zr.comps[1] = &compressors[1];
	zr.comps[3] = &compressors[3];
	zr.num_active_comps = 3;
	compressors[1].strm.len = 1800;
	compressors[3].strm.len = 1000;
	for (i = 0; i < 8; i++) {
		zr.slots[i] = (struct slot) { 1, 3000, 0, BIT(ZRAM_IDLE) };
		zr.stats.compr_data_size += 3000;
		zr.stats.pages_stored++;
	}
	attempts = reads = 0;
	fail_alloc = false;
}
static ssize_t params(const char *s)
{ return algorithm_params_store(&dev, NULL, s, strlen(s)); }
static ssize_t recompress(const char *s)
{
	zr.disksize = 8 * PAGE_SIZE;
	return recompress_store(&dev, NULL, s, strlen(s));
}

int main(void)
{
	u64 budget;
	struct page page;
	const char *bad[] = {
		"level=0 algo=zstd", "level=-1 algo=zstd", "level=23 algo=zstd",
		"level=6 priority=4", "level=6 priority=2", "level=6",
		"level=6 algo=unknown", "level=6 priority=0 algo=zstd",
		"level=6 level=7 algo=zstd", "level=6 algo=zstd dict=/tmp/x",
		"algo=zstd", "", "level=6 priority=-1", "level=6 nope=2",
	};
	unsigned int i;
	reset_fixture();
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		assert(params(bad[i]) == -EINVAL);
	assert(params("algo=zstd level=6") > 0 && zr.comp_levels[1] == 6);
	assert(params("algo=zstd priority=3 level=22") > 0 && zr.comp_levels[3] == 22);
	zr.disksize = PAGE_SIZE;
	assert(params("algo=zstd level=4") == -EBUSY && zr.comp_levels[1] == 6);
	zr.disksize = 0;
	zr.comp_levels[0] = 6;
	comp_algorithm_set(&zr, 0, default_compressor);
	assert(!zr.comp_levels[0]);

	assert(recompress("max_pages=0") > 0 && !attempts && !reads);
	assert(recompress("priority=2") == -EINVAL);
	assert(recompress("priority=4") == -EINVAL);
	assert(recompress("priority=-1") == -EINVAL);
	assert(recompress("type=typo") == -EINVAL);
	assert(recompress("max_pages=-1") == -EINVAL);
	assert(recompress("max_pages=18446744073709551616") == -EINVAL);
	assert(recompress("max_pages=1 max_pages=2") == -EINVAL);
	assert(recompress("algo=lz4 priority=1") == -EINVAL);
	assert(recompress("unknown=1") == -EINVAL);
	assert(recompress("priority=3 max_pages=2") > 0 && attempts == 2);
	assert(zr.slots[0].prio == 3 && zr.slots[1].prio == 3 && !zr.slots[2].prio);

	reset_fixture();
	compressors[1].strm.len = 3000;
	compressors[3].strm.len = 3000;
	assert(recompress("max_pages=1") > 0 && attempts == 2);
	assert(zram_test_flag(&zr, 0, ZRAM_INCOMPRESSIBLE));
	assert(!zram_test_flag(&zr, 1, ZRAM_INCOMPRESSIBLE));
	assert(zr.slots[0].len == 3000);

	reset_fixture();
	compressors[1].strm.len = 2900; /* Same size class: never replace. */
	assert(recompress("priority=1 max_pages=1") > 0 && attempts == 1);
	assert(zr.slots[0].len == 3000 && !zram_test_flag(&zr, 0, ZRAM_INCOMPRESSIBLE));

	reset_fixture();
	zr.slots[0].prio = 3;
	zr.slots[1].flags = BIT(ZRAM_WB);
	zr.slots[2].len = 1000;
	assert(recompress("threshold=1500 priority=0 max_pages=1") > 0);
	assert(attempts == 1 && reads == 1 && zr.slots[3].prio == 0);
	assert(zr.slots[3].len == 3000); /* Result did not meet threshold. */

	reset_fixture();
	budget = 2;
	compressors[1].strm.error = -EIO;
	assert(zram_recompress(&zr, 0, &page, &budget, 0, 1, 4) == -EIO);
	assert(budget == 1 && attempts == 1 && zr.slots[0].len == 3000);
	assert(!held_streams);

	reset_fixture();
	fail_alloc = true;
	assert(recompress("priority=1 max_pages=1") == -ENOMEM);
	assert(attempts == 1 && zr.slots[0].len == 3000 && !held_streams);

	reset_fixture();
	assert(recompress("priority=1") > 0 && attempts == 8);
	assert(!held_streams && !allocated_pages && !zr.init_lock);
	puts("zram parameter, selection, budget and replacement tests: PASS");
	return 0;
}
