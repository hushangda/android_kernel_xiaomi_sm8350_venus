Venus compatibility fixes for the ported ROM
===========================================

These changes address interfaces exercised by the November 2026 ported ROM.
The initial diagnosis used f2a9aebe. The later live check found b14ec7d0 plus
the first repair set: the boot_a kernel payload matched SHA-256
1feb4f09526ca157e7e90c9047dcc2d73aae6dd8064611df094de8434012469d.
MILLET delivery and the real charge-counter value were observed on that
kernel. The later display, scheduler, memory and board-temperature changes
are separate work and require booting their image; host tests do not prove
device stability, successful ROM installation or suspend/resume behavior.

Freezer notifications
---------------------

MILLET must recognize both legacy freezing()/PF_FROZEN and the unified
hierarchy's cgroup.freeze request.  The latter uses a different task state.
The existing cgroup_task_freeze() helper provides the RCU-protected lookup.
This lets the existing Binder/signal notification and userspace Greeze
policy handle cgroup-v2 tasks; it does not forcibly thaw them, change queue
limits or discard Binder transactions.

Audio and charging
------------------

Only automotive routing registers the COPP command/control pair and its
event producer.  Shared PCM open/close now selects the corresponding queue
with the same platform condition as Kbuild.  Phone ASM callbacks and ADM
parameter handling retain their existing paths.  ADM event repacking also
allocates the twelve bytes of user metadata that it actually copies.

The read-only qcom-battery/charge_counter attribute delegates to the battery
power_supply property, keeping microamp-hours and backend error returns.
See ABI/testing/sysfs-class-qcom-battery-venus for the interface contract.

Thermal message text
--------------------

boost and board_sensor_second_temp relay userspace text.
They are not sensor acquisition APIs.  Their 128-byte storage now accepts
at most 127 bytes, preserves literal percent signs and serializes readers
and writers.  Oversized writes return E2BIG without changing the value.
This fixes format-string interpretation and out-of-bounds writes, without
inventing board or ambient temperature readings. The board sample is now a
typed mC relay shared by thermal_message and qcom-battery. It rejects malformed
reads and expires after 30 seconds of boottime, including suspend. This keeps
a crashed temperature producer from leaving an indefinitely valid old value.

tools/thermal/venus-board-temp contains the missing producer, the measured
Venus six-sensor model, init service and narrow policy additions. It only
publishes temperature. The temporary producer was tested in the real
mi_thermald domain with SELinux Enforcing, including periodic updates and
termination invalidation; temporary changes were then reverted. The new
kernel alias and expiry still require a new-kernel boot.

Display, QoS and memory backends
-------------------------------

The legacy 15-field disp_count, count-info ioctl 0xc030440d and verified
features 35/39/40 use real panel state, commit/watchdog timestamps, brightness
commands and AOD/FOD restrictions. Unknown/unimplemented subtypes return an
error. See ABI/testing/sysfs-class-mi-display-info and the display selftests.

The native xr_fbt ABI implements the four actual v2 ioctl commands, task
credentials, scheduler contributions, bounded VIP service, WALT transfer and
dependency ownership. Original task/rq/lock field offsets are retained through
reserved storage; no private OEM offsets or forged module CRCs are used.
The dedicated device label, ueventd mode and four ioctl permissions are ROM
changes, not things a kernel node can supply. Failed library open results are
cached by existing processes, so a new-kernel cold boot with the complete
policy must validate the actual framework/application callers.

MCTRL/XSWAPD tracks real ZRAM owners, actual compressed size, refaults and
storage ownership. Reclaim is request-scoped, cancellable and bounded, and
reports actual progress. ZRAM writeback/readback pins live backing blocks;
the zram-control ABI validates extent maps and uses real software AES-XTS or
inline encryption. SmartCache tags real inodes, charges/migrates file pages
with native memcg accounting, and uses bounded, reclaimable protection.
The options default off in Kconfig. ROM_COMPAT=1 enables their complete core
backends in build_venus.sh; it does not enable the incompatible old MIMD HAL
to consume the new event FIFO. See the per-feature ABI/admin documentation.

SELinux policy export
---------------------

The running root policy export returned EINVAL. The new snapshot code measures
the actual serialized policy, allocates outside policy_rwlock and rechecks
under the lock before writing, with bounded retries if it grows. This avoids
treating the original loaded length as a permanent capacity after dynamic
updates. No permission decision or enforcing state is changed. Actual writer
call-chain review and extracted-function tests passed; the suspected live
failure cause must still be confirmed by exporting on the new kernel.

MIMD companion and remaining adaptation work
-------------------------------------------

The current legacy MIMD HAL parses event text with atoi; it cannot consume
the newer colon-delimited, read-consumed FIFO. The completed optional ROM
companion redirects its single event path to mimdlegacy and supplies the
parallel stock AIDL service in system_ext with its matching interface,
configuration, init launcher, labels and dedicated policy. The new service
uses mimdtrigger; the two consumers cannot drain each other's event queue.
The launcher validates and prepares real v1 memory cgroups before exec, using
write-only process_fg and read-only metadata opens for event attributes.
It does not read/consume events or trigger IN_CLOSE_WRITE during preparation.

ROM_COMPAT=1 MIMD_V2=0 builds the core image; ROM_COMPAT=1 MIMD_V2=1 enables
the complete MIMD backend. The latter must be paired with the complete
core-plus-MIMD companion, which already contains the core ROM changes.
Both profiles passed fresh complete ARM64 Image.gz builds on 2026-11-02.
The new daemon's real-device linker resolution passed without executing
constructors/main; startup, Binder registration, reclaim and stability in
its new enforcing domain still require booting that matching combination.
Enabling its kernel nodes alone is not a complete repair.

Ambient temperature needs a Venus-compatible algorithm and calibration. The
available two-coefficient configuration does not match the stock publisher's
27-coefficient model, and old deci-C output is incompatible with the current
Java consumer. No zero stub or unrelated sensor alias is exposed. Display
count-info subtype 8 also lacks a verified unit/reset contract for this ROM;
it continues to return EOPNOTSUPP rather than pretending to perform a reset.

The remaining observed ADSP unsupported status was traced to four parameters
not implemented by the actual DSP speaker module. A separate ACDB companion
removes only those requests from the affected speaker vectors, preserving
the nine supported parameters and all other table references. It does not
turn a DSP failure into a successful kernel return. Reloaded calibration and
normal playback still need device validation after that package is installed.

Host regression commands
------------------------

::

    python3 tools/testing/selftests/venus/millet_freezer_test.py
    python3 tools/testing/selftests/venus/millet_delivery_test.py
    python3 tools/testing/selftests/venus/thermal_message_test.py
    python3 tools/testing/selftests/venus/audio_compat_test.py

The tests exercise extracted production functions, not a booted device.
After booting, verify actual MILLET delivery and Greeze thaw/queue drainage,
charge-counter consistency with battery power_supply, and normal audio
playback/recording.  Display/AOD and suspend/resume also need device tests.
