#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KPM="${KPM:-0}"
if [[ "$KPM" =~ ^(1|y|Y|yes|YES|true|TRUE|on|ON)$ ]]; then
  KPM=1
elif [[ "$KPM" =~ ^(0|n|N|no|NO|false|FALSE|off|OFF)$ ]]; then
  KPM=0
else
  echo "ERROR: KPM must be 1 or 0" >&2
  exit 1
fi
# Keep MGLRU compiled but disabled by default pending vendor reclaim fixes.
# MGLRU=1 remains an explicit opt-in for future compatibility testing.
MGLRU="${MGLRU:-0}"
if [[ "$MGLRU" =~ ^(1|y|Y|yes|YES|true|TRUE|on|ON)$ ]]; then
  MGLRU=1
elif [[ "$MGLRU" =~ ^(0|n|N|no|NO|false|FALSE|off|OFF)$ ]]; then
  MGLRU=0
else
  echo "ERROR: MGLRU must be 1 or 0" >&2
  exit 1
fi
# Enable EROFS per-CPU workers at normal priority by default.
# EROFS_PCPU=0 selects the established workqueue path.
EROFS_PCPU="${EROFS_PCPU:-1}"
if [[ "$EROFS_PCPU" =~ ^(1|y|Y|yes|YES|true|TRUE|on|ON)$ ]]; then
  EROFS_PCPU=1
elif [[ "$EROFS_PCPU" =~ ^(0|n|N|no|NO|false|FALSE|off|OFF)$ ]]; then
  EROFS_PCPU=0
else
  echo "ERROR: EROFS_PCPU must be 1 or 0" >&2
  exit 1
fi
# Complete native backends for the newer ROM ABI. The matching xr_fbt node
# label/permissions and thermal producer belong in the companion ROM package.
# MIMD v2 transport remains separate: the installed legacy HAL cannot parse it.
ROM_COMPAT="${ROM_COMPAT:-1}"
if [[ "$ROM_COMPAT" =~ ^(1|y|Y|yes|YES|true|TRUE|on|ON)$ ]]; then
  ROM_COMPAT=1
elif [[ "$ROM_COMPAT" =~ ^(0|n|N|no|NO|false|FALSE|off|OFF)$ ]]; then
  ROM_COMPAT=0
else
  echo "ERROR: ROM_COMPAT must be 1 or 0" >&2
  exit 1
fi
MIMD_V2="${MIMD_V2:-1}"
if [[ "$MIMD_V2" =~ ^(1|y|Y|yes|YES|true|TRUE|on|ON)$ ]]; then
  MIMD_V2=1
elif [[ "$MIMD_V2" =~ ^(0|n|N|no|NO|false|FALSE|off|OFF)$ ]]; then
  MIMD_V2=0
else
  echo "ERROR: MIMD_V2 must be 1 or 0" >&2
  exit 1
fi
if (( MIMD_V2 && !ROM_COMPAT )); then
  echo "ERROR: MIMD_V2 requires ROM_COMPAT=1 and the matching ROM companion" >&2
  exit 1
fi
# Keep interactive desktop sessions responsive by default.  Dedicated build
# hosts can still override this explicitly, for example JOBS=16.
JOBS="${JOBS:-2}"
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "ERROR: JOBS must be a positive integer" >&2
  exit 1
fi
if (( KPM )); then
  DEFAULT_OUT_DIR="$ROOT_DIR/out-venus-5.4.302-kpm"
else
  DEFAULT_OUT_DIR="$ROOT_DIR/out-venus-5.4.302"
fi
OUT_DIR="${OUT_DIR:-$DEFAULT_OUT_DIR}"

# Match the reference Venus boot image by default. These are display/build
# metadata; the source SUBLEVEL and feature checks remain at 5.4.302.
export KBUILD_KERNEL_VERSION="${KBUILD_KERNEL_VERSION:-5.4.274}"
export KBUILD_LOCALVERSION="${KBUILD_LOCALVERSION--qgki-gcc3ec22f5061}"
export KBUILD_BUILD_USER="${KBUILD_BUILD_USER:-builder}"
export KBUILD_BUILD_HOST="${KBUILD_BUILD_HOST:-pangu-build-component-vendor-472342-r50m5-nds1f-hgnpk}"
export KBUILD_BUILD_VERSION="${KBUILD_BUILD_VERSION:-1}"
export KBUILD_BUILD_TIMESTAMP="${KBUILD_BUILD_TIMESTAMP:-Wed Mar 12 07:40:57 UTC 2025}"
export KBUILD_BUILD_UTS_VERSION="${KBUILD_BUILD_UTS_VERSION:-#$KBUILD_BUILD_VERSION SMP PREEMPT $KBUILD_BUILD_TIMESTAMP f2fs-hash:6ee821ead5}"
export KBUILD_BUILD_COMPILER="${KBUILD_BUILD_COMPILER:-Android (6877366 based on r383902b1) clang version 11.0.2 (https://android.googlesource.com/toolchain/llvm-project b397f81060ce6d701042b782172ed13bee898b79), LLD 11.0.2 (https://android.googlesource.com/toolchain/llvm-project b397f81060ce6d701042b782172ed13bee898b79)}"
export TZ=UTC LC_ALL=C
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(date -u -d "$KBUILD_BUILD_TIMESTAMP" +%s)}"

