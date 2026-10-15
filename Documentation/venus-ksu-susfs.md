# Venus KSU / SuSFS integration

The Linux 5.4.302 Venus tree uses these pinned upstream sources:

- BakaSU (formerly ReSukiSU):
  https://github.com/Baka-SU/BakaSU/commit/d7260ca1e5c128061122924d0d48754e361e135c
- SuSFS `gki-android12-5.10`:
  https://gitlab.com/simonpunk/susfs4ksu/-/commit/ca7093a1a26f7911987110c34a1352d74cfb1d09
  The upstream version string is still `v2.3.0`. This is the October 15, 2026
  source update, not the older September snapshot with the same version string.

## Retained local behavior

`CONFIG_KSU_SUKISU_420_COMPAT=y` selects the SukiSU Ultra 4.2.0 ABI:
version code 40900, UAPI 2, legacy driver-fd discovery and mount-list ioctl.
Per-fd authorization and certificate validation remain enforced. The SukiSU
certificate is accepted only with this compatibility option enabled. Disabling
it restores BakaSU's native UAPI 5. The full-version query identifies the actual
source plus the compatibility suffix.

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
JOBS=4 OUT_DIR="$PWD/out-venus-bakasu-susfs-20261015" KPM=0 MGLRU=0 ./build_venus.sh
```

Build and host tests do not establish device boot, root authorization, module
behavior, or SELinux hiding effectiveness. Those require flashing and runtime
verification. No device flashing is part of this source update.
