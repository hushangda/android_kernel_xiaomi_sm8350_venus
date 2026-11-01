#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the unchanged kernel Zstd sources with small host-only OS shims."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(__file__).resolve().parents[4]
out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp(prefix="venus-zram-codec-"))
out.mkdir(parents=True, exist_ok=True)
headers = {
    "linux/types.h": """
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
""",
    "linux/compiler.h": """
#pragma once
#include <stdint.h>
#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif
#define noinline __attribute__((noinline))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
""",
    "linux/kernel.h": """
#pragma once
#include <linux/types.h>
#include <linux/compiler.h>
#include <string.h>
#include <errno.h>
#define ALIGN(x,a) (((x) + ((a)-1)) & ~((typeof(x))(a)-1))
#define PTR_ALIGN(p,a) ((typeof(p))ALIGN((uintptr_t)(p), (a)))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define div_u64(a,b) ((a) / (b))
""",
    "linux/mm.h": "#define PAGE_SIZE 4096U\n#define PAGE_SHIFT 12\n",
    "linux/math64.h": "#include <linux/kernel.h>\n",
    "linux/string.h": "#include <string.h>\n",
    "linux/errno.h": "#include <asm-generic/errno-base.h>\n#include <asm-generic/errno.h>\n",
    "linux/module.h": """
#define EXPORT_SYMBOL(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
""",
    "linux/slab.h": """
#pragma once
#include <stddef.h>
#define GFP_KERNEL 0
void *zram_test_alloc(size_t size);
void zram_test_free(const void *ptr);
#define kzalloc(size, flags) zram_test_alloc(size)
#define kfree(ptr) zram_test_free(ptr)
""",
    "linux/vmalloc.h": """
#include <linux/slab.h>
#define vzalloc(size) zram_test_alloc(size)
#define vfree(ptr) zram_test_free(ptr)
""",
    "asm/unaligned.h": """
#pragma once
#include <stdint.h>
#include <string.h>
#include <endian.h>
#define get_unaligned(p) ({ typeof(*(p)) v; memcpy((void *)&v, (p), sizeof(v)); v; })
#define put_unaligned(v,p) do { typeof(*(p)) x = (v); memcpy((p), &x, sizeof(x)); } while (0)
""",
}
for bits in (16, 32, 64):
    for byteorder in ("le", "be"):
        headers["asm/unaligned.h"] += (
            f"static inline uint{bits}_t get_unaligned_{byteorder}{bits}(const void *p) "
            f"{{ uint{bits}_t v; memcpy(&v, p, sizeof(v)); return {byteorder}{bits}toh(v); }}\n"
            f"static inline void put_unaligned_{byteorder}{bits}(uint{bits}_t v, void *p) "
            f"{{ v = hto{byteorder}{bits}(v); memcpy(p, &v, sizeof(v)); }}\n"
        )
for name, contents in headers.items():
    path = out / "include" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)
for name in ("zstd.h", "xxhash.h"):
    (out / "include/linux" / name).write_text((source / "include/linux" / name).read_text())
files = [source / "lib/zstd" / (name + ".c") for name in (
    "compress", "decompress", "fse_compress", "huf_compress", "huf_decompress",
    "entropy_common", "fse_decompress", "zstd_common")]
files += [source / "lib/xxhash.c", source / "tools/testing/selftests/venus/zram_codec_test.c"]
binary = out / "zram_codec_test"
command = [os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-O1", "-g",
           "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
           # Match the kernel Makefile's pointer/overflow semantics. The legacy
           # Zstd implementation represents dictionary positions as offsets
           # from a temporarily null base; ordinary userspace flags differ.
           "-fno-delete-null-pointer-checks", "-fno-strict-overflow", "-no-pie",
           "-DCONFIG_ZRAM_ZSTD_PARAMS", "-I" + str(out / "include"),
           "-I" + str(source), "-o", str(binary)] + [str(path) for path in files]
subprocess.run(command, check=True)
subprocess.run([str(binary)], check=True)