# Use the same Android Clang/LLD release as the reference image. A different
# installation location can be supplied with TOOLCHAIN_DIR.
TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-$(dirname "$ROOT_DIR")/toolchains/clang-r383902b1}"
if [[ ! -x "$TOOLCHAIN_DIR/bin/clang" || ! -x "$TOOLCHAIN_DIR/bin/ld.lld" ]]; then
  echo "ERROR: Android Clang toolchain not found at $TOOLCHAIN_DIR" >&2
  echo "Set TOOLCHAIN_DIR to the clang-r383902b1 installation directory." >&2
  exit 1
fi
export PATH="$TOOLCHAIN_DIR/bin:$PATH"
export LD_LIBRARY_PATH="$TOOLCHAIN_DIR/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export HOSTCC="${HOSTCC:-gcc}"
export HOSTCXX="${HOSTCXX:-g++}"

export ARCH=arm64
export SUBARCH=arm64
export LLVM=1
export LLVM_IAS=1
export CC=clang
export LD=ld.lld
export AR=llvm-ar
export NM=llvm-nm
export OBJCOPY=llvm-objcopy
export OBJDUMP=llvm-objdump
export STRIP=llvm-strip
export READELF=llvm-readelf

MAKE_ARGS=(
  O="$OUT_DIR"
  ARCH="$ARCH"
  SUBARCH="$SUBARCH"
  LLVM="$LLVM"
  LLVM_IAS="$LLVM_IAS"
  CC="$CC"
  LD="$LD"
  AR="$AR"
  NM="$NM"
  OBJCOPY="$OBJCOPY"
  OBJDUMP="$OBJDUMP"
  STRIP="$STRIP"
  READELF="$READELF"
)

# Keep the reference release independent of the current Git commit.
MAKE_ARGS+=(KERNELVERSION="$KBUILD_KERNEL_VERSION" LOCALVERSION=)

# Keep host-tool compilation independent from the target compiler sysroot.
# Pass host compiler settings on make's command line because the kernel
# Makefile otherwise overrides environment values.
[[ -n "${HOSTCC:-}" ]] && MAKE_ARGS+=(HOSTCC="$HOSTCC")
[[ -n "${HOSTCXX:-}" ]] && MAKE_ARGS+=(HOSTCXX="$HOSTCXX")
[[ -n "${HOSTCFLAGS:-}" ]] && MAKE_ARGS+=(HOSTCFLAGS="$HOSTCFLAGS")
[[ -n "${HOSTCXXFLAGS:-}" ]] && MAKE_ARGS+=(HOSTCXXFLAGS="$HOSTCXXFLAGS")
[[ -n "${HOSTLDFLAGS:-}" ]] && MAKE_ARGS+=(HOSTLDFLAGS="$HOSTLDFLAGS")
cd "$ROOT_DIR"
mkdir -p "$OUT_DIR"

make "${MAKE_ARGS[@]}" venus_defconfig
CONFIG_ARGS=(
  --enable VSOCKETS \
  --enable VSOCKETS_DIAG \
  --enable VIRTIO_VSOCKETS \
  --enable ZRAM \
  --enable ZRAM_MULTI_COMP \
  --enable ZRAM_ZSTD_PARAMS \
  --enable CRYPTO_ZSTD \
  --enable CRYPTO_LZ4 \
  --enable ZRAM_DEF_COMP_LZ4 \
  --disable ZRAM_DEF_COMP_LZORLE \
  --disable ZRAM_DEF_COMP_LZO \
  --disable ZRAM_DEF_COMP_ZSTD \
  --disable ZRAM_DEF_COMP_LZ4HC \
  --disable ZRAM_DEF_COMP_842 \
  --enable F2FS_FS_COMPRESSION \
  --enable F2FS_FS_LZO \
  --enable F2FS_FS_LZORLE \
  --enable F2FS_FS_LZ4 \
  --enable F2FS_FS_LZ4HC \
  --enable F2FS_FS_ZSTD \
  --enable F2FS_UNFAIR_RWSEM \
  --enable F2FS_CP_OPT \
  --enable KPROBES \
  --enable EXT4_FS \
  --enable HEADERS_INSTALL \
  --enable KSU \
  --enable KSU_MANUAL_SU \
  --disable KSU_DEBUG \
  --disable KSU_DISABLE_MANAGER \
  --disable KSU_DISABLE_POLICY \
  --enable KALLSYMS \
  --enable KALLSYMS_ALL \
  --enable BPF_STREAM_PARSER \
  --enable LRU_GEN \
  --disable LRU_GEN_STATS \
  --enable OPLUS_FEATURE_HANS \
  --enable MILLET
)

