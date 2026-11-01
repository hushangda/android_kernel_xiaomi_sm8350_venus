Native Xiaomi XR task QoS
========================

Scope and ABI
-------------

CONFIG_XIAOMI_XR_QOS implements the libxqos v2 interface observed in the current
Venus port. It defaults to n. It requires built-in Binder, futex, FUSE, WALT,
RT mutexes and RCU_BOOST, and excludes RT_GROUP_SCHED. The device is registered
only when this complete configuration is selected.

/dev/xr_fbt accepts an eight-byte request: u32 qos/result followed by s32 ID.
The commands are SET 0x40087807, GET_PID 0xc0087808, GET_TID 0xc0087809 and
one-shot asynchronous Binder inheritance 0x4008780b. GET_PID reports the
maximum effective level among the process's threads; GET_TID reports the
individual effective level. Neither returns packed policy flags. Unknown
commands return ENOTTY. Compat requests use the same fixed-width layout.

Bits 0..3 hold level 0 or 3..10. Bits 4..5 select normal, RT or VIP; value 3 is
invalid for a static request. Level 6 requires VIP and levels 7..10 require RT
or VIP. Bits 6..7 are retained. Level 0 revokes the static contribution. The
async command accepts 0 or 1 and is consumed by the next successfully queued
oneway Binder transaction; a dead-destination failure does not consume it.

Each ioctl checks the target UID. Root, system UID or CAP_SYS_NICE may address
another UID; other callers may address only their own UID. Kernel threads are
excluded. SET and async also call the scheduler LSM hook. Direct RT requests
additionally require root, system or CAP_SYS_NICE. Resource and backend control
errors are returned, rather than converted into success.

Scheduling and revocation
-------------------------

RT QoS contributes effective priority 98 (userspace RT priority 1), independently
of the task's policy, normal priority, nice and uclamp. Native rtmutex PI is
merged with that contribution. A task promoted solely by QoS gets round-robin
accounting and remains subject to the native global RT bandwidth limit.
Changing or removing QoS never restores a saved old sched_attr over a newer
user request. Direct RT is intentionally privileged even if a caller can open
the device.

VIP contributes to a native per-rq CFS selection list. Selection preference is
limited to 1 ms per 8 ms of task-clock time, with normal tick/hrtick accounting
granularity. Equal VIP priorities rotate, and CFS group throttling still applies.
After this preference budget is consumed, ordinary CFS selection applies,
including current nice, group weights and uclamp. The preference budget is not
a CPU-runtime cap on a task that legitimately wins ordinary CFS selection.

WALT groups 11..14 represent levels 10..7. Level 6 uses three process-owned
slots 15..17. Retained flags 6..7 select group 13. Existing explicit/cgroup WALT
ownership takes precedence. QoS removes only a group it attached itself.
An explicit group write also wins against a queued QoS worker that has not yet
attached its requested group. A QoS-owned nonzero-to-nonzero move changes both
group lists and the RCU task pointer under group/rq locks without subtracting
and adding the shared grouped busy-time history. WALT placement is secondary
to user/cgroup ownership, rather than forcibly replacing it.

The native RT/VIP policy is a target-compatible implementation, not a claim of
bit-for-bit equivalence to a proprietary 6.6 scheduler module. No private
structure offsets, copied function-pointer layouts or forged symbol CRCs are
used.

Dependencies and bounded work
-----------------------------

A dependency is a caller-owned link with task references. Normal zero-QoS
waiters are also recorded, so a static QoS setting made after a task has blocked
can propagate through the recorded dependency. Ordinary zero-QoS edge insertion
is constant-time after each task's first lazy metadata allocation; it does not
recompute the entire machine's graph.

The seven dependency classes are:

* Binder: sync transaction until reply/cleanup; explicitly armed async
  transaction until its delivered buffer is freed, including error/abort cleanup.
* Mutex: actual owner, stack waiter lifetime and handoff/cancellation refresh.
* rwsem: writer/predecessor dependency; mmap_lock also records bounded live
  readers and redirects to remaining readers as they leave.
* percpu rwsem: both the inner writer rw_sem and the outer read-count drain.
  Successful outer read/trylock records a live reader; the drain writer donates
  to a recorded reader and redirects when that reader leaves. Static DEFINE
  locks have statically allocated metadata. Async I/O release/acquire transfers
  task ownership without changing lock counters; the ownerless interval has no
  invented task dependency. Reader storage is bounded to 64 distinct tasks per
  lock, shared by nested holds, with diagnostics on allocation/coverage loss.
