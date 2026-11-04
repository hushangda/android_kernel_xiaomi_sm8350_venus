Venus：Android 17 优先接口补齐
==============================

日期：2026-11-04。适用源码：SM8350/Xiaomi Venus，Linux 5.4.302。
本轮在上一版 7 项稳定性修复和 hrtimer CPU 热插拔修复上，完成三个优先项目。
旧版排查记录保留在 ``Documentation/venus-android17-audit.rst``；其中的
“尚未实现”和旧产物校验值描述的是上一版，不代表本轮状态。

产物与验证边界
--------------

* Image.gz：``/home/abc/sm8350/out-venus-android17-priority-20261104/arch/arm64/boot/Image.gz``
* 字节数：20599706。
* SHA-256：``70512f5b766c3894f57586d2bef8ca18a464a9580c91100e4aa16902ef17db1f``。
* 源码基线：``1a61ec64eaf9958919a411ceef244d5a90333458`` 加当前工作区修改。
* release 继续保留 ``5.4.274-qgki-gcc3ec22f5061``，实际源码版本为 5.4.302。
* 用户要求只交付 Image.gz、暂不重启。本轮没有刷写分区、重启手机或打包 boot.img。
* 编译、主机测试和静态 ABI 核对通过；新内核尚未启动，尚未进行实际 DMA
  一致性、GPU/相机导入、并发进程清理、网络限速或完整 CTS/VTS 验证。

这次补齐已确认的内核接口缺口，不等于旧平台已满足 Android 17 的完整内核
要求，也不代表已经测得功耗或流畅度收益。上次实机观测来自旧内核：设备自身
的 libdmabufheap 不支持 ION 回退，system 分配报 ENOENT；非根 v2 cgroup
缺少 cgroup.kill；网络限速缺少下面三个配置。

DMA-BUF system heaps
--------------------

新增 DMA-heap 分配核心及标准 ``DMA_HEAP_IOCTL_ALLOC`` UAPI。Qualcomm
system ION 堆成功注册后，创建 ``/dev/dma_heap/system`` 和
``/dev/dma_heap/system-uncached``。前者使用 ION_FLAG_CACHED，后者使用
uncached 分配；只允许非安全的 MSM system heap，不能经这两个名称访问
secure/carveout 堆。

新接口复用本机既有 Qualcomm 页池、清零、DMA 映射、mmap、CPU cache sync
和释放操作。既有 /dev/ion 调用仍走原接口。新增 ion_alloc_fd_flags 使
DMA-BUF 文件权限遵循请求的 O_RDONLY/O_WRONLY/O_RDWR/O_CLOEXEC；旧 ION
调用继续使用原来的 O_RDWR，不改变原有函数签名或公开结构体。

核心以 Linux 5.15.y 的 DMA-heap 实现为基础，文件内容已对照固定提交
``606091b2f6dcbfbc9c1f948f9525cf0484288054``，并做以下本地适配：

* 在 u64 长度转 size_t、页对齐之前拒绝截断或回绕。
* 核对 ioctl magic 和足以返回 fd 的输出长度；保留较短/较长参数结构的
  零扩展与兼容处理。
* 先保留 fd 编号，分配 DMA-BUF；copy_to_user 成功后才 fd_install。
  分配失败或 copyout 失败释放编号和 DMA-BUF，避免不可见 fd 泄漏。
* 未完成 core class 初始化时拒绝注册。后端必须与 ION_MSM_HEAPS 一同
  built-in，按 subsys_initcall / subsys_initcall_sync 的既有顺序注册。

这是适配本机 ION 后端的实现，并非把较新 ACK 的 allocator 整体替换进来。
UAPI 与 ``android12-5.10-2026-10_r1`` 对应文件逐字节相同。

来源：

* https://github.com/gregkh/linux/blob/606091b2f6dcbfbc9c1f948f9525cf0484288054/drivers/dma-buf/dma-heap.c
* https://android.googlesource.com/kernel/common/+/refs/tags/android12-5.10-2026-10_r1/include/uapi/linux/dma-heap.h
* https://android.googlesource.com/platform/system/memory/libdmabufheap/+/refs/tags/android-17.0.0_r1/BufferAllocator.cpp

网络限速配置
------------

启用本树已有的 ``CONFIG_NET_CLS_MATCHALL=y``、``CONFIG_NET_ACT_POLICE=y``、
``CONFIG_NET_ACT_BPF=y``，提供 matchall 分类器、police 限速动作和 BPF
动作。这是 Android 17 netd 的 TestRateLimitingSupport 检查所需配置。
相关初始化/分类函数已确认进入最终 vmlinux。没有修改设备的 tc/qdisc 规则。

venus_defconfig 与 build_venus.sh 同时启用上述三项及两个 DMA-heap 选项，
脚本在 olddefconfig 后检查它们和 ION_MSM_HEAPS=y，配置缺失会直接构建失败。
相对上一版，最终配置恰好只增加这五个选项，没有顺带打开其他优化开关。

来源：
https://android.googlesource.com/platform/system/netd/+/refs/tags/android-17.0.0_r1/tests/kernel_test.cpp

cgroup.kill 与并发修复
---------------------

v2 非根 cgroup 新增只写 cgroup.kill，写入 1 对本组及后代进程发送 SIGKILL；
对 threaded cgroup 返回 EOPNOTSUPP。沿用上游的 cgroup mutex 排除迁移，
并用每组递增序号覆盖 fork 提交检查到 cgroup_post_fork 之间的竞态窗口。
仅在扫描期间置位一个标志，无法覆盖扫描先于 post_fork 完成的情况。

