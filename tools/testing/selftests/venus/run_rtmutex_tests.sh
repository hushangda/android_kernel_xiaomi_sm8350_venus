#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Usage: run_rtmutex_tests.sh [output-directory] [pre-fix-git-ref]
# The optional baseline must reject the bug regressions with a CHECK failure.
# Runs production functions with deterministic stubs, not the host futex syscall.
set -euo pipefail
test_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="$(cd "$test_dir/../../../.." && pwd)"
output_dir="${1:-$(mktemp -d /tmp/venus-rtmutex-tests.XXXXXX)}"
mkdir -p "$output_dir"
compiler="${HOSTCC:-cc}"
compile_flags=(-std=gnu11 -Wall -Wextra -Werror -g -O1
  -finstrument-functions -fsanitize=undefined -fno-sanitize-recover=all)
if [[ ${RTMUTEX_ASAN:-0} == 1 ]]; then
  compile_flags+=(-fsanitize=address)
fi

make_harness() {
  python3 - "$test_dir/rtmutex_test.c" "$1" "$2" <<'PY'
import pathlib
import re
import sys

harness, source, output = map(pathlib.Path, sys.argv[1:])
text = source.read_text()
functions = (
    "task_blocks_on_rt_mutex", "remove_waiter",
    "__rt_mutex_start_proxy_lock", "rt_mutex_start_proxy_lock",
)
parts = [harness.read_text()]
for function in functions:
    pattern = rf"^(?:static )?(?:int|void) {function}\("
    starts = list(re.finditer(pattern, text, re.MULTILINE))
    if len(starts) != 1:
        raise SystemExit(f"expected exactly one definition of {function}")
    start = starts[0].start()
    end = re.search(r"^}", text[start:], re.MULTILINE)
    if end is None:
        raise SystemExit(f"missing closing brace for {function}")
    parts.append(text[start:start + end.end()])
output.write_text("\n\n".join(parts) + "\n")
PY
}

make_harness "$source_dir/kernel/locking/rtmutex.c" "$output_dir/rtmutex_test.c"
"$compiler" "${compile_flags[@]}" "$output_dir/rtmutex_test.c" -o "$output_dir/rtmutex_test"
for scenario in remove-proxy remove-unenqueued remove-top remove-nontop \
                remove-chain start-acquired start-waiting self-deadlock \
                chain-rollback owner-released; do
  "$output_dir/rtmutex_test" "$scenario"
done

if [[ $# -ge 2 ]]; then
  git -C "$source_dir" show "$2:kernel/locking/rtmutex.c" > "$output_dir/rtmutex_baseline.c"
  make_harness "$output_dir/rtmutex_baseline.c" "$output_dir/rtmutex_baseline_test.c"
  "$compiler" "${compile_flags[@]}" "$output_dir/rtmutex_baseline_test.c" -o "$output_dir/rtmutex_baseline_test"
  python3 - "$output_dir/rtmutex_baseline_test" <<'PY'
import subprocess
import sys

for scenario in ("remove-proxy", "remove-unenqueued", "start-acquired",
                 "self-deadlock", "chain-rollback", "remove-chain"):
    result = subprocess.run([sys.argv[1], scenario], capture_output=True, text=True)
    if result.returncode == 0 or "CHECK failed" not in result.stderr:
        raise SystemExit(f"baseline did not fail the expected invariant: {scenario}\n"
                         f"{result.stdout}{result.stderr}")
    print(f"PASS baseline sensitivity: {scenario} rejects pre-fix source")
PY
fi
printf 'Host test sources and binaries: %s\n' "$output_dir"
