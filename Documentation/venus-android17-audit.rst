Venus：7 项稳定性修复与 Android 17 内核接口排查
================================================

审计日期：2026-11-04。对象为当前 SM8350/Xiaomi Venus 源码和通过 ADB
连接的 Android 17 / API 37 手机。本文记录实测、源码核对及待移植项目，
不表示已经通过完整 CTS/VTS 或新内核的启动、待机、功耗测试。

交付范围
--------

本轮 Image.gz 包含原有 hrtimer CPU 热插拔修复以及下面 7 项稳定性修复。
Android 17 新接口本轮完成排查，尚未加入该二进制。
遵照用户要求保留文件，未刷写、未重启，未生成新的 boot.img。
设备上的接口探测运行于当前旧内核，不是新 Image.gz 的启动验证。

产物：

* ``out-venus-stable7-20261104/arch/arm64/boot/Image.gz``
* 字节数：20590673
* SHA-256：``7b21da4aa0b383e49a01449646915a3effb8550cffb61f7f954b2a7cf153246a``
* 源码基线：``1a61ec64eaf9958919a411ceef244d5a90333458`` 加现有工作区修改。
* 源码版本为 5.4.302；构建脚本保留的 release 为
  ``5.4.274-qgki-gcc3ec22f5061``。不能根据 uname 的补丁号判断源码版本。

构建采用 ``build_venus.sh``，``JOBS=4``，输出到独立目录；目标包含
``Image Image.gz dtbs``。生成配置与此前 hrtimer 修复构建一致，
Image.gz 内嵌 IKCONFIG 与实际构建配置逐字节一致，gzip 完整性校验通过。
SukiSU、ROM_COMPAT/MIMD 等已有选项继承此前构建；已有 ROM 配套要求见
``Documentation/venus-rom-kernel-compat.rst``。

7 项修复及适配
--------------

1. RT 任务迁移竞争：在 ``double_lock_balance()`` 释放并重取运行队列锁后，
   重新确认目标仍是 pushable 队列首项，避免分别检查 CPU/on_rq 等状态时
   跨越并发唤醒。将 ``pick_next_pushable_task()`` 原样前移以便调用；
   保留本树的 ``cpus_ptr`` 和 WALT 语义。
   上游：``690e47d1403e90b7f2366f03b52ed3304194c793``；
   stable 参考：``35b55cd665fbfd72805318f7017626eefd13dca5``。

2. RT IPI 遍历：更新遍历游标后跳过当前 CPU，避免向自己投递 RT push IPI。
   上游：``94894c9c477e53bcea052e075c53f89df3d2a33e``；
   stable 参考：``16ca9f3117e9a294646c897daf08a5ab546c711b``。

3. EROFS 解压上下文：识别可抢占 RCU 读侧临界区，并在没有 PREEMPT_COUNT
   时保守采用工作线程；防止在不能睡眠的上下文直接解压。
   上游：``c99fab6e80b76422741d34aafc2f930a482afbdd``；
   stable 参考：``be7929770a783b983583a5f9aeebd3facd688ed2``。

4. EROFS 生命周期：先更新 ``readahead_sync_decompress``，再提交解压任务，
   避免任务完成、卸载释放 sbi 后仍写入 sbi。保留本树 per-CPU/workqueue 分发。
   上游：``1aee05e814d292064bf5fa15733741040cdc48ba``；
   stable 参考：``5627e57f1da68ff0146896eb3d2eed25e62adc44``。

5. ZRAM writeback：在持有 init_lock 且检查初始化状态后读取 disksize，
   避免与 reset/重新初始化竞争而使用过期的页数扫描表。
   上游：``894913e2d35c46ff19a77530907771ae57862b96``；
   stable 参考：``4d82d60178b02797dbe529e01ac211b26670c49f``。

6. F2FS NAT/checkpoint：读取 inode NAT 更新状态时，按 node_write、
   nat_tree_lock 的顺序加读锁，并逆序释放。当前 checkpoint 持有
   node_write 写锁直到提交完成；核对了两个调用位置，没有递归取该锁。
   上游：``238e14eb7226f883b72caccd2d37bf5707df066b``；
   stable 参考：``eb65e8c7bd0a78a470cbdd3ff96232ba4a2ace89``。