CONFIG_ARGS+=(--set-str LOCALVERSION "$KBUILD_LOCALVERSION" --disable LOCALVERSION_AUTO)

if (( MGLRU )); then
  CONFIG_ARGS+=(--enable LRU_GEN_ENABLED)
else
  CONFIG_ARGS+=(--disable LRU_GEN_ENABLED)
fi

if (( EROFS_PCPU )); then
  CONFIG_ARGS+=(--enable EROFS_FS_PCPU_KTHREAD)
else
  CONFIG_ARGS+=(--disable EROFS_FS_PCPU_KTHREAD)
fi
CONFIG_ARGS+=(--disable EROFS_FS_PCPU_KTHREAD_HIPRI)

if (( ROM_COMPAT )); then
  CONFIG_ARGS+=(--enable RCU_BOOST --enable XIAOMI_XR_QOS
    --enable XIAOMI_MEMCG_RECLAIM --enable XIAOMI_SMART_CACHE
    --enable ZRAM_WRITEBACK --enable KEYS --enable BLK_INLINE_ENCRYPTION
    --enable XIAOMI_ZRAM_EXTM)
fi
if (( MIMD_V2 )); then
  CONFIG_ARGS+=(--enable PERF_HELPER --enable BOOTUP_RECLAIM
    --enable XIAOMI_MIMD_TRIGGER --enable XIAOMI_MIMD_RECLAIM)
fi

if (( KPM )); then
  CONFIG_ARGS+=(--enable KPM)
else
  CONFIG_ARGS+=(--disable KPM)
fi

"$ROOT_DIR/scripts/config" --file "$OUT_DIR/.config" "${CONFIG_ARGS[@]}"
make "${MAKE_ARGS[@]}" olddefconfig
if (( ROM_COMPAT )); then
  for compat_config in RCU_BOOST XIAOMI_XR_QOS XIAOMI_MEMCG_RECLAIM \
    XIAOMI_SMART_CACHE ZRAM_WRITEBACK XIAOMI_ZRAM_EXTM; do
    grep -Fxq "CONFIG_${compat_config}=y" "$OUT_DIR/.config" || {
      echo "ERROR: requested ROM compatibility backend $compat_config is unavailable" >&2
      exit 1
    }
  done
fi
if (( MIMD_V2 )); then
  for mimd_config in PERF_HELPER BOOTUP_RECLAIM XIAOMI_MIMD_TRIGGER XIAOMI_MIMD_RECLAIM; do
    grep -Fxq "CONFIG_${mimd_config}=y" "$OUT_DIR/.config" || {
      echo "ERROR: requested MIMD backend $mimd_config is unavailable" >&2
      exit 1
    }
  done
fi
# Verify the requested workers survived dependency resolution; never select FIFO.
if (( EROFS_PCPU )); then
  grep -Fxq 'CONFIG_EROFS_FS_PCPU_KTHREAD=y' "$OUT_DIR/.config" || {
    echo "ERROR: EROFS per-CPU workers were requested but are unavailable" >&2
    exit 1
  }
fi
if grep -q '^CONFIG_EROFS_FS_PCPU_KTHREAD_HIPRI=y$' "$OUT_DIR/.config"; then
  echo "ERROR: EROFS high-priority workers must remain disabled" >&2
  exit 1
fi
# Check the requested config after Kconfig dependencies have been resolved.
# PAGE_OWNER must stay disabled because it selects DEBUG_FS.
for required_config in \
  'CONFIG_MODULES=y' \
  'CONFIG_VSOCKETS=y' \
  'CONFIG_VSOCKETS_DIAG=y' \
  'CONFIG_VIRTIO=y' \
  'CONFIG_VIRTIO_VSOCKETS=y' \
  'CONFIG_VIRTIO_VSOCKETS_COMMON=y' \
  '# CONFIG_DEBUG_FS is not set'; do
  grep -Fxq "$required_config" "$OUT_DIR/.config" || {
    echo "ERROR: Venus kernel config requires $required_config" >&2
    exit 1
  }
