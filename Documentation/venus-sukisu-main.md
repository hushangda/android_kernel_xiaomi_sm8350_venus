# Venus: SukiSU Ultra main integration

## Source and integration mode

The `KernelSU` submodule is a clean clone of
[SukiSU-Ultra/SukiSU-Ultra main](https://github.com/SukiSU-Ultra/SukiSU-Ultra/tree/main),
pinned for this integration to
[`b34a3c98b027c21e1e12aee21122b9115657296e`](https://github.com/SukiSU-Ultra/SukiSU-Ultra/commit/b34a3c98b027c21e1e12aee21122b9115657296e).
All compatibility changes belong to the Linux 5.4.302 kernel; the SukiSU source
is unmodified. Do not add compatibility patches inside the submodule.

Follow `KernelSU/docs/guide/how-to-integrate.md` for the branch's contract.
`main` registers kprobes/kretprobes and the syscall-entry tracepoint at runtime.
The kernel supplies `CONFIG_KPROBES`, `CONFIG_KRETPROBES`,
`CONFIG_HAVE_SYSCALL_TRACEPOINTS`, `CONFIG_TRACEPOINTS`, and `CONFIG_EXT4_FS`.
`CONFIG_KSU=y` builds the driver into the image. `CONFIG_KSU_MANUAL_SU=y`
is an upstream authorization feature, not the removed manual-hook mode.

The previous ReSukiSU/BakaSU compatibility modes, UAPI translation switches,
manual hook calls and SuSFS core/headers/hooks have been removed. SuSFS is not
part of upstream `main`; it must not be mixed back into this integration.
Manager authorization, signatures, version reporting and UAPI are supplied by
the unchanged upstream main source.

## Kernel API backports

- **SELinux:** the policy core is based on Android common
  [`222b41edc3a297e6b34d2a4c47084e3695e5d807`](https://android.googlesource.com/kernel/common/+/222b41edc3a297e6b34d2a4c47084e3695e5d807/security/selinux/)
  (`android12-5.10-2026-10_r1`). It supplies real RCU policies, policy mutex and
  commit/cancel lifecycle, compressed filename transitions, SID/context caches
  and status-page ownership. The 5.4 LSM entry points and vendor configuration
  behavior remain in place. Because main selects some APIs by kernel version,
  symbol tables retain the 5.4 pointer interface while hash tables also accept
  explicit key operations. Both interfaces operate on the same stored data.
  Policy snapshots count their serialized size while holding `policy_mutex`,
  check arithmetic overflow and clean up all failed allocations.
- **Seccomp:** real native/compat action caches and BPF constant-allow analysis
  are backported from Linux 5.11, together with filter `refs/users`, filter
  counts, task release and notification lifetime handling from 5.10.
  Relevant upstream commits include
  [`f9d480b6ffbe`](https://github.com/torvalds/linux/commit/f9d480b6ffbeb336bf7f6ce44825c00f61b3abae),
  [`8e01b51a31a1`](https://github.com/torvalds/linux/commit/8e01b51a31a1e08e2c3e8fcc0ef6790441be2f61),
  `ffde703470b0`, `c818c03b661c`, `b707ddee11d1`, `3a15fb6ed92c`,
  `76194c4e830d` and `99cdb8b9a573`. The ARM64 structure layout matches main's
  private `seccomp_filter` definition, including the compat bitmap.
- **Filesystem notifications:** the inode-event callback and rename cookie
  interface from `b9a1b9772509` / `950cc0d2bef0` is adapted to the 5.4 event
  dispatcher. Existing `handle_event` consumers retain their original path.
- **Mount namespaces:** `path_umount()` / `can_umount()` are based on Linux
  v5.10. The helper retains capability, namespace, mount-lock and LSM checks
  and consumes the path references on both success and failure. `path_mount()`
  extracts the existing mount implementation behind the v5.10 interface and
  borrows its path references; `do_mount()` retains the original userspace
  pathname lookup and cleanup. This supplies main's individual namespace mode.
- **Small API bridges:** nofault memory access uses the existing 5.4 fault-safe
  operations with their address-limit checks; task work exposes its supported
  NONE/RESUME notification modes; page-table/minmax headers expose the existing
  helpers; ARM64 folded-P4D address conversion matches this tree's typedef.
  The ARM/ARM64 arch timer and vDSO use the real `clocksource.vdso_clock_mode`
  field, including the timer erratum path.

## Build

Initialize the pinned dependency on a new checkout:

```sh
git submodule update --init KernelSU
```

Build using the supplied Android Clang 11 toolchain:

```sh
JOBS=4 OUT_DIR="$PWD/out-venus-sukisu-main-20261103" KPM=0 ./build_venus.sh
```

`build_venus.sh` installs the target UAPI headers before compiling the driver.
This ensures main's embedded preinit runner is built even on a fresh output
directory. The script verifies the resolved probe/tracepoint configuration, the exact
runtime hook names in `System.map`, and the image's embedded IKCONFIG. KPM is disabled in the default configuration;
`KPM=1` remains the upstream optional feature, not part of the default image.

Existing EROFS, ROM compatibility, MIMD and boot metadata defaults are preserved.
The Linux source version remains 5.4.302 even where boot display metadata is
set to the stock version.

## Validation boundary

Host validation covers compilation, link-time symbol availability, the embedded
configuration, seccomp structure offsets and cache/filter lifetime behavior,
and SELinux hash-table/snapshot behavior including allocation failure paths.
The `KPM=1` SELinux and SukiSU object build was also checked separately; this is
not a complete KPM image or runtime verification.
The snapshot regression test is
`tools/testing/selftests/venus/selinux_policy_snapshot_test.py`.

Device boot, probe registration, manager recognition, authorization and module
mount/unmount still require testing on Venus. In particular, verify the
`syscall_regfunc`, `syscall_unregfunc`, `input_event` and `__arm64_sys_reboot`
probe registrations, syscall dispatcher installation and init/ksud startup.
Compilation does not perform a device flash or upgrade `/data/adb/ksud`.