处理了组长已退出而其他线程仍存活的情况：扫描全部线程。对扫描到的线程
使用 PIDTYPE_PID 信号语义，避免通过留在旧组的退出中线程误杀已迁出的进程。
fork 序号发生变化时，仍对本次尚未运行的子进程进行组级 SIGKILL。

适配时没有给 struct cgroup / kernel_clone_args 添加字段，也没有改变现有
cgroup 回调函数签名。每个 v2 非根 cgroup 的序号单独分配，通过 XArray
按 ID 保存；创建失败完整回收，最终 css release 在 ID 可重用之前删除。
fork/kill 路径不进行内存分配。这样保留 vendor 模块依赖的结构体布局。
本树没有 CLONE_INTO_CGROUP，子进程继承父 cgroup；fork 窗口已有的
cgroup_threadgroup_rwsem 读锁保证该父任务不会被外部迁移。

回移植依据（按最终语义适配，不是机械 cherry-pick）：

* ``661ee6280931548f7b3b887ad26a157474ae5ac4``：引入接口与递归语义。
* ``b69bb476dee99d564d65d418e9a20acca6f32c3f``：fork/kill 序号竞态修复。
* ``da13e0e72a7329c95642c44b2d03d194ab071e04``：覆盖已退出组长的存活线程。
* ``5f4a3f743acc96987ae5c2e966947942c34ba56c``：不通过退出线程杀死已迁出的进程。

提交链接格式：``https://github.com/torvalds/linux/commit/<提交号>``。
另核对了 ``8e359920216689b3b79e0fe8961a77fe312a511f``，它修复
CLONE_INTO_CGROUP 的目标序号选择；本树没有该功能和错误分支，因此不适用。

完成的检查
----------

* 完整 ARM64 ``Image Image.gz dtbs`` 构建返回 0。编译器告警与上一版相同，
  没有新增编译器告警。gzip 解压内容与 Image 一致，内嵌 IKCONFIG 与 .config
  逐字节一致。
* 上一版 hrtimer、RT、EROFS、F2FS、ZRAM 修复涉及的五个生产源文件 SHA-256
  均保持不变；原修复已包含在本产物中。
* Module.symvers 中原有 14577 个导出符号无移除、无 CRC 变化；新增一个
  ion_alloc_fd_flags 导出。使用同一套构建参数加调试信息检查 cgroup、
  cgroup_root、task_struct 的完整成员布局，三者与基线一致。这些检查覆盖
  可见的 ABI 变化，不代替 vendor 模块实际加载和功能验证。
* android17_priority_test.py 从生产源码提取实际 C 函数，以 ASan/UBSan
  运行有界模型：cached/uncached 路由、访问模式、旧 ION 行为、非法参数、
  分配/导出/fd/用户拷贝失败回收、扩展 ioctl；递归 kill、fork 窗口、退出
  组长、已迁出进程、内核线程豁免、错误返回、ID 重用和 kill 无分配。
* 三个变异对照分别恢复“仅遍历组长”“遗漏 fork 序号检查”“向退出线程
  发送组级信号”，测试均检测到对应缺陷。这些是确定性主机模型，不是实际
  内核调度、锁顺序或内存序压测。
* NDK 29 分别针对 ARM64、ARM32 编译 UAPI 静态断言：结构 24 字节，fd、
  fd_flags、heap_flags 偏移 8/12/16，ioctl 编号 0xc0184800 一致。
* millet_freezer_test.py 的三种配置与旧实现反例、millet_delivery_test.py
  的投递/失败路径及反例均通过，覆盖与本次 cgroup 改动相邻的 ROM 接口。
* git diff --check 通过。增量补丁 checkpatch：0 errors、3 warnings，
  另有 4 条对齐提示；警告为两个 Kconfig 帮助段长度提示和新增文件的
  MAINTAINERS 提示，没有功能检查失败。

复现构建与主机测试（在仓库根目录执行）：

::

    env -u TWRP_DEVICE_DIR JOBS=4 \
      OUT_DIR=/home/abc/sm8350/out-venus-android17-priority-20261104 \
      ./build_venus.sh
    python3 tools/testing/selftests/venus/android17_priority_test.py
    python3 tools/testing/selftests/venus/millet_freezer_test.py
    python3 tools/testing/selftests/venus/millet_delivery_test.py

之后的设备验收
--------------

待用户实际启动此 Image.gz 后，按以下顺序验收；本轮没有执行这些步骤：

1. 核对运行内核内嵌配置和 dmesg，确认两个 heap 成功注册，非根 v2 cgroup
   存在 cgroup.kill。root 上没有该文件属于正常设计。
2. 分别以 ARM64/ARM32 调用本 ROM libdmabufheap，复核之前的 ENOENT；
   验证两种 heap 的分配、清零、mmap、CPU sync、释放及重复分配。复用上次
   android17_probe.c 可先确认库分配，后续还需实际 GPU/相机 DMA 导入验证。
   ROM 已有 ueventd 和 file_contexts 配置不等于各 HAL/app 域均获得权限，
   应同时检查实际节点标签和 SELinux 拒绝记录。
3. 在专为测试创建的空 cgroup 子树中，仅放入测试自身创建的子进程，验证
   递归清理、并发 fork、组长退出及迁移边界；不对系统或应用现有组做 kill。
4. 在隔离网络环境运行 netd 的限速检查与 tc 功能测试，再检查本机实际业务。
5. 最后验证相机、视频、应用切换、冻结/解冻、充电、休眠唤醒与跨 6 点待机。
   无法用本次编译和模型测试宣布原来的夜间重启已在实机消失。

证据目录：
``/home/abc/BUG/reboot_20261104_0610/priority-android17-20261104/``。
包含基线快照、上游原文、增量补丁、配置变化、布局对比、Module.symvers
对比、测试日志与 build-manifest.json。完整构建日志位于产物目录 build.log。
