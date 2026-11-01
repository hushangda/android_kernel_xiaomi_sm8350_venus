Xiaomi SmartCache on Venus
=========================

``CONFIG_XIAOMI_SMART_CACHE`` defaults to n and requires the real Xiaomi memcg
backend, MMU and sysfs. It adds the fd-level policy used by the current ROM's
SmartCacheService. It must be enabled only as part of the integrated memory
backend build; adding policy files without the charge/reclaim hooks is not a
functional implementation.

Protocol evidence
-----------------

The GPL-2.0 X-Ring 2024 ``mctrl.c`` and ``smart_cache.c`` were read at commit
``430688007431c0c69a3c5782c1766b3aee66c8cd`` in this source backup::

  https://github.com/Project-Hikari-Liella/MTK_kernel_device_modules-50da8b3c19a331c676dcaa1334fa3b49e4b1a531/tree/430688007431c0c69a3c5782c1766b3aee66c8cd/drivers/block/zram

This is a vendor module source backup, not a Lineage device kernel transplant.
Only the fd-level and level/max/ratio policy was adapted; the donor's inode
high bits, folio private-pointer scratch use, Android vendor hooks and
assumptions about a newer memcg migration helper were not copied.

The current device's miui-services.jar SHA-256 is
``e65d3479b22f116f8c0c1b670b8dc9af8047a3cf855f1206e4e3296a9f1c73cf``.
Its SmartCacheService writes fd-level pairs, three memory.mctrl.level/max
groups, enable and ratio. The checked ROM creates and chowns those groups and
nodes in init; its product XML sets ratio 50, and the 8 GiB profile sets
300 MiB/80 MiB/0 for levels 1/2/3. The source ABI and real consumer therefore
agree on byte units, level order, mount-point detection, and control names.
Init also chowns refault_distance/debug_log_enable, but no consumer or
verified semantics for those controls was found; no inert nodes are added.

Real backing operations
-----------------------

* The existing filemap insertion path sets mapping/index before calling
  mem_cgroup_charge. A locked, regular, tagged file page selects an online
  cache-group CSS there, then uses the ordinary try_charge/commit path.
  Allocation failure and insertion rollback retain normal memcg behavior.
* Tagging an already cached file also walks its actual page cache. It keeps
  source CSS/moving_account references across adjacent pages, waits for the
  RCU page-stat barrier, locks the page before sampling THP size, precharges
  the destination without reclaim/OOM, isolates from LRU, moves native VM
  statistics/ownership, cancels the source charge and puts the page back.
  Failure cancels only the provisional target charge. Partial progress is
  reported as an error, and repeated writes converge without double charge.
* The common shrink_page_list path covers classic LRU and MGLRU. Only a real
  file page owned by its currently tagged level may receive protection.
  Level/max/ratio form a bounded eviction policy. Unlike the newer donor's
  excess-page migration to root, excess pages stay honestly charged to their
  group and become reclaimable there. Reclaiming them reduces real usage.
  Targeted memcg reclaim and urgent priorities bypass protection; this is
  cache preference, not unevictable memory or a new OOM exemption.
* Overlayfs resolves the actual data file using its own ovl_real_fdget. No
  generic cast of another filesystem's private_data is used. Tags apply to
  the resolved inode at write time; later overlay copy-up needs a retag.

Lifetime and locking
--------------------

No existing inode, page, mem_cgroup, file or task layout is changed. Cache
level/max/dying metadata live in the already allocated private mi_memcg tail;
inode tags live in a bounded RCU hash table. Neither i_state nor page private
or reserved fields are repurposed. Existing exported function signatures are
unchanged. The existing static move_account operation is split into its
unchanged accounting body plus its existing trylock/unlock wrapper, so the
new caller can keep THP size and charge transfer under the same page lock.

sc_update_lock serializes userspace policy transactions only. sc_lock is a
leaf lock for tags, level slots and limits; it never waits, allocates, reclaims
or takes another subsystem lock. Page locking precedes sc_lock in reclaim;
no reverse path exists. Inode destruction unlinks tags before filesystem
freeing, then uses kfree_rcu. It never takes sc_update_lock. Cache tags do not
hold permanent inode references and therefore do not prevent unmount.

Level lookup pins CSS while sc_lock excludes offline slot removal. Offline
marks the tail as dying and clears the slot before normal memcg teardown.
Ongoing migration owns its destination CSS and source CSS/moving_account
references, even if the cgroup is concurrently offlined. All page-array,
isolation, CSS, and file references are balanced on each failure/abort path.
The memcg policy/ZRAM leaf lock is never acquired from SmartCache's sc_lock.

Validation boundary
-------------------

``tools/testing/selftests/venus/smart_cache_test.py`` compiles the complete
policy source and the actual memcontrol account/migration functions with
mocked OS primitives. Clang and GCC use ASan+UBSan with -no-pie (sanitizers
remain enabled). Tests cover charge/stat/CSS transfer, 512-page THP, failed
precharge/isolation/locking and concurrent owner change, source RCU barriers,
level replacement/conflict/offline, parser failure atomicity, partial retry,
timeout/signal cleanup, overlay resolution, 4096-tag capacity, 150000
concurrent lookups against inode removal, and initialization rollback.

ARM64 object builds with both MGLRU compiled in and classic-only configurations
validate kernel APIs. An ARM64 compiler layout probe with SmartCache on/off
also checks inode/page/mem_cgroup/file/task sizes and relevant member offsets:
all match. CONFIG_MODVERSIONS is off in this configuration, so no symbol CRC
claim is made. These host/build checks do not prove successful boot,
device SELinux authorization, real reclaim behavior, or long-duration load.
Those require the integrated image and separate real-device validation.