7. F2FS fsync/truncate：重试前锁定最后一个 node page 后，验证其仍属于
   node mapping；失效时解锁并释放引用，结束重试。本树最终出口统一返回
   EIO，沿用该错误约定，避免引入另一项行为变更。
   上游：``ce366bfa821ec81dd45bde547ee31e659306cc61``；
   stable 参考：``64a47dc8286ff98d8c231ae035bdfe16be1b93bf``。

stable 提交可使用 ``https://github.com/gregkh/linux/commit/<提交号>`` 查看。
原始补丁保存在证据目录的相邻 ``upstream_audit_20261104/references`` 中。

验证结果
--------

* ``stable7_test.py``：提取实际 C 函数，在受控的锁/CPU/I/O 模型中验证
  7 条修复路径；固定的修复前提交作为对照，7 项旧缺陷均被检测到。
  使用 ASan/UBSan，EROFS 旧版生命周期用例实际触发 heap-use-after-free。
  这些是主机回归模型，不等价于内核并发压测或 F2FS 断电恢复验证。
* ``erofs_pcpu_test.py``：workqueue、per-CPU、per-CPU 高优先级三种模式通过。
* ``hrtimer_hotplug_test.py``：32 个 late enqueue 场景及 online、迁移、
  IPI 合并策略通过；此前 hrtimer 补丁逐字节保留。
* ``zram_group_io_test.py`` 和 ``extm_quota_test.py``：实际槽位 I/O、
  writeback 生命周期及配额回归通过。
* 完整 ARM64 构建、内嵌配置一致性、gzip 校验和 ``git diff --check`` 通过。
  checkpatch 为 0 errors、5 warnings；五个警告全部来自原样前移的
  ``pick_next_pushable_task()`` 内已有 BUG_ON，没有新增断言。
* 新 Image.gz 尚未在手机启动。夜间重启是否消失、真实 RT 负载、
  fsync/截断并发、待机功耗与充电/休眠唤醒仍需之后的设备验证。

可复现的主机测试命令：

::

    python3 tools/testing/selftests/venus/stable7_test.py
    python3 tools/testing/selftests/venus/erofs_pcpu_test.py
    python3 tools/testing/selftests/venus/hrtimer_hotplug_test.py
    env PATH=/usr/lib/llvm-14/bin:$PATH python3 tools/testing/selftests/venus/zram_group_io_test.py
    env PATH=/usr/lib/llvm-14/bin:$PATH python3 tools/testing/selftests/venus/extm_quota_test.py

Android 17 审计方法和平台边界
----------------------------

使用 Android 17 正式源码标签 ``android-17.0.0_r1`` 核对 ART、Binder、
lmkd、libprocessgroup、bionic、netd/Connectivity、libdmabufheap 和
共享内存策略，结合当前内核源码、设备 ``/proc/config.gz``、实际节点、
ROM 的 cgroups/task_profiles 配置以及 32/64 位 ABI 探测。

设备 fingerprint：
``Xiaomi/venus/venus:17/CP2A.260605.016/OS4.0.5.0.XPACNXM:user/release-keys``。
system SDK 为 37，vendor/board/first API 为 30。当前实际页大小是 4096。
``ro.product.cpu.pagesize.max`` 一类能力属性不能当作实际 16 KB 页验证。

官方 Android 17 兼容矩阵不包含 5.4；5.10 也仅列于 QPR1 之前。
这说明本设备依赖定制回移植和 ROM 适配，补齐若干接口不等于获得官方
Android 17/GKI 支持。版本矩阵依据：
https://source.android.com/docs/core/architecture/kernel/android-common

探测程序 ``tools/testing/selftests/venus/android17_probe.c`` 使用 NDK 29
分别编译 arm64 和 arm32，分别以 shell/root 运行。它仅操作自身映射、
文件描述符和未附着的 BPF 程序，未冻结/终止其他进程，未附着 BPF 到
网络或内核钩子，未修改 sysctl、设备配置或 SELinux enforcing 状态。
二进制经临时目录运行后清理。shell/root 结果不能代替 app seccomp 或
真实服务 SELinux 域中的验证；程序退出 0 表示完成探测，不代表所有能力存在。

明确的缺口和建议顺序
--------------------

P1：DMA-BUF heaps，已复现实际分配失败
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

内核只有 ION，缺少 DMA-BUF heap 核心接口，设备没有 ``/dev/dma_heap``。
调用设备自身的 ``libdmabufheap.so``，``CheckIonSupport()`` 返回 0；
``DmabufHeapAllocSystem2(..., true, 4096, 0)`` 在 32/64 位、shell/root
四种组合下均返回 -1/ENOENT。这里已有真实用户空间调用失败证据。