done
grep -q '^CONFIG_ZRAM_DEF_COMP="lz4"$' "$OUT_DIR/.config" || {
  echo "ERROR: ZRAM default compressor is not lz4" >&2
  exit 1
}
for required_config in \
  'CONFIG_ZRAM_ZSTD_PARAMS=y' \
  'CONFIG_CRYPTO_ZSTD=y' \
  'CONFIG_F2FS_FS_COMPRESSION=y' \
  'CONFIG_F2FS_FS_LZO=y' \
  'CONFIG_F2FS_FS_LZORLE=y' \
  'CONFIG_F2FS_FS_LZ4=y' \
  'CONFIG_F2FS_FS_LZ4HC=y' \
  'CONFIG_F2FS_FS_ZSTD=y' \
  'CONFIG_F2FS_UNFAIR_RWSEM=y' \
  'CONFIG_F2FS_CP_OPT=y'; do
  grep -q "^$required_config$" "$OUT_DIR/.config" || {
    echo "ERROR: missing $required_config" >&2
    exit 1
  }
done
for required_config in \
  'CONFIG_ZRAM_MULTI_COMP=y' \
  'CONFIG_LRU_GEN=y' \
  'CONFIG_ANDROID_VENDOR_FREEZER_COMPAT=y' \
  'CONFIG_OPLUS_FEATURE_HANS=y' \
  'CONFIG_MILLET=y'; do
  grep -q "^$required_config$" "$OUT_DIR/.config" || {
    echo "ERROR: missing $required_config" >&2
    exit 1
  }
done
if (( MGLRU )); then
  grep -q '^CONFIG_LRU_GEN_ENABLED=y$' "$OUT_DIR/.config" || {
    echo "ERROR: CONFIG_LRU_GEN_ENABLED must be enabled for MGLRU=1" >&2
    exit 1
  }
else
  grep -q '^# CONFIG_LRU_GEN_ENABLED is not set$' "$OUT_DIR/.config" || {
    echo "ERROR: CONFIG_LRU_GEN_ENABLED must be disabled for MGLRU=0" >&2
    exit 1
  }
fi
grep -q '^# CONFIG_LRU_GEN_STATS is not set$' "$OUT_DIR/.config" || {
  echo "ERROR: CONFIG_LRU_GEN_STATS must stay disabled for production builds" >&2
  exit 1
}
# main registers probes and syscall tracepoints itself; no legacy inline hooks.
for required_config in \
  'CONFIG_KSU=y' \
  'CONFIG_KSU_MANUAL_SU=y' \
  'CONFIG_KPROBES=y' \
  'CONFIG_KRETPROBES=y' \
  'CONFIG_HAVE_SYSCALL_TRACEPOINTS=y' \
  'CONFIG_TRACEPOINTS=y' \
  'CONFIG_EXT4_FS=y' \
  'CONFIG_HEADERS_INSTALL=y' \
  '# CONFIG_KSU_DISABLE_MANAGER is not set' \
  '# CONFIG_KSU_DISABLE_POLICY is not set'; do
  grep -Fxq "$required_config" "$OUT_DIR/.config" || {
    echo "ERROR: SukiSU Ultra main requires $required_config" >&2
    exit 1
  }
done
if (( KPM )); then
  grep -Fxq 'CONFIG_KPM=y' "$OUT_DIR/.config" || {
    echo "ERROR: CONFIG_KPM is not enabled" >&2
    exit 1
  }
else
  grep -Fxq '# CONFIG_KPM is not set' "$OUT_DIR/.config" || {
    echo "ERROR: CONFIG_KPM must be disabled for this build" >&2
    exit 1
  }
fi

# Install UAPI headers before Kbuild is parsed so the preinit runner is included
# on the first build as well as subsequent incremental builds.
make "${MAKE_ARGS[@]}" headers_install

rm -f "$OUT_DIR/kernel/config_data" \
      "$OUT_DIR/kernel/config_data.gz" \
      "$OUT_DIR/kernel/configs.o" \
      "$OUT_DIR/kernel/.configs.o.cmd"
make -j"$JOBS" "${MAKE_ARGS[@]}" Image Image.gz dtbs

