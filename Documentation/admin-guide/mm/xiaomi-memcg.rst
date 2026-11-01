Xiaomi memory policy compatibility on Venus
==========================================

This is an opt-in implementation on the native SM8350 VM/ZRAM backend. All new
Kconfig options default to n. Merely compiling a backend does not turn on its
worker or configure a backing device. It has not been exercised on the phone.

Protocol references
-------------------

The public GPL-labelled X-Ring 2024 mctrl.c, xswapd.c, zgroup.c, zgroup_wb.c and
smart_cache.c reference is pinned at commit
430688007431c0c69a3c5782c1766b3aee66c8cd of
https://github.com/Project-Hikari-Liella/MTK_kernel_device_modules-50da8b3c19a331c676dcaa1334fa3b49e4b1a531 .
It is a public backup, not an authenticated official release. No private OEM
structure offsets, symbol CRC substitutions, MiCode or Lineage kernel code are
used. This port implements the contracts on the existing 5.4 backend rather
than importing the donor's MTK/private-layout hooks.

The current ROM's MiuiMemcgSwapService writes ``1 4096 4096`` to reclaim and one
integer percentage to swapin/swapout. The current system_perf_init binary
confirms inline key type 1=standard, 2=hardware-wrapped. These are independently
checked against the public protocol. One-field swapout completion is not proven
by the client: this port defines it as synchronous, with cancellation/deadline.
The public two-field form remains supported (second field 0 queues, 1 waits).

State, lifetime and locks
-------------------------

CONFIG_XIAOMI_MEMCG_RECLAIM allocates private state after the original variable
mem_cgroup allocation, past sizeof(mem_cgroup)+nodeinfo array. It does not add a
member to the public structure or move existing members. ARM64 DWARF layouts
with the option on/off compare byte-for-byte equal. This does increase allocation
size. ZRAM's private slot gains an owner CSS reference; its private device gains
backend state. No precompiled driver may assume this private ZRAM layout.

A slot pins its owning CSS until free/replacement. Hierarchical counters follow
parent CSS lifetime. Recompression updates compressed bytes without changing
ownership. RAM-to-backing and backing-to-RAM change the tier, preserving the
owner. Offline removes policy-list membership; outstanding slots retain valid
accounting storage. SAME entries, uncompressible entries and failed commits
follow the same accounting rules.

Lock order is registry mutex -> device init_lock -> slot -> leaf accounting
lock. Policy-list lock may precede leaf accounting. No accounting/slot lock spans
allocation, block IO or reclaim. Readback owns a physical-block reference until
IO completion, preventing discard/reallocation ABA. Compare-and-commit checks
WB/UNDER_WB/IDLE/block identity before installing a recompressed object.

Queued fault reads use a per-device WQ_MEM_RECLAIM workqueue, drained before
reset/free. Partial-page RMW waits for its scratch read before copying/freeing
that page. Allocation failures return errors without completing a parent bio
that was never submitted; asynchronous success/error completes exactly once.

Registered memory controls
--------------------------

Both legacy and unified memory cgroups receive independent cftype registrations.
The ``memory.`` prefix below is added by cgroup core.

* mctrl.name: up to 31 characters, used alongside real per-group accounting.
* mctrl.score: 0..1000; automatic reclaim skips <=100, higher scores first.
* mctrl.comp_ratio: 0..100; additional swap budget is the percentage of resident
  anon + compressed pages + backing pages, less already swapped pages.
* mctrl.refault_ratio: 0..100; recent refaults at/above the threshold skip a group.
* mctrl.wb_ratio (WRITEBACK): 0..100, enforced against this backend's actual bytes
  after moving a compressed object into a page-sized backing block.
* mctrl.stat: real comp_pages/comp_size/comp_fault/wb_pages/wb_size. Bytes are
  bytes; the donor's unimplemented drop/packed-extent counters are not invented.