Android 17 的 BufferAllocator 已移除 ION 分配回退，仅为旧二进制保留
部分符号。当前 ROM 已包含 system/system-uncached 的 ueventd 节点规则
及 SELinux file_contexts，因此首要缺口在内核分配器；标签存在仍不能
证明所有目标服务的 SELinux allow 规则都足够。

建议引入上游/ACK DMA-BUF heap 框架，先补 system 和符合平台缓存语义的
system-uncached，保留老 vendor 依赖的 ION。验证需涵盖 fd 生命周期、
mmap、CPU/device cache 同步、DMA attachment 和实际媒体/图形服务。
不能仅创建同名节点或把 heap ioctl 重定向到不兼容的 ION ioctl。
这项失败不能推导成“所有图形/摄像头都故障”，旧 vendor 仍可能直接使用 ION。

依据：
https://android.googlesource.com/platform/system/memory/libdmabufheap/+/refs/tags/android-17.0.0_r1/BufferAllocator.cpp

P1：cgroup.kill，应用进程组清理快速路径缺失
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

实际非根 cgroup ``/sys/fs/cgroup/system/uid_0`` 有 freeze/procs，没有 kill。
内核源码也没有该用户接口；不能通过检查根 cgroup 是否有该文件来判断。
ROM task_profiles 已声明 CgroupKill，Android 17 lmkd/reaper 与
libprocessgroup 有对应调用和回退。当前可回退到逐个读取 cgroup.procs
及 pidfd 清理，不是 lmkd 完全失效。

建议回移植上游 cgroup.kill 的完整实现，包含 fork/迁移并发与线程组
生命周期相关后续修复，再用独立测试进程树验证。上游引入提交为
``661ee6280931548f7b3b887ad26a157474ae5ac4``。不要仅加一个遍历并发送
SIGKILL 的文件处理器来冒充该接口的并发保证。

依据：
https://android.googlesource.com/platform/system/memory/lmkd/+/refs/tags/android-17.0.0_r1/reaper.cpp
https://android.googlesource.com/platform/system/core/+/refs/tags/android-17.0.0_r1/libprocessgroup/processgroup.cpp

P1：网络限速的配置缺口
~~~~~~~~~~~~~~~~~~~~~~

设备配置和新构建配置均未启用 ``CONFIG_NET_CLS_MATCHALL``、
``CONFIG_NET_ACT_POLICE``、``CONFIG_NET_ACT_BPF``。Android 17 netd 的
``TestRateLimitingSupport`` 明确检查这些选项；现有 BPF_JIT 和
BPF_JIT_ALWAYS_ON 已启用，SCHED_CLS 程序可加载不等于 police/act_bpf 已有。
本树存在这些功能源码，可先作为小范围配置修复构建，再在隔离网络环境验证。
本轮没有执行 netd 测试，也没有修改手机 tc/qdisc 或网络规则。

依据：
https://android.googlesource.com/platform/system/netd/+/refs/tags/android-17.0.0_r1/tests/kernel_test.cpp

P2：memory.reclaim 与 swappiness，需同时适配层级
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

当前 memory/cpu/cpuset/blkio 使用 cgroup v1，v2 主要用于 freezer。
``memory.reclaim`` 内核接口缺失，当前应用 v2 层级也没有 memory.current。
Android 17 libprocessgroup 的 CompactMemcgAction 读取 memory.current，
再按需写入带 ``swappiness=0/200`` 的 memory.reclaim。基础回收接口与
swappiness 扩展均需核对，后者的上游提交为
``68cd9050d871e4db5433420b5ceb32f5512d18bc``。

ROM 当前仍配置旧 Compact 路径，且有 MCTRL/XSWAPD/MIMD 的 v1 后端。
因此不能只回移植一个 memcontrol.c 函数，就声称新版应用回收可用；
必须设计 ROM task_profiles 和 memcg 层级配套，保留原有 vendor 路径。
ROM 声明的 memory.events/high/swap.max 在当前 v1 挂载也不存在，但本次
未发现对应操作被调用的证据，不能据此声称已发生服务故障。

依据：
https://android.googlesource.com/platform/system/core/+/refs/tags/android-17.0.0_r1/libprocessgroup/task_profiles.cpp

P2：ART GC，基础能力已齐，开关和 MOVE 应分别处理
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

