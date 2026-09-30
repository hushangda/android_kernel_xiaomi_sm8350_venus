#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Compile the actual ported C functions against deterministic host stubs.
# This does not access sysctl, sysfs, ADB, or the running kernel.
set -euo pipefail
test_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="$(cd "$test_dir/../../../.." && pwd)"
output_dir="${1:-$(mktemp -d /tmp/venus-miui-tests.XXXXXX)}"
mkdir -p "$output_dir"
compiler="${HOSTCC:-cc}"
compile_flags=(-x c -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter
  -g -O1 -fsanitize=undefined -fno-sanitize-recover=all -I"$source_dir")

extract_function() {
  sed -n "/^$1(/,/^}/p" "$2"
}

for variant in packing core_ctl touch touch_factory battery; do
  test="${variant%_factory}"
  variant_flags=()
  [[ "$variant" != touch_factory ]] || variant_flags+=(-DCONFIG_FACTORY_BUILD)
  {
    if [[ "$test" == packing || "$test" == core_ctl ]]; then
      sed -n '/^#define MIUI_POWER_ENHANCE_CLUSTER_PACKING/,/^extern unsigned int miui_power_enhance;/p' \
        "$source_dir/include/linux/sched/sysctl.h"
    fi
    if [[ "$test" == battery ]]; then
      sed -n '/^#define VENUS_BATTERY_MODEL_NAME/,/^#define VENUS_UI_SOC_FALLBACK_STEP_MS/p' \
        "$source_dir/drivers/power/supply/qti_battery_charger_venus.c"
    fi
    sed -n '1,$p' "$test_dir/${test}_test.c"
    case "$test" in
      packing)
        extract_function 'static int walt_miui_packing_cpu' "$source_dir/kernel/sched/fair.c"
        ;;
      core_ctl)
        extract_function 'int sys_miui_power_enhance_handler' "$source_dir/kernel/sched/walt/core_ctl.c"
        extract_function 'static void core_ctl_busy_thresholds' "$source_dir/kernel/sched/walt/core_ctl.c"
        ;;
      touch)
        extract_function 'static u8 fts_need_enter_lp_mode' "$source_dir/drivers/input/touchscreen/fts_spi/fts.c"
        for function in fts_resume_work fts_suspend_work fts_switch_mode_work; do
          extract_function "static void $function" "$source_dir/drivers/input/touchscreen/fts_spi/fts.c"
        done
        extract_function 'static int fts_drm_state_chg_callback' "$source_dir/drivers/input/touchscreen/fts_spi/fts.c"
        ;;
      battery)
        battery_source="$source_dir/drivers/power/supply/qti_battery_charger_venus.c"
        for signature in \
          'static bool battery_chg_capacity_uah_valid' \
          'static u32 battery_chg_refresh_pack_capacity_uah' \
          'static int battery_chg_read_fg_remaining_uah' \
          'static u32 battery_chg_effective_full_uah' \
          'static u32 battery_chg_ui_soc_step_ms' \
          'static void battery_chg_reset_ui_soc' \
          'static void battery_chg_restart_ui_soc' \
          'static int battery_chg_get_ui_capacity' \
          'static void battery_chg_ui_soc_work' \
          'static int battery_psy_get_prop'; do
          extract_function "$signature" "$battery_source"
        done
        ;;
    esac
  } | "$compiler" "${compile_flags[@]}" "${variant_flags[@]}" -o "$output_dir/${variant}_test" -
  "$output_dir/${variant}_test"
done
printf 'Host test binaries: %s\n' "$output_dir"