* xswapd.reclaim: ``type target_MiB window_MiB``; types 0=inactive anon, 1=all anon,
  2=inactive file, 3=all file. Target 0 means available target-class pages; window
  0 defaults to 4 MiB. Requests validate ranges and overflow. Kernel calls are
  capped at 4 MiB even when a ROM asks for a 4096 MiB window.
* xswapd.swapin (WRITEBACK): percentage 0..100 of the group's actual backing bytes
  to decompress/recompress into ZRAM, not directly into process RAM.
* xswapd.swapout (WRITEBACK): percentage, optionally followed by sync 0/1. A
  one-field command waits. Async jobs have a real CSS pin and bounded queue.
* Root xswapd.enable / umrenable: 0/1; require a tracked initialized ZRAM device
  and swap capacity. enable controls automatic work. umrenable permits explicit
  requests without score protection. Both reset to disabled at boot.
* Root xswapd.stop_swap: bit 0 cancels anon reclaim, bit 1 cancels writeback. The
  relevant running operation consumes its stop bit; unrelated requests do not.
* Root xswapd.stat: real reclaim calls/pages/no-progress, tracked devices and
  actual write/readback bytes/errors/queued jobs.

The automatic worker uses low/high 2500/3000 MiB, at most 100 MiB per pass and a
1000 ms wake interval. It is freezable and bounded by progress, per-pass deadline,
visited-group count and request call count. Refault snapshots follow completed
worker rounds. The donor's separate 200 ms sampling and additional global tuning
nodes (group_param/watermark/wake/shrink/quota policy) are not exposed as inert
controls. The current checked framework consumes the controls listed above.

Scoped reclaim respects memory.low/min in the target subtree, traverses NUMA
nodes, and returns actual freed pages. Classic LRU respects anon/file and
inactive-only masks. Runtime MGLRU selects a fixed page class; inactive-only
requests do not age generations. Normal VM reclaim behavior is unchanged.

ZRAM setup and native backing-device ABI
---------------------------------------

``/sys/block/zramN/zgroup_enable`` is a real pre-disksize switch. It cannot change
while IO is possible. Without CONFIG_ZRAM_WRITEBACK the reclaim/accounting
backend still builds; writeback controls are absent.

CONFIG_XIAOMI_ZRAM_EXTM adds the real privileged ``/dev/zram-control`` ioctl ABI:
160-byte header, 16-byte extent entries, SET=0x40a05c01, GET=0xc0a05c02. It validates
reserved fields, terminated path, 32 KiB alignment, positive non-overlapping
extents, capacity and overflow. Logical pages map into the caller-supplied real
block extents. Setup is only allowed before disksize and with group mode enabled.
The caller must own/pin the allocated backing file/extents for the device's whole
lifetime; resetting ZRAM must precede deallocation of those extents.

Unlike the donor's 32 KiB packed compressed extents, this implementation uses the
existing driver's PAGE_SIZE uncompressed backing format. This consumes more
storage but is internal to the opaque userspace setup ABI; reported WB sizes and
ratios use the actual format. Standard and group writeback share quota reservations
before IO; failed IO refunds reservations, successful physical IO consumes quota
even if the logical slot changes concurrently.

Software AES-256-XTS uses a fresh 64-byte key and physical-page DUN/IV, not a
constant zero IV. Inline encryption imports only a validated fscrypt-provisioning
key under the key semaphore, checks payload/type/size, initializes the native
block-crypto key and checks device support. Raw temporary material is wiped,
keyring references are dropped, and programmed keys are evicted/freed at reset.
An unsupported/revoked/malformed key or unavailable crypto context fails; it does
not downgrade an encrypted request to plaintext. Actual UFS inline-key behavior
still requires device testing.

MIMD transport and reclaim
-------------------------

CONFIG_XIAOMI_MIMD_TRIGGER (PERF_HELPER=y) creates the actual bounded event FIFO at
/sys/module/perf_helper/mimd/mimdtrigger. Types 1..6 and their colon fields match
the framework/new daemon protocol. Reads consume one event; empty reads return
EOF. Writes to a full queue return ENOSPC. The daemon watches real IN_CLOSE_WRITE
notifications. Stats report accepted/consumed/pending/full counts.

