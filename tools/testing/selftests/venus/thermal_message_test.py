#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run the actual thermal text helpers with ASan/UBSan and concurrent callers."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
source = (ROOT / "drivers/thermal/thermal_core.c").read_text()


def function(name):
    start = source.index("static ssize_t " + name + "(")
    end = source.index("{", start)
    depth = 1
    end += 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


harness = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#define PAGE_SIZE 4096
static pthread_mutex_t thermal_message_lock = PTHREAD_MUTEX_INITIALIZER;
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
static int scnprintf(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return ret >= (int)n ? (int)n - 1 : ret;
}
#include "helpers.h"
static char value[128];
static void *writer(void *arg)
{
    char text[127], out[PAGE_SIZE];
    memset(text, (int)(long)arg, sizeof(text));
    for (int i = 0; i < 5000; ++i) {
        assert(thermal_message_string_store(value, sizeof(value), text,
                                            sizeof(text)) == 127);
        assert(thermal_message_string_show(out, value) == 127);
        for (int j = 1; j < 127; ++j)
            assert(out[j] == out[0]);
    }
    return NULL;
}
int main(void)
{
    char out[PAGE_SIZE], large[PAGE_SIZE];
    const char literal[] = "25.0 %s %p %n %%\n";
    assert(thermal_message_string_show(out, value) == 0);
    assert(thermal_message_string_store(value, sizeof(value), literal,
                                        sizeof(literal) - 1) == sizeof(literal) - 1);
    assert(thermal_message_string_show(out, value) == sizeof(literal) - 1);
    assert(!strcmp(out, literal));
    memset(large, 'x', sizeof(large));
    for (size_t len = 128; len <= sizeof(large); len += 128) {
        assert(thermal_message_string_store(value, sizeof(value), large, len) == -E2BIG);
        assert(!strcmp(value, literal));
    }
    assert(thermal_message_string_store(value, sizeof(value), large, 127) == 127);
    assert(value[127] == '\0');
    assert(thermal_message_string_show(out, value) == 127);
    assert(!memcmp(out, large, 127));
    assert(thermal_message_string_store(value, sizeof(value), "", 0) == 0);
    assert(thermal_message_string_show(out, value) == 0);
    pthread_t threads[4];
    for (long i = 0; i < 4; ++i)
        assert(!pthread_create(&threads[i], NULL, writer, (void *)('a' + i)));
    for (int i = 0; i < 4; ++i)
        assert(!pthread_join(threads[i], NULL));
    puts("PASS: literal formats, 127-byte limit, oversized rejection, empty values, 20000 concurrent round trips");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="venus-thermal-test-") as tmp:
    tmp = Path(tmp)
    (tmp / "helpers.h").write_text(function("thermal_message_string_show") + "\n" +
                                  function("thermal_message_string_store"))
    (tmp / "test.c").write_text(harness)
    subprocess.run([os.environ.get("HOSTCC", "clang"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-fsanitize=address,undefined", "-pthread",
                    str(tmp / "test.c"), "-o", str(tmp / "test")], check=True)
    subprocess.run([str(tmp / "test")], check=True)