# These exact names are resolved by main's runtime probe registrations.
for ksu_symbol in syscall_regfunc syscall_unregfunc input_event \
  __arm64_sys_reboot __arm64_sys_ni_syscall sys_call_table security_hook_heads; do
  awk -v symbol="$ksu_symbol" '$3 == symbol { found = 1 } END { exit !found }' \
    "$OUT_DIR/System.map" || {
    echo "ERROR: missing SukiSU runtime hook target $ksu_symbol" >&2
    exit 1
  }
done

if [[ -x "$ROOT_DIR/scripts/extract-ikconfig" ]]; then
  EMBEDDED_CONFIG="$OUT_DIR/.config.embedded"
  "$ROOT_DIR/scripts/extract-ikconfig" "$OUT_DIR/arch/arm64/boot/Image.gz" > "$EMBEDDED_CONFIG"
  cmp -s "$OUT_DIR/.config" "$EMBEDDED_CONFIG" || {
    echo "ERROR: Image.gz embedded IKCONFIG does not match $OUT_DIR/.config" >&2
    diff -u --label ".config" "$OUT_DIR/.config" \
      --label "Image.gz IKCONFIG" "$EMBEDDED_CONFIG" | sed -n '1,120p' >&2 || true
    exit 1
  }
fi

KERNEL_IMAGE="$OUT_DIR/arch/arm64/boot/Image"
KERNEL_IMAGE_GZ="$OUT_DIR/arch/arm64/boot/Image.gz"
if [[ -n "${TWRP_DEVICE_DIR:-}" ]]; then
  TWRP_DEVICE_DIR="$(realpath "$TWRP_DEVICE_DIR")"
  if [[ ! -f "$TWRP_DEVICE_DIR/BoardConfig.mk" ||
        ! -d "$TWRP_DEVICE_DIR/prebuilt/venus" ]]; then
    echo "ERROR: TWRP_DEVICE_DIR must point to device/xiaomi/venus" >&2
    exit 1
  fi
  install -m 0644 "$KERNEL_IMAGE" "$TWRP_DEVICE_DIR/prebuilt/venus/kernel"
  if [[ -f "$TWRP_DEVICE_DIR/prebuilt/modules.load.recovery" ]]; then
    cp -a "$TWRP_DEVICE_DIR/prebuilt/modules.load.recovery" \
      "$TWRP_DEVICE_DIR/prebuilt/modules.load.recovery.bak"
    cat > "$TWRP_DEVICE_DIR/prebuilt/modules.load.recovery" <<'EOF'
# This kernel builds the Venus recovery display/touch/charger paths in-tree.
# Do not load stale prebuilt modules from the 5.4.147 TWRP kernel package.
EOF
  fi
  if grep -q '^TW_LOAD_VENDOR_MODULES[[:space:]]*:=' "$TWRP_DEVICE_DIR/BoardConfig.mk"; then
    cp -a "$TWRP_DEVICE_DIR/BoardConfig.mk" "$TWRP_DEVICE_DIR/BoardConfig.mk.bak"
    sed -i -E 's/^TW_LOAD_VENDOR_MODULES[[:space:]]*:=.*/TW_LOAD_VENDOR_MODULES := ""/' \
      "$TWRP_DEVICE_DIR/BoardConfig.mk"
    echo "TWRP vendor module autoload disabled: $TWRP_DEVICE_DIR/BoardConfig.mk"
  fi
  echo "TWRP prebuilt kernel updated: $TWRP_DEVICE_DIR/prebuilt/venus/kernel"
fi

echo "SukiSU Ultra main: enabled (runtime kprobes + syscall tracepoints)"
echo "KPM: $([[ "$KPM" == 1 ]] && echo enabled || echo disabled)"
echo "Multi-Gen LRU: compiled, default $([[ "$MGLRU" == 1 ]] && echo enabled || echo disabled) (MGLRU=$MGLRU)"
echo "F2FS compression: enabled"
echo "F2FS performance options: unfair rwsem + checkpoint fsync optimization enabled"
echo "Vendor freezer protocol 29: HANS/MILLET auto multiplex enabled"
echo "ZRAM multi-compressor recompression: enabled"
echo "ZRAM explicit Zstd compression levels: enabled (default compressor unchanged)"
echo "EROFS per-CPU workers: $([[ "$EROFS_PCPU" == 1 ]] && echo enabled || echo disabled) (normal priority)"
echo "Native ROM compatibility backends: ROM_COMPAT=$ROM_COMPAT"
echo "MIMD v2 matched-ROM backends: MIMD_V2=$MIMD_V2"
echo "Image (for TWRP prebuilt/venus/kernel): $KERNEL_IMAGE"
echo "Image.gz: $KERNEL_IMAGE_GZ"