The older HIDL daemon must watch the separate mimdlegacy attribute. This accepts
only real integer events 0..19, with one outstanding event and ENOSPC backpressure
because that consumer does not drain a FIFO. Neither channel fabricates events
or translates policy numbers. The companion changes exactly one NUL-terminated
path in the old daemon, preserving its Binder implementation and ELF layout.
dump_set retains the signed dump-notification value consumed by the daemon's
inotify watcher. /proc/mimdlog holds the latest 2048 actual daemon messages with
timestamps; each message is at most 127 bytes with a guaranteed terminator.

Stock SM8850 pandora binaries from Jiovanni-dump/xiaomi_pandora_dump commit
b3be90ee211267cc6c91a0a8c9d141ee13f6d23b independently prove the FIFO contract:
mimd2 TriggerEventParse watches mask 8, splits ':' and repeatedly opens/fgets the
attribute until empty; perf_helper.ko dequeues and frees each queued event.

CONFIG_XIAOMI_MIMD_RECLAIM additionally decodes /proc/global_reclaim commands:
prefix value/10000000 selects 2=anon, 3=file; the remainder is a page count. Prefix
0 is mixed. A 16-request FIFO runs real scoped reclaim, 4 MiB calls, 4096-call and
5-second bounds, no-progress exit, shutdown cancellation and cond_resched.
Mixed requests alternate real anon/file scans and stop after both make no
progress; this intentionally does not reproduce the donor's global scan_type
race or slab-inclusive implicit reclaim. Protected groups remain protected.

The same option provides memory.reclaim_once and memory.process_fg. The decimal
encoding is retries*100000000 + type*10000000 + pages: retries 1..9 are explicit,
otherwise 10; type 2=anon, 3=file, other digits=mixed. Reading reclaim_once returns
the last completed operation's actual reclaimed KiB. A positive process_fg write
cancels an in-flight request for that group; the next new request resets the flag.
Admission is serialized per group (EBUSY for a concurrent request), and an online
CSS reference protects lifetime. Offline, fatal signal, foreground, 5-second
deadline and 4096 backend-call limits all stop work. Full attempts are subdivided
into 4 MiB windows for cancellation; a short positive result consumes a retry.
These bounded operations may complete partially, and report only real progress.

The current phone has an older vendor HIDL 1.0 MIMD daemon which parses atoi(),
not the colon protocol, and lacks /odm/etc/mimdconfig.json. Do not enable either
MIMD option for that unmodified ROM. A matching ROM companion and permission /
service-lifecycle validation are required; a kernel event node alone cannot
repair the old consumer. Existing BOOTUP_RECLAIM must also stay off for the new
encoded JSON unless XIAOMI_MIMD_RECLAIM is enabled.

The prepared companion places MIMD2 and its NDK interface in system_ext. Static
checking found all strong ELF imports in the current system libraries, while
vendor VNDK30 lacks required C++ symbols. Shared libc++ is not replaced. Its JSON
corrects the donor's undefined ``dropcache`` action reference to ``drop_cache``
and omits the donor's global MGLRU-disable policy, since this port supports both
LRU modes. Static ELF/policy checks do not establish runtime Binder registration,
namespace loading, Enforcing permissions or functional boot-reclaim behavior.

Verification boundaries
-----------------------

The selftests under tools/testing/selftests/venus extract actual functions and
exercise policy math/parser fuzzing, concurrent accounting/CSS lifetime, transfer
races and faults, block pins, BIO completion, real OpenSSL XTS round trips, key
and capability errors, independent event-channel concurrency, bounded global and
per-group reclaim, foreground cancellation, and diagnostic log wrap/boundaries.
ARM64 object builds cover backend-on, writeback-off and backend-off combinations.
These do not establish a successful flash, UFS hardware encryption, Enforcing
service startup, suspend/deep-sleep stability or long-duration performance.
