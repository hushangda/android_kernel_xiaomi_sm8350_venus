Venus SM8850 backports
=====================

Source provenance and scope
---------------------------

Target baseline: ``b14ec7d0d4976fcecce8d367c1fe437cf6cad0a2`` (Linux 5.4.302).
Reference: ``YuzakiKokuban/android_kernel_xiaomi_sm8850``, branch ``main``,
commit ``800c99a9477cae73e47bb3a19b45a31d9a2c9e7a`` (Linux 6.12.69).

These are selected adaptations to the existing 5.4 APIs and vendor tree.
They do not replace the scheduler, device drivers, DTB/DTBO, or Android vendor
interfaces with those of SM8850.

Fix before features
-------------------

The rtmutex proxy-lock rollback must clear the actual waiter's
``pi_blocked_on`` under that task's ``pi_lock``. During PI-futex requeue,
the waiter need not be ``current``. Priority-chain propagation must use the
same waiter task. Also skip removal when the waiter was never enqueued, and
only clean up a failed proxy-lock attempt (negative return, not success 1).

The core change follows upstream commit
``3bfdc63936dd4773109b7b8c280c0f3b5ae7d349``
("rtmutex: Use waiter::task instead of current in remove_waiter()"), with
the unqueued-waiter guard and proxy success-path handling from the newer
implementation. It does not establish the cause of any prior phone freeze.

Selected features
-----------------

ZRAM recompression now accepts ``priority`` and ``max_pages``. The budget
counts attempted pages, including attempts that save no memory. Skipped
pages do not consume it. An omitted budget is unrestricted; zero performs
no compression. It is neither a scan-time bound nor a persistent cursor.
Existing size-class checks, slot locking, and default LZ4 selection remain.

``CONFIG_ZRAM_ZSTD_PARAMS`` adds explicit Zstd levels through
``algorithm_params`` before device initialization. Only explicitly configured
Zstd slots use the local workspace backend. Other slots retain the Crypto
API path. Level changes do not affect F2FS. See
``Documentation/admin-guide/blockdev/zram.rst`` for the interface and examples.

``CONFIG_EROFS_FS_PCPU_KTHREAD`` adds per-CPU kthread workers for asynchronous
decompression from atomic context. CPU hotplug withdraws the worker pointer,
waits for RCU readers, and drains queued work before destroying the worker.
The existing workqueue handles missing workers and allocation failures.
No filesystem format or synchronous decompression changes are involved.

Build and activation
--------------------

The regular ``build_venus.sh`` enables the ZRAM interfaces. Existing systems
retain their compressor parameters unless userspace explicitly sets them.
No automatic recompression daemon or Android init policy is installed.

EROFS per-CPU workers are opt-in, pending device measurements::

    JOBS=4 OUT_DIR=/absolute/path/to/fresh-output EROFS_PCPU=1 ./build_venus.sh

The script always leaves ``EROFS_FS_PCPU_KTHREAD_HIPRI`` disabled. Omitting
``EROFS_PCPU`` uses the established workqueue path. MGLRU's existing default
and the KernelSU configuration are preserved.

Host regression checks
----------------------

Run from the repository root::

    tools/testing/selftests/venus/run_rtmutex_tests.sh /tmp/venus-rtmutex \
        b14ec7d0d4976fcecce8d367c1fe437cf6cad0a2
    tools/testing/selftests/venus/zram_test.sh /tmp/venus-zram
    python3 tools/testing/selftests/venus/zram_codec_test.py /tmp/venus-zram-codec
    python3 tools/testing/selftests/venus/erofs_pcpu_test.py

The rtmutex tests extract production functions and show that the baseline
fails the relevant cases. ZRAM tests exercise the production parser and
recompression logic with bounded host mocks; the codec test compiles the
actual kernel Zstd library and backend with host allocation shims, testing
all levels with ASan and UBSan. EROFS tests exercise worker
lifecycle and dispatch functions in three configurations. These tests do
not emulate kernel concurrency, reclaim, I/O completion, or device suspend.

Device validation still required
-------------------------------

Successful compilation and image/config checks prove a build artifact,
not boot, deep sleep, or runtime performance. Validate PI-futex contention
and requeue under load; ZRAM read/write integrity across recompression and
CPU hotplug; EROFS reads during CPU online/offline transitions; and repeated
suspend/resume. Compare memory savings, CPU time, latency and power against
the same baseline before selecting compression levels or enabling EROFS
workers by default. Tests that reset or reconfigure ZRAM should use a spare
device, not the active swap device.
