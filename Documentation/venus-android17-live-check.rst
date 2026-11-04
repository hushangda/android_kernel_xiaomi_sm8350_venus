Venus：Android 17 新内核实机验收
==============================

日期：2026-11-04。用户自行刷入新内核后，通过 ADB 验证当前 Venus。
本次操作未刷写、未重启、未修改 SELinux 策略，也未修改实际联网接口的规则。
构建时记录见 ``Documentation/venus-android17-priority.rst``；本文补充其
当时尚未进行的设备测试。

版本确认
--------

* Android 17 / API 37，SELinux Enforcing，sys.boot_completed=1。
* 当前槽位 a，boot header v3。只读提取 boot_a 中 kernel payload，长度
  20599706，SHA-256 为
  ``70512f5b766c3894f57586d2bef8ca18a464a9580c91100e4aa16902ef17db1f``，
  与交付的 Image.gz 逐字节一致；解压后的 Image 也一致。
* /proc/config.gz 解压后与本次构建 .config 逐字节一致，SHA-256 为
  ``0d1031c51d5ba206584a6cb98714b513aa7e8f79633c8f13154eb28defcb1b50``。
* uname 保留既有 5.4.274 显示字符串，因此版本判定使用 payload、配置和
  新接口实测，不仅看 uname。
* 初次采集时间 11:11:39，uptime=181.44 秒。11:18:46 复核时 uptime=608.50
  秒，两次 boot_id 相同；测试期间没有重启。

DMA-BUF heaps：通过
------------------

system / system-uncached 设备节点已出现，标签均为
``u:object_r:dmabuf_system_heap_device:s0``，system:system、0444。

分别以 ARM64/ARM32、shell/root 运行之前的 android17_probe.c：
设备自身 libdmabufheap 的 CheckIonSupport 仍返回 0，但 system 分配在四种
组合下均成功，获得有效 fd。上一版同一调用返回 ENOENT，确认该缺口已消除。

新增 ``tools/testing/selftests/venus/dma_heap_device_test.c``，使用 NDK 29
分别编译两种 ABI，运行同样四种身份组合。两个 heap 全部通过：

* 每个 heap/组合 34 次分配，共 272 次成功分配；覆盖页对齐、小尺寸、
  64 KiB、O_RDONLY/O_WRONLY/O_RDWR 与 CLOEXEC。
* mmap、初始清零、写入/读回、DMA_BUF_IOCTL_SYNC START/END、释放与重分配。
* 只读 fd 的共享可写映射被拒绝，返回 EACCES。
* 零长度、UINT64_MAX、非法 heap flags/fd flags 和非零输入 fd 被拒绝。
* 每个 heap/组合 32 次可读但不可写参数页造成 copyout EFAULT，共 256 次；
  /proc/self/fd 计数无增长，没有泄漏安装后的 fd。

这些测试验证 CPU 映射和同步接口；未实际让 GPU、相机或视频硬件导入测试
buffer，也不能以 shell/root 的权限代表所有 HAL 和应用的 SELinux 域。

cgroup.kill：通过
-----------------

非根 v2 cgroup 出现 cgroup.kill。新增
``tools/testing/selftests/venus/cgroup_kill_device_test.c``，只操作新建的
``/sys/fs/cgroup/venus-a17-check-23823`` 测试子树。测试管理进程始终在子树外，
只迁移和清理它自己创建的子进程，子进程数量和存活时间均有上限。

通过的用例：

* 写 0 返回 ERANGE。
* 对本组及嵌套子组递归 SIGKILL，全部被回收，populated 归零。
* 冻结测试组后再 kill，两个子进程仍被正确清理。
* 组长 pthread_exit 后，存活线程迁到测试兄弟组：清理原组不会杀死已迁出
  的线程；清理目标组能结束该进程。用管道往返确认迁移后确实存活。
* 24 轮 fork/kill 并发，每轮最多派生 8 个后代，全部清理，无逃逸任务。

这是有界实机回归，未穷尽所有调度时序、迁移/退出的短暂窗口或高压并发。
测试结束后子树已删除，未遗留测试进程。

网络分类与动作：通过
--------------------

使用 unshare -n 创建临时网络命名空间，仅在其中的 lo 安装 clsact、
matchall + police（1 Mbit/s、16 KiB burst）和 matchall + BPF action。
BPF action 使用单条返回 TC_ACT_OK 的 classic BPF 程序。

向隔离命名空间自己的 127.0.0.1 发送 3 次 ping，全部返回；police 与 BPF
动作计数各增加到 6 个包，确认实际经过分类及动作执行路径。随后删除 qdisc，
进程退出销毁临时命名空间。手机实际 Wi-Fi/蜂窝接口和规则没有被修改。

本测试没有测量限速精度，也没有运行完整 Android netd 测试或其生产 eBPF
程序；已确认新增的三个内核组件能够注册并处理流量。

启动日志的遗留问题与边界
------------------------

* 测试前后 crash buffer 相同。当前 boot 的 init 子进程在 11:08:50 和
  11:09:07 各发生一次 SIGABRT，PID 分别为 2307 和 6072，均不是 PID 1。
  刷入前保存的日志也有 06:10:18 / 06:10:34 的同类事件。因此没有证据将它们
  归因于本批补丁，但其具体 abort 原因尚未定位，不能视作已修复。
* 初次读取 kernel tainted 已为 516，测试后仍为 516。按本树定义含 bit 2
  （CPU_OUT_OF_SPEC）及 bit 9（WARN）。初次 dmesg 最早记录已是开机后
  37.52 秒，早期内容被环形缓冲覆盖，未取得对应告警栈，不能宣布启动全程
  没有内核告警。相同位不变也不能独立排除再次发生同类告警。
* 本次保存的测试前后可见 dmesg 均未发现 WARNING/BUG/Oops/panic 调用栈，
  pstore 为空，测试期间无新增 crash buffer 事件；现有 tombstone 时间均
  早于本次启动。
* 原夜间 6 点重启是否消失仍需跨该时段待机观察；本轮短测不能替代夜间待机、
  充电/唤醒、GPU/相机、功耗或完整 CTS/VTS 验收。

可复现性与清理
--------------

上述两个设备测试源码保存在 tools/testing/selftests/venus，使用
NDK 29.0.14206865 的 aarch64-linux-android30-clang 编译；DMA 测试还使用
armv7a-linux-androideabi30-clang。参数为 -std=gnu11 -O2 -Wall -Wextra -Werror，
cgroup 测试加 -pthread。它们是手动设备测试，不加入主机测试自动执行入口。

本轮临时推送的五个二进制及其 /data/local/tmp 子目录均已删除，测试 cgroup
已删除，未留下后台测试或监控进程。内核生产代码和已刷入 Image.gz 没有变化。

完整证据：
``/home/abc/BUG/reboot_20261104_0610/priority-live-20261104-111139/``。
其中包含 boot-kernel-identity.json、两种 ABI/身份的输出、网络动作计数、
cgroup 测试输出、前后日志、旧版 init 异常对照及 live-validation.json。
