=======================================
Venus metadata boot trace and kernel log
=======================================

This optional diagnostic build restores the Venus boot trace and persistent
printk capture used in the September 2026 debug kernel. It is controlled by
``CONFIG_VENUS_BOOTTRACE``, independently of ``CONFIG_DEBUG_INFO``.

Building
========

The normal build remains the default. To build the diagnostic kernel::

    source .build-tools/env.sh  # local toolchain, if needed on this machine
    DEBUG=1 JOBS=4 bash build_venus.sh

With the default KPM=0, output goes to ``out-venus-5.4.302-debug``. The kernel
release contains ``-DSL-qgki-debug``. The images are::

    out-venus-5.4.302-debug/arch/arm64/boot/Image
    out-venus-5.4.302-debug/arch/arm64/boot/Image.gz

``DEBUG=0 bash build_venus.sh`` builds the normal kernel with the logger
disabled. ``KPM=1 DEBUG=1`` selects ``out-venus-5.4.302-kpm-debug``. An explicit
``OUT_DIR`` overrides either default. Existing KPM, MGLRU, ReSukiSU and SuSFS
build options retain their existing behavior.

These are kernel images, not repacked boot images or flashable ZIP files.

Capture behavior
================

Early startup records stages and initcall entry/exit, result and monotonic
timestamp in an 8192-record static RAM buffer. Timestamps before the kernel
clock is initialized are zero. This does not enable ``initcall_debug``, raise
console verbosity, allocate early memory or start an early logging thread.
Recording is sealed before the first attempt to execute userspace init.

After a successful writable ext4/f2fs mount at exactly ``/metadata`` through
the mount syscall, the kernel exports the boot trace and starts ``venus_kmsg``.
The files are::

    /metadata/venus_boottrace-A.txt
    /metadata/venus_boottrace-B.txt
    /metadata/venus-kmsg-A.log
    /metadata/venus-kmsg-B.log

Each pair selects a missing slot first, otherwise the slot with the older
modification time. The selected file is truncated; the other slot is retained.
This is separate from Android boot partition slots. Newly created files have
mode 0600. Symbol names, initcall return values and the kernel release identify
the captured boot.

The printk reader starts at the oldest record still available in the ring,
then follows new records using its own cursor. It neither consumes syslog
records nor requires opening ``/dev/kmsg``. Ring overruns produce a marker.
Log file creation still needs to succeed in the metadata mounting task's
filesystem and security context.

The thread polls every 50 ms when idle and attempts to sync dirty output every
500 ms, including during continuous log output. It participates in the kernel
freezer. Writes and sync failures stop the capture instead of causing a retry
loop. A failed initial start can be retried by a later qualifying mount.

Compared with the earlier logger, capture is now bounded. Each kmsg slot is
limited to 8 MiB by default and receives a stop marker when the size limit is
reached. ``CONFIG_VENUS_BOOTTRACE_KMSG_MAX_MB`` permits 1--64 MiB in custom
configurations; ``build_venus.sh`` selects 8 MiB. Free space is checked at open
and periodically during capture; less than 4 MiB available stops the logger.
The size limit preserves the beginning of the boot rather than rotating away
its first messages. The boot trace remains bounded by its record count.

Reading logs and limitations
===========================

After boot, use a root shell or recovery that has mounted metadata::

    ls -lt /metadata/venus_boottrace-*.txt /metadata/venus-kmsg-*.log
    cat /metadata/venus_boottrace-A.txt
    tail -n 100 /metadata/venus-kmsg-A.log

Select A or B using the modification time and the kernel release in the file
header. Once installed, ``uname -r`` should contain ``-debug`` and, with access
to IKCONFIG, ``zcat /proc/config.gz`` should show ``CONFIG_VENUS_BOOTTRACE=y``.

If a boot hangs or panics before metadata is mounted, this facility cannot
save that boot to disk. Old files can belong to a previous boot. Printk
records overwritten before capture starts cannot be recovered. Filesystem
writeback or storage failure can also lose the most recent messages despite
periodic fsync. The logger captures kernel printk, not Android logcat, and
does not replace pstore/ramoops for crash capture.
