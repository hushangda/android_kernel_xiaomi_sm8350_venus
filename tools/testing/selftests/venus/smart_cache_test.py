#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual SmartCache policy and native charge-moving code under sanitizers.

Only OS primitives and page/cgroup fixtures are mocked; no copied policy model.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
source = (ROOT / "mm/memcontrol.c").read_text()


def function(name):
    match = re.search(r"^(?:static )?(?:int|void) " + name + r"\(", source, re.M)
    assert match, name
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


with tempfile.TemporaryDirectory(prefix="venus-smart-cache-test-") as temp:
    temp = Path(temp)
    (temp / "move.h").write_text("\n".join(function(name) for name in (
        "mem_cgroup_move_account_locked", "mem_cgroup_move_account",
        "mi_smart_cache_move_end", "mi_smart_cache_move_page")))
    (temp / "policy.h").write_text("\n".join(
        line for line in (ROOT / "mm/mi_smart_cache.c").read_text().splitlines()
        if not line.startswith("#include")))
    subprocess.run([os.environ.get("HOSTCC", "clang"), "-std=gnu11", "-g", "-O1",
                    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-unused-function", "-Wno-unused-variable", "-no-pie",
                    "-fsanitize=address,undefined", "-pthread", "-I", str(temp),
                    str(Path(__file__).with_suffix(".c")), "-o", str(temp / "test")],
                   check=True)
    subprocess.run([str(temp / "test")], check=True)
