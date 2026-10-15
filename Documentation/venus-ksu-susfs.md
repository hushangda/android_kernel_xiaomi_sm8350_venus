# Venus KSU / SuSFS integration

The Linux 5.4.302 Venus tree uses these pinned upstream sources:

- BakaSU (formerly ReSukiSU):
  https://github.com/Baka-SU/BakaSU/commit/d7260ca1e5c128061122924d0d48754e361e135c
- SuSFS `gki-android12-5.10`:
  https://gitlab.com/simonpunk/susfs4ksu/-/commit/ca7093a1a26f7911987110c34a1352d74cfb1d09
  The upstream version string is still `v2.3.0`. This is the October 15, 2026
  source update, not the older September snapshot with the same version string.

## Retained local behavior

`CONFIG_KSU_SUKISU_UAPI5_COMPAT=y` selects SukiSU Ultra 4.2.1 compatibility
using native UAPI 5 and scoped driver-fd permissions. The SukiSU certificate
remains validated. Its mount-list ioctl and absent-KPM probe are translated;
GET_INFO retains BakaSU's real version and UAPI. The full-version query includes
`+sukisu-uapi5-compat`. Do not use the old 40900/UAPI 2 identity for this manager.

The separate, opt-in `CONFIG_KSU_SUKISU_420_COMPAT` remains available for an
actual SukiSU 4.2.0 manager/daemon, but is disabled in the Venus defconfig and
build script. The two compatibility modes cannot be enabled together.

When upgrading a phone from the legacy mode, install the official manager's
UAPI 5 ksud together with the new kernel. An old `/data/adb/ksud` reporting
UAPI 2 must not be retained for the new kernel. Preserve both the original boot
image and daemon for rollback. Host compilation does not perform this upgrade.

The previously reverted LSPosed port remains absent. Linux modules stay enabled;
DEBUG_FS and PAGE_OWNER stay disabled. CI can produce explicitly unsigned
manager artifacts without repository signing secrets; formal releases still
require all signing credentials.

## SuSFS backport

The existing 5.4 inode-flag and fsnotify adaptations remain in place. The latest
SuSFS core `fs/susfs.c`, `susfs.h`, and `susfs_def.h` have no upstream changes
relative to the previously used `9892175b` snapshot. The updated kernel patch
changes SELinux integration from duplicated wrappers to inline checks in the
native status, context, access and setprocattr paths. Its removed unused execve
extern was already absent from this tree.

SuSFS's patch against tiann/KernelSU cannot be applied wholesale to BakaSU.
BakaSU d7260ca1 uses a dual-policy parser and no longer provides the old
`fake_state` object. The versioned interface in `include/linux/susfs_selinux.h`
connects the updated inline flow to BakaSU's own context/SID/access helpers.
This retains live SID-table updates, context hash handling, native permission
checks and the original setprocattr transition checks. Context parsing returns
the live SID/error together, without a second parse through the old wrapper.
The status page is selected only after SELinux hiding is successfully running.

The KSU build check requires both updated hooks and interface version 1, and
rejects remaining `fake_state` hooks. This backport is scoped to Linux 5.4;
other SuSFS kernel integrations retain a build error until separately ported.

## Build and validation boundary

Build with a separate output directory:

```sh
JOBS=4 OUT_DIR="$PWD/out-venus-sukisu-uapi5-20261016" KPM=0 MGLRU=0 ./build_venus.sh
```

Build and host tests do not establish device boot, root authorization, module
behavior, or SELinux hiding effectiveness. Those require flashing and runtime
verification. No device flashing is part of this source update.