UFFD 特征位为 0x7fe，SIGBUS、shmem minor、缺页注册/COPY、minor
CONTINUE 和 Android 的 MMAP_TRYLOCK 探测均通过；共享映射
MREMAP_DONTUNMAP 在两种 ABI 上也通过。因此不能将基础 UFFD 或
共享 mremap 列为缺失。

当前 ``ro.dalvik.vm.enable_uffd_gc=false``，查询到的
runtime_native_boot enable_uffd_gc_2 为 null。这说明所查配置没有打开
该路径；尚未验证 boot 缓存或每个 ART 进程最终选择的 collector。
可先核实实际 ART collector，再考虑受控启用已有 COPY 路径并测量
GC pause、PSS/RSS、启动耗时和待机，而不是先增加大批 MM 补丁。

``UFFD_FEATURE_MOVE`` 缺失。AOSP 17 ART 使用该能力时有 major >= 6
的 CHECK，6.1/6.6 还探测额外安全标志与修复。只把 MOVE 回移植到 5.4
并暴露 feature 位，可能让 ART 进入断言失败路径；需要 MM 正确性修复、
ART 版本判定和 app seccomp 配合。不要伪造 uname 来绕过此类判定。

依据：
https://android.googlesource.com/platform/art/+/refs/tags/android-17.0.0_r1/runtime/gc/collector/mark_compact.cc

P3：Android 17 memfd_class，需要 SELinux/ROM 配套
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

基础 memfd、MFD_EXEC、MFD_NOEXEC_SEAL、F_SEAL_FUTURE_WRITE 已通过实测。
缺少的是 SELinux ``memfd_class`` policy capability，而不是 memfd 本身。
Android 17 的 libcutils 默认选择还受 vendor API、应用 target SDK 和
policycap 约束；本机 vendor API 30，继续使用 ashmem 是兼容路径。

官方对 vendor API >= 202604 且 android16-6.12 或更新内核的首发设备
提出该要求，本机不属于该条件组合。若移植，应同时核对内核安全类、
memfd 标签继承、策略编译与共享权限，保留旧 vendor 使用 ashmem 的能力。

依据：
https://source.android.com/docs/security/features/selinux/compatibility#shared-memory-changes-for-android-17
https://android.googlesource.com/platform/system/core/+/refs/tags/android-17.0.0_r1/libcutils/ashmem-dev.cpp

大型优化项目：Proxy Execution
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

这是真正面向 Android 新内核的调度优化：android17-6.18 默认启用，
支持内核 mutex/rwsem 的执行时间捐赠，降低优先级反转导致的长尾延迟。
官方说明 android15-6.6 及以下未支持；本树没有相应配置和运行节点。
它涉及 rq->curr/donor、阻塞任务、迁移、锁所有者和 WALT/vendor 扩展，
应当作为单独调度移植项目评估，不能按几个独立性能补丁直接合并。
官方当前仍把 Binder/futex 扩展列为后续工作，不能声称它已自动解决
所有 Binder 优先级继承问题。

依据：
https://source.android.com/docs/core/architecture/kernel/proxy-exec

版本门槛与次要候选
------------------

Android 17 AOSP NetBpfLoad 对 25Q4 及以上要求 >=5.10，对 26Q4 及以上
要求 >=5.15；这是版本检查，独立于具体 BPF 功能是否已回移植。
当前 ROM 的 init 版本标记为 26Q2/API37.0，APEX loader 包含这些诊断
字符串，但当前 bpf.progs_loaded、hyperos_progs_loaded 已为 1，存在
实际 pin 的网络/GPU BPF 对象。因此当前 ROM 的适配方式需要保留；
字符串不能证明未修改的门槛被执行过，也不能声称当前 BPF 全部失效。
本次没有执行 loader，也没有对其二进制打补丁。更换为原版 APEX 的
兼容性需要单独确认，伪造较新的 uname 不能补足行为与 ABI 保证。

依据：
https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-17.0.0_r1/bpf/loader/NetBpfLoad.cpp

其他发现：

* ``mseal`` 在 arm64 返回 ENOSYS。Android 17 bionic 列出了 LP64 包装，
  可以作为内存映射加固候选；本次没有确定当前应用的失败调用点。
  32 位返回 ENOSYS 不应算作同等必需接口缺陷。
* ``CONFIG_RSEQ`` 和 ``CONFIG_CGROUP_PIDS`` 未启用。尚无本 ROM 的
  Android 17 调用需求证据，不作为默认的优先移植项目。
