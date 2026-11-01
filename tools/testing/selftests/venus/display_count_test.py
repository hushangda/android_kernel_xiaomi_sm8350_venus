#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the complete display counter implementation with deterministic clocks.

Only Linux environment primitives are replaced. Arithmetic, parser, event
handlers, restore and snapshots are the actual compiled driver source.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
BASE = ROOT / "techpack/display/msm/mi_disp"


def without_includes(path):
    return "\n".join(line for line in path.read_text().splitlines()
                     if not line.startswith("#include"))


environment = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
typedef unsigned long long u64;
typedef unsigned int u32;
typedef pthread_mutex_t spinlock_t;
#define U64_MAX UINT64_MAX
#define HZ 250
#define PAGE_SIZE 4096
#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define spin_lock_init(p) assert(!pthread_mutex_init(p, NULL))
#define spin_lock_irqsave(p, flags) do { (flags) = 0; assert(!pthread_mutex_lock(p)); } while (0)
#define spin_unlock_irqrestore(p, flags) do { (void)(flags); assert(!pthread_mutex_unlock(p)); } while (0)
static u64 ticks, realtime = 1000, boottime;
static u64 get_jiffies_64(void) { return ticks; }
static u64 ktime_get_real_seconds(void) { return realtime; }
static u64 ktime_get_boottime_seconds(void) { return boottime; }
static u64 div_u64(u64 n, u32 d) { return n / d; }
static u64 div_u64_rem(u64 n, u32 d, u32 *r) { *r = n % d; return n / d; }
static int scnprintf(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return ret >= (int)n ? (int)n - 1 : ret;
}
'''

tests = r'''
static struct mi_panel_count first, second;
static char saved[PAGE_SIZE], record_a[PAGE_SIZE], record_b[PAGE_SIZE];
static size_t record_len;
static unsigned long malformed_records;

static void snapshot(struct mi_panel_count *count, u64 *value)
{
    char buf[PAGE_SIZE];
    ssize_t len = mi_panel_count_show(count, buf);
    assert(len > 0 && len < PAGE_SIZE);
    assert(!mi_count_parse(buf, len, value));
}

static void reset(void)
{
    ticks = 0;
    realtime = 1000;
    boottime = 0;
    mi_panel_count_init(&first);
    mi_panel_count_init(&second);
}

static void check_bad(const char *buf, size_t len)
{
    u64 before[MI_COUNT_FIELDS];
    memcpy(before, first.value, sizeof(before));
    assert(mi_panel_count_store(&first, buf, len) < 0);
    assert(!memcmp(before, first.value, sizeof(before)));
    malformed_records++;
}

static void parser_tests(void)
{
    u64 value[MI_COUNT_FIELDS + 1];
    char bad[PAGE_SIZE];
    record_len = mi_panel_count_show(&first, saved);
    assert(!mi_panel_count_store(&first, saved, record_len));
    /* The final newline is optional, every other truncation is invalid. */
    assert(!mi_panel_count_store(&first, saved, record_len - 1));
    for (size_t i = 0; i < record_len - 1; i++)
        check_bad(saved, i);
    for (size_t i = 0; i < record_len; i++) {
        memcpy(bad, saved, record_len);
        bad[i] = '\0';
        check_bad(bad, record_len);
        bad[i] = '-';
        check_bad(bad, record_len);
    }
    memcpy(bad, saved, record_len);
    bad[record_len] = 'x';
    check_bad(bad, record_len + 1);
    bad[record_len] = '\n';
    check_bad(bad, record_len + 1);
    memset(bad, '0', sizeof(bad));
    check_bad(bad, sizeof(bad));
    check_bad(saved, 0);
    snprintf(bad, sizeof(bad), "panel_active=18446744073709551616%s",
             strchr(saved, '\n'));
    check_bad(bad, strlen(bad));
    snprintf(bad, sizeof(bad), "panel_active=+1%s", strchr(saved, '\n'));
    check_bad(bad, strlen(bad));
    snprintf(bad, sizeof(bad), "panel_active= 1%s", strchr(saved, '\n'));
    check_bad(bad, strlen(bad));
    memcpy(bad, saved, record_len);
    bad[record_len - 2] = '1';
    check_bad(bad, record_len);
    snprintf(bad, sizeof(bad), "panel_active=18446744073709551615%s",
             strchr(saved, '\n'));
    assert(!mi_panel_count_store(&first, bad, strlen(bad)));
    snapshot(&first, value);
    assert(value[MI_COUNT_ACTIVE] == U64_MAX);
    assert(!mi_panel_count_store(&first, saved, record_len));
}