* Futex: optional Xiaomi WAIT/WAIT_BITSET owner hint, validated against the
  address key and a live same-mm TID; wait, timeout, signal, retry and restart
  revoke the link. Ordinary waits without an owner hint are unchanged. PI
  futexes retain their existing native rtmutex RT inheritance.
* FUSE: synchronous requesting task to the actual /dev/fuse reader, until
  reply, abort, cancellation or request release. Background requests do not
  invent a waiting donor.
* RCU: the existing RCU boost engine identifies the blocked reader. The most
  recent live QoS synchronize_rcu waiter donates until the boost mutex completes,
  matching the donor policy; stack registration and task references protect
  lifetimes. This does not boost unrelated callback workers.

Each task caches separate values L[0..3]: static roots at distance at most 0..3.
Only changed values enqueue the next downstream layer. A cycle cannot preserve
a contribution after its last static root is removed. A proposed update is
calculated in scratch state before committing.

Explicit limits are 8192 live graph metadata objects, 128 nonzero effective
QoS tasks, 64 incoming and 16 outgoing recorded edges per task, 256 touched
tasks per update, and 128 registered RCU wait scopes. Admission errors are
reported as ENOMEM/ENOSPC/E2BIG, with ratelimited dependency diagnostics.
Removing an admitted contribution does not require extra capacity. A rejected
edge retains a caller-owned marker and task references until link_del; SET
rejects a reachable incomplete topology instead of reporting complete success.
Callers must therefore call link_del even after an unsuccessful link_add.
Task exit revokes static/async state and admitted incoming/outgoing links.

Locking and ABI preservation
----------------------------

The raw graph lock protects graph values, links and admission scratch state.
No task pi_lock or rq lock is taken while holding it. A high-priority workqueue
applies committed scheduling contributions after dropping the graph lock;
control SET waits for its target's worker. Object hooks may hold their object
lock, then acquire the graph lock. Workers never acquire those object locks.
Task-reference drops occur outside the graph lock. The task state lives until
final task destruction; queued work and links hold their own task references.

Scheduler changes use pi_lock followed by rq lock, then adjust any existing
rtmutex dependency chain after releasing them. WALT changes use pi_lock,
related-thread-group lock, group locks in ascending ID order, then rq lock.
RCU uses its node/registry locks before the graph lock; the graph has no reverse
path to RCU's node or registry lock.

The task data occupies its existing 24-byte vendor reservation; the dynamic
VIP list node is outside task_struct. Runqueue data occupies its existing
96-u64 reservation. The original reservation declaration remains visible to
genksyms, following the kernel's reserved-field ABI mechanism. Existing task/rq
member offsets and overall sizes are retained. Internal Binder/FUSE metadata
is appended, preserving the offsets of their original members; these private
allocation sizes change and all built-in users must be rebuilt together.

Validation and deployment boundary
----------------------------------

The Venus host selftests extract the actual implementation functions:

* xr_qos_test.py: independent bounded-path oracle, cycles, late activation,
  multiple donors, cancellation/exit, rollback and concurrent graph updates.
* xr_qos_walt_test.py: concurrent real helper moves, explicit ownership,
  queued-worker arbitration and cleanup, with host accounting/locking stubs.
* xr_qos_ioctl_test.py: fixed ABI, target credentials, direct RT restriction,
  LSM denial, process aggregation, uaccess and backend errors.
* xr_qos_vip_test.py: budget, rotation, CFS throttling and list removal.
* qos_locks_test.py: real mutex/rwsem helpers, cancellation, reader handoff,
  bounded metadata and concurrent lifetimes.
* qos_percpu_rwsem_test.py: actual outer fast/try/slow read APIs, drain handoff,
  async I/O and writer ownership transfer, static metadata, allocation failure,
  rejected dependency cleanup and concurrent nested lifetimes.

These ASan/UBSan host checks and ARM64 object compilation are not a boot test,
Linux lockdep proof, a device scheduler fairness test or a thermal/power test.
Full configured-kernel linkage and real-device validation remain necessary.

A dedicated SELinux type and exact ioctl xperms are required in the matching
ROM. The current ROM has no matching xr_fbt file context or type. A real
platform_app caller lacks system group membership, so 0660 system/system alone
is insufficient. The prepared source fragments use 0666 with a dedicated type
limited to system_server, surfaceflinger and platform_app, and only these four
ioctls. They are not a compiled policy and are not automatically deployed by
enabling the kernel option. Do not grant generic device access or disable
Enforcing. Current libxqos caches its failed first open, so a later node appearing
is not proof that already-running clients recovered.