* DMA-BUF 的 IMPORT/EXPORT_SYNC_FILE UAPI 在本树缺失；当前审计没有
  测到对应用户空间调用失败，可在 DMA-BUF heaps 后续工作中核对需求。
* MGLRU 已编译但运行值 0x0000，不是尚未回移植。构建脚本明确保留
  vendor reclaim 兼容修复前默认关闭的策略，本轮没有启用。
* F2FS 压缩代码已存在，但实际 /data 没有 compression 文件系统特性；
  不把其压缩路径的新补丁当成这台手机当前的 Android 17 适配收益。

已经具备的接口与验证深度
------------------------

* Binder：protocol 8、extended_error、oneway spam 检测 ioctl 通过；
  root frozen-info 查询通过。binderfs 的 freeze_notification、
  extended_error、oneway_spam_detection 均为 1；冻结回调完整生命周期
  没有实际执行。shell 的 frozen-info EACCES 是权限差异。
* UFFD、共享 mremap 和 memfd：见上文，两种 ABI 的实际数据路径通过。
* pidfd_open 和 signal 0 对自身调用通过；process_madvise 对自身 COLD
  在 root 成功，shell 为 EPERM。process_mrelease 无效 fd 返回 EBADF，
  只证明已分派到系统调用，不等价于验证死亡进程内存回收。
* openat2、close_range、epoll_pwait2 基础调用通过；futex_waitv 的值不匹配
  返回 EAGAIN，证明接口识别了该场景，未执行完整唤醒竞态测试。
* PR_SET_VMA_ANON_NAME、MADV_POPULATE_READ/WRITE 通过；membarrier 查询
  返回 127，包含 private expedited/sync-core 能力。
* BPF：root 成功加载最小有效 BTF，创建 array/hash/percpu-array/ringbuf/
  SK_STORAGE map，加载未附着的 SOCKET_FILTER/SCHED_CLS/CGROUP_SKB 程序。
  shell 被拒绝是权限限制；测试不涵盖所有 verifier 特性、helpers 或
  真实网络程序。系统已有网络/GPU pin 对象与 loader 成功属性。
* PSI：编译启用，实际 memory pressure 数据可读；不是缺失项。
* UCLAMP：配置启用，top-app/background 子 cgroup 的 min/max 和
  latency_sensitive 节点存在。根节点不存在由 CFTYPE_NOT_ON_ROOT
  决定，不能据此错误地判断功能缺失。
* cgroup v2 freezer：实际应用层级有 freeze/events/procs；不表示 v2
  memory controller 已用于应用。
* IncFS：corefs、v2、zstd 均报告 supported；未运行完整增量安装测试。
* fscrypt/fsverity：配置存在，/data 特性包含 encryption/verity；
  本轮没有创建加密/verity 测试文件。EROFS 为当前系统分区格式。
* ARM64 4 KB 页及 ARM32 compat 已实际运行。页大小改变涉及 boot/ELF/
  vendor 驱动适配，不是把某个配置改成 16 KB 即完成 Android 17 优化。

证据索引和后续验证
------------------

完整证据目录：
``/home/abc/BUG/reboot_20261104_0610/stable7-android17-20261104/``。

* ``build-manifest.json``：产物、源文件与配置 SHA-256，构建/检查结果。
* ``stable7.patch``、``stable7-test.log``、``checkpatch.log``：修复与回归证据。
* ``device-probes.json`` 和四份 ``android17-probe-*.txt``：最终设备 ABI 结果。
* ``device-kernel-config.txt``：设备实际 /proc/config.gz 解压内容。
* ``device-features-verified.txt``、``device-cgroup-verified.txt``、
  ``device-task-profile-child.txt``：修正 root 命令整体引用、非根层级检查后
  的最终记录。较早 device-environment.json 中的权限失败或根路径缺失
  不能代替这些最终记录。
* ``device-dmaheap-policy-hints.txt``、``device-netbpfload-integration.txt``：
  ROM 节点规则、标签及 loader 集成信息。
* ``aosp/sources.json``：已下载的正式标签源码地址与本地快照索引。

优先顺序建议：DMA-BUF heaps 和网络配置缺口，然后完整 cgroup.kill；
接着评估 memory.reclaim 与 ROM 层级配套，以及已有 UFFD GC 的实际配置。
memfd_class、mseal、UFFD MOVE 按真实消费者需求单独推进；Proxy Execution
作为长期调度改造。每项收益都需要实机测量，本文没有宣称提升百分比，
也没有把接口探测成功当作完整 Android 17 兼容认证。