static void event_tests(void)
{
    u64 value[MI_COUNT_FIELDS + 1];
    mi_panel_count_fps(&first, 60);
    mi_panel_count_active(&first, true);
    mi_panel_count_backlight(&first, 100);
    ticks = 125;
    snapshot(&first, value);
    assert(value[MI_COUNT_ACTIVE] == 125);
    assert(value[MI_COUNT_FPS60] == 0);
    assert(value[MI_COUNT_BL_INTEGRAL] == 12500);
    mi_panel_count_active(&first, true); /* duplicate cannot restart epoch */
    mi_panel_count_fps(&first, 60);
    ticks = 250;
    snapshot(&first, value);
    assert(value[MI_COUNT_FPS60] == 1);
    assert(value[MI_COUNT_BL_LOW] == 250);
    mi_panel_count_backlight(&first, 1536);
    mi_panel_count_fps(&first, 90);
    mi_panel_count_hbm(&first, true);
    ticks = 375;
    mi_panel_count_hbm(&first, true);
    snapshot(&first, value); /* frequent reads preserve fractional seconds */
    ticks = 500;
    mi_panel_count_hbm(&first, false);
    mi_panel_count_fps(&first, 120);
    snapshot(&first, value);
    assert(value[MI_COUNT_HBM_DURATION] == 1);
    assert(value[MI_COUNT_HBM_TIMES] == 1);
    assert(value[MI_COUNT_FPS90] == 1);
    assert(value[MI_COUNT_BL_HIGH] == 250);
    assert(value[MI_COUNT_BL_INTEGRAL] == 250 * (100 + 1536));
    ticks = 750;
    mi_panel_count_active(&first, false); /* LP does not invent brightness 0 */
    ticks = 1000;
    snapshot(&first, value);
    assert(value[MI_COUNT_ACTIVE] == 750);
    assert(value[MI_COUNT_FPS120] == 1);
    assert(value[MI_COUNT_BL_DURATION] == 1000);
    mi_panel_count_active(&first, true);
    mi_panel_count_hbm(&first, true);
    ticks = 1250;
    mi_panel_count_disable(&first);
    ticks = 2500;
    snapshot(&first, value);
    assert(value[MI_COUNT_ACTIVE] == 1000);
    assert(value[MI_COUNT_BL_DURATION] == 1250);
    assert(value[MI_COUNT_HBM_DURATION] == 2);
    assert(value[MI_COUNT_HBM_TIMES] == 2);
    assert(value[MI_COUNT_FPS120] == 2);
    mi_panel_count_disable(&first);
    snapshot(&first, value);
    assert(value[MI_COUNT_HBM_TIMES] == 2);
    snapshot(&second, value);
    assert(value[MI_COUNT_ACTIVE] == 0 && value[MI_COUNT_BL_DURATION] == 0);
    mi_panel_count_active(&first, true);
    mi_panel_count_fps(&first, 144); /* unknown refresh never aliases 120 */
    ticks += 250;
    snapshot(&first, value);
    assert(value[MI_COUNT_FPS120] == 2);
    mi_panel_count_disable(&first);
}

