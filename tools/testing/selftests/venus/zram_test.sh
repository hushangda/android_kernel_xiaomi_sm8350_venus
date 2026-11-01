#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
test_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="$(cd "$test_dir/../../../.." && pwd)"
output_dir="${1:-$(mktemp -d /tmp/venus-zram-tests.XXXXXX)}"
mkdir -p "$output_dir"
extract_function() {
  sed -n "/^$1(/,/^}/p" "$2"
}
{
  cat "$test_dir/zram_params_test.c"
  extract_function 'char \*next_arg' "$source_dir/lib/cmdline.c" |
    sed '1s/^/static /'
  for signature in \
    'static void comp_algorithm_set' \
    'static int comp_algorithm_lookup' \
    'static ssize_t algorithm_params_store' \
    'static int zram_recompress' \
    'static ssize_t recompress_store'; do
    extract_function "$signature" "$source_dir/drivers/block/zram/zram_drv.c"
  done
} | "${HOSTCC:-cc}" -x c -std=gnu11 -Wall -Wextra -Werror \
  -Wno-unused-parameter -Wno-sign-compare -g -O1 -fsanitize=undefined \
  -fno-sanitize-recover=all -o "$output_dir/zram_params_test" -
"$output_dir/zram_params_test"
printf 'ZRAM host test binaries: %s\n' "$output_dir"