static void overflow_and_restore_tests(void)
{
    u64 value[MI_COUNT_FIELDS + 1];
    assert(!mi_panel_count_store(&first, saved, record_len));
    boottime = 123;
    snapshot(&first, value);
    assert(value[MI_COUNT_BOOTTIME] == 123);
    assert(!mi_panel_count_store(&first, saved, record_len));
    first.value[MI_COUNT_ACTIVE] = U64_MAX - 1;
    first.value[MI_COUNT_KICKOFF] = U64_MAX;
    first.value[MI_COUNT_BL_INTEGRAL] = U64_MAX - 1;
    first.value[MI_COUNT_BOOTTIME] = U64_MAX - 1;
    mi_panel_count_active(&first, true);
    mi_panel_count_backlight(&first, 65535);
    mi_panel_count_kickoff(&first);
    ticks += 250;
    snapshot(&first, value);
    assert(value[MI_COUNT_ACTIVE] == U64_MAX);
    assert(value[MI_COUNT_KICKOFF] == U64_MAX);
    assert(value[MI_COUNT_BL_INTEGRAL] == U64_MAX);
    assert(value[MI_COUNT_BOOTTIME] == U64_MAX);
    assert(mi_count_mul(U64_MAX, 2) == U64_MAX);
    assert(mi_count_mul(U64_MAX, 0) == 0);
    /* Restore on an active panel rebases timestamps without losing its state. */
    assert(!mi_panel_count_store(&first, saved, record_len));
    mi_panel_count_fps(&first, 60);
    ticks += 250;
    snapshot(&first, value);
    assert(value[MI_COUNT_ACTIVE] == 250);
    assert(value[MI_COUNT_BL_INTEGRAL] == 65535 * 250);
    assert(value[MI_COUNT_FPS60] == 1);
    mi_panel_count_disable(&first);
    boottime = 0;
    assert(!mi_panel_count_store(&first, saved, record_len));
    realtime += 86400 * 2 + 50;
    snapshot(&first, value);
    assert(value[MI_COUNT_DAYS] == 2);
    assert(value[MI_COUNT_RTC] == realtime - 50);
    realtime = 500; /* RTC rollback does not underflow */
    snapshot(&first, value);
    assert(value[MI_COUNT_DAYS] == 2 && value[MI_COUNT_RTC] == 500);
    realtime += 11ULL * 365 * 86400;
    snapshot(&first, value);
    assert(value[MI_COUNT_DAYS] == 2 && value[MI_COUNT_RTC] == realtime);
    realtime = 1000;
    assert(!mi_panel_count_store(&first, saved, record_len));
}

static void *kicker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 10000; i++)
        mi_panel_count_kickoff(&first);
    return NULL;
}

static void *restorer(void *arg)
{
    char *record = arg;
    u64 value[MI_COUNT_FIELDS + 1];
    for (int i = 0; i < 2000; i++) {
        assert(!mi_panel_count_store(&first, record, strlen(record)));
        snapshot(&first, value);
        assert(value[0] == 1 || value[0] == 2);
        for (int j = 0; j < MI_COUNT_FIELDS; j++)
            if (j != MI_COUNT_RTC)
                assert(value[j] == value[0]);
    }
    return NULL;
}

static void concurrent_tests(void)
{
    pthread_t threads[4];
    u64 value[MI_COUNT_FIELDS + 1];
    for (int i = 0; i < 4; i++)
        assert(!pthread_create(&threads[i], NULL, kicker, NULL));
    for (int i = 0; i < 4; i++)
        assert(!pthread_join(threads[i], NULL));
    snapshot(&first, value);
    assert(value[MI_COUNT_KICKOFF] == 40000);
    size_t a = 0, b = 0;
    for (int i = 0; i < ARRAY_SIZE(mi_count_keys); i++) {
        u64 va = i == MI_COUNT_FIELDS ? 0 : i == MI_COUNT_RTC ? 1000 : 1;
        u64 vb = i == MI_COUNT_FIELDS ? 0 : i == MI_COUNT_RTC ? 1000 : 2;
        a += snprintf(record_a + a, sizeof(record_a) - a,
                      "%s=%llu\n", mi_count_keys[i], va);
        b += snprintf(record_b + b, sizeof(record_b) - b,
                      "%s=%llu\n", mi_count_keys[i], vb);
    }
    for (int i = 0; i < 4; i++)
        assert(!pthread_create(&threads[i], NULL, restorer,
                               i % 2 ? record_a : record_b));
    for (int i = 0; i < 4; i++)
        assert(!pthread_join(threads[i], NULL));
}

int main(void)
{
    reset();
    parser_tests();
    event_tests();
    overflow_and_restore_tests();
    concurrent_tests();
    printf("PASS: %lu malformed records rejected atomically; Venus 15-field ABI, "
           "jiffies/seconds, duplicate events, 60/90/120 Hz, LP/off, two panels, "
           "RTC, overflow, live restore, 40000 concurrent kickoffs and "
           "8000 concurrent restore/snapshots\n", malformed_records);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="venus-display-count-test-") as tmp:
    tmp = Path(tmp)
    (tmp / "test.c").write_text(environment +
        without_includes(BASE / "mi_dsi_panel_count.h") + "\n" +
        without_includes(BASE / "mi_dsi_panel_count.c") + "\n" + tests)
    # Clang 14 PIE ASan can fault in AsanInitInternal before main on this host.
    # A fixed executable mapping avoids that runtime startup failure without
    # suppressing either sanitizer or changing the tested driver functions.
    subprocess.run([os.environ.get("HOSTCC", "clang"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-no-pie", "-pthread",
                    str(tmp / "test.c"), "-o", str(tmp / "test")], check=True)
    subprocess.run([str(tmp / "test")], check=True)
