# CapMix 内核总览：设计理念、接口定义、能力边界与当前状态

**记录日期：2026-10-13；覆盖代码阶段：phase0–9、NUMA、最小 EPT、M2–M8。**

本文面向使用者、内核开发者和评审者，回答四个问题：CapMix 想构建什么，
现在真正能做什么，各接口代表什么，以及当前实现的下限与上限在哪里。
参数以本仓库源码为准；阶段文档用于追踪实现细节和验收过程。

当前版本宏仍是 `0.8.0-phase8`，属于历史命名，不代表 phase9 或 M8 尚未完成。
本文不修改内核版本号，也不把里程碑编号当作稳定 ABI 版本。

## 阅读导航

- [1. 定位与“上限/下限”的含义](#1-定位与上限下限的含义)
- [2. 三大家族融合的设计理念](#2-三大家族融合的设计理念)
- [3. 系统结构与实际执行路径](#3-系统结构与实际执行路径)
- [4. 当前能运行的最小系统与硬件条件](#4-当前能运行的最小系统与硬件条件)
- [5. 当前代码的容量上限](#5-当前代码的容量上限)
- [6. 对象、权限、生命周期与错误码](#6-对象权限生命周期与错误码)
- [7. 子系统接口定义与使用语义](#7-子系统接口定义与使用语义)
- [8. 用户态 syscall ABI 和开放范围](#8-用户态-syscall-abi-和开放范围)
- [9. 阶段与产品里程碑状态](#9-阶段与产品里程碑状态)
- [10. 安全、隔离和可靠性的实际边界](#10-安全隔离和可靠性的实际边界)
- [11. 构建、运行与验收](#11-构建运行与验收)
- [12. 后续发展的上限与优先事项](#12-后续发展的上限与优先事项)
- [13. 源码和文档索引](#13-源码和文档索引)

## 1. 定位与“上限/下限”的含义

CapMix 是一个用 C11 实现的 x86_64 capability 混合内核，提供两种运行方式：

| 运行方式 | 含义 | 能验证什么 | 不能据此证明什么 |
|---|---|---|---|
| hosted | 子系统在宿主进程中运行，`make test` | 对象、内存记账、IPC、文件系统、协议编解码等软件逻辑 | 真实 CPL3、CR3 隔离、硬件 DMA、VMX 指令、实际 PS/2 输入 |
| freestanding | Limine 引导的独立内核，生成 ISO 在 QEMU/兼容虚拟机中运行 | ring3、KPTI、抢占、中断、真实设备模拟器收发、VMX/EPT、产品启动 | 全部真实硬件兼容性、生产安全认证、无限规模运行 |

**当前下限**指已经连通并验收的基本使用闭环：引导 → 挂根 → PID1 →
文本 shell → 加载 CapMix ABI 静态 ELF → 用户态执行/退出 → 同步/关机。
它不是声称已经测出的最低 RAM、最低 CPU 频率或最便宜硬件规格。

**当前上限**有三层，必须分开：

1. **编译期容量上限**：例如最多 8 CPU、32 个 proc 槽位、512 项/CPU 静态 trace 环。
2. **运行路径上限**：多个 ELF 可并发，首版固定 CPU 亲和、不迁移；
   32 个 proc 槽包括 PID1 和退出记录，shell 最多 8 后台 + 1 前台。
3. **设计可扩展方向**：以后可扩展多进程、驱动 runtime、网络服务和客户机；
   这些属于路线，不是当前能力或可承诺的性能数字。

当前适合做可运行内核研究、机制验证、受控虚拟机演示和硬件路径开发。
它还不是能替代 macOS/Windows/Linux 的通用桌面或服务器系统。
没有 POSIX/Linux、Win32/NT、Mach/BSD 二进制兼容层，不能直接运行三大家族应用。

## 2. 三大家族融合的设计理念

### 2.1 融合机制，而非把三个内核拼在一起

项目选择的是三大家族的工程思路，并加入 capability/L4 等来源。
没有链接 XNU、NT 或 Linux 内核，没有实现它们全部 ABI，也没有获得它们
现成的驱动生态。“借鉴”应理解为目标行为和结构选择，不能理解为实现等价。

| 来源方向 | 选择的思路 | CapMix 的对应机制 | 当前成熟度边界 |
|---|---|---|---|
| macOS / XNU | 混合内核：高频 FS/net 路径留在内核 | VFS/COW 与包处理直接调用内核服务 | 已连通，不是完整 BSD 层 |
| macOS / Mach | 内存对象与映射；task/thread 分工 | VMO/VMAR、task、thread、capability IPC | 真实共享映射已验；没有完整 Mach API |
| macOS / APFS 选型方向 | COW 对象图与根提交 | 自有 COWFS、omap、A/B superblock | 不兼容 APFS 磁盘格式 |
| macOS / DriverKit | 将可隔离的驱动工作放在用户进程 | IRQ 经 channel 到 ring3 blockd；睡眠/唤醒/崩溃隔离 | 用户驱动机制已验；新增存储/NIC 驱动仍是内核最小实现 |
| macOS 调度/内存思路 | QoS、优先级传播、压力分层 | QoS bands、turnstile、压缩/换出/终止管线 | 有可运行机制，非完整 XNU 行为复刻 |
| Windows / NT | 对象/句柄、资源承诺与完成通知 | object/handle、commit/no-overcommit、完成队列、job | 不是 NT Object Manager/Win32 API |
| Windows 内存思路 | 申请资源时检查 backing 与 commit 限额 | commit charge/release、OVERCOMMIT 拒绝、预留比例 | 不是所有内核分配都自动成为用户 commit，也没有无限 pagefile |
| Windows IOCP / ETW 方向 | 完成队列、带因果身份的事件记录 | uring/blk completion、per-CPU trace、trace_id | 不是完整 IOCP 线程池或 ETW 协议兼容层 |
| Windows / ReFS 选型方向 | 元数据冗余、scrub、稀疏内容管理 | 自有 metadata DUP、损坏检测/受限修复、hole 读零 | 不兼容 ReFS 磁盘格式，也不保证单副本数据修复 |
| Linux 调度 | EEVDF：资格、虚拟时间、虚拟截止期 | `sched/eevdf.c` | 自身实现，有 SMP 测试；不等同 Linux 调度器全部策略 |
| Linux I/O/网络 | blk-mq、uring 形状、XDP/nft、受限 verifier | 块队列、提交/完成、包分类与过滤 | 没有 Linux 完整 io_uring、nftables 或 eBPF ISA/JIT |
| Linux FS/容器方向 | checksum、COW、分配组、资源控制 | COW 数据校验、AG、job/memory.max | 不提供完整 Btrfs/XFS、namespace 或 cgroup v2 文件接口 |
| capability 系统 / L4 | 对象权限为基础；同步 IPC 降低切换开销 | 有类型的对象、任务局部句柄、channel、handoff | 非 seL4 形式化验证；小消息仍是实际消息结构，不保证全驻寄存器 |

XNU 的混合组成可参考 [Apple 内核架构说明](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/Architecture/Architecture.html)；
用户态驱动方向参考 [Apple System Extensions/DriverKit](https://developer.apple.com/system-extensions/)。
CapMix 的具体分层则以本仓库 `config.h`、`sched.h`、`ipc.h` 和实现为准。

EEVDF 的资格与虚拟截止期思路见 [Linux EEVDF 文档](https://kernel.org/doc/html/latest/scheduler/sched-eevdf.html)。
完成队列方向参考 [Microsoft IOCP](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)；
承诺内存与可用 backing 的区别参考 [Microsoft pagefile/commit 说明](https://learn.microsoft.com/da-dk/troubleshoot/windows-client/performance/introduction-to-the-page-file)。
这些资料解释来源，不证明 CapMix 已达到其完整性或性能。

### 2.2 固定骨架与取舍

**权限先于兼容性。** 原生资源通过 capability handle 管理；POSIX 风格 fd 是
上层适配，不成为另一个绕过权限检查的资源入口。观察本身也要授权。

**spawn 优先于 fork。** `posix_spawn` 直接创建任务并准备镜像，避免把
fork 当所有启动路径的基础。fork 当前明确返回 NOTSUP；没有假装完成
地址空间复制和多线程 fork 语义。

**高频路径与隔离边界分开选择。** FS/net 留在内核，减少每次文件/包处理
都跨进程 IPC 的成本；驱动 runtime 用 channel 和能力权限隔离。
M3/M4/M7/M8 的硬件 bring-up 使用内核最小驱动是当前实现例外，不能由此
声称所有设备驱动已经用户态化。

**大块 IPC 传访问权，而非反复搬整块数据。** 小消息传 opcode、长度、
trace_id 和 handle；大块数据由 VMO backing pages 支撑，接收者映射相同页。
零拷贝描述的是 IPC payload 的共享映射，不表示创建初始内容或小消息元数据
完全没有复制。

**资源承诺与物理驻留分开。** commit 表示系统承诺能提供 backing；
resident 表示数据当前在 RAM 中。压力管线可以回收 clean cache、压缩匿名页、
换出，再按策略终止任务。允许压缩/换出不代表允许无限过量承诺。

**从一开始记录因果。** IPC trace_id 和静态 tracepoint 帮助解释
“哪个请求导致了调度、内存、I/O 行为”，而不是仅看到孤立时间戳。
目前因果传播不是对所有路径的百分之百覆盖。

### 2.3 不应从“融合”推导出的结论

- 不宣称比三个成熟内核都快；没有这样的可比工作负载证据。
- 不宣称具备三者的全部安全策略、兼容性或硬件支持。
- 不把结构相似的 API 名称当作行为完全相同，例如 uring、job、SIP、签名。
- 不把“有对象/接口/标志”当作“相关产品功能已经完整实现”。

## 3. 系统结构与实际执行路径

### 3.1 子系统位置

| 层 | 目录 | 责任 |
|---|---|---|
| 引导与架构 | `boot/`、`arch/x86_64/` | Limine、页表、CR3、ring3、syscall、IDT、LAPIC、MMIO |
| 内存 | `mm/` | buddy、slab、commit、匿名页、压力、缓存、VMO/VMAR、NUMA |
| 权限与 IPC | `cap/`、`ipc/` | 对象、句柄、引用计数、消息、能力转移、等待/唤醒 |
| 调度/进程 | `sched/`、`proc/` | EEVDF、QoS、task/thread/proc、ELF、PID1、syscall 分发 |
| 文件系统 | `fs/` | VFS、COW、snapshot/clone、checksum、scrub、fsync |
| 设备 I/O | `io/` | DriverKit 演示、IRQ、blk、uring、virtio-blk、AHCI |
| 网络 | `net/` | iface、包解析、lo UDP、XDP/nft、virtio-net、e1000、ARP/ICMP 探测 |
| 安全 | `security/`、`lib/sha256.c` | capability gate、策略覆盖、一次授权、摘要验证 |
| 虚拟化 | `virt/` | job、VM/vCPU/VFIO 对象，VMX/VMCS/EPT 最小真实路径 |
| 观测/控制台 | `obs/`、`console/` | per-CPU trace、授权读取、FB 文本、PS/2 输入 |

### 3.2 产品启动路径

```text
Limine → 串口与 cmdline/内存/FB 快照 → 内核自有页表
       → 内存/NUMA/ACPI、cap/sched/proc/syscall、块设备与 COW 根
       → 网络/安全/虚拟化基础初始化、LAPIC/AP 启动
       → bootstrap 供应缺失 ELF → CPL3 PID1 init 正式挂根 → CPL3 shell
       → run PATH → 真实 CPL3 静态 ELF → exit → off-CPU 回收
```

默认产品模式不依赖先跑完整 phase 测试。初始化基础子系统不等于执行其
破坏性 selftest。debug 才会建立真实根 → scratch RAM 根 → 恢复真实根
的测试流程，隔离压力/COW/设备 offline 测试。

产品 PID1 和 shell 已分别是 `/sbin/init.elf`、`/bin/shell.elf`，均真实 CPL=3。
用户 init/shell 固定 BSP；产品打开 AP 调度，shell
将 ELF round-robin 固定到 online CPU，`run ... &` 可后台运行。入口隔离、
overlap 与回收语义见 [ELF 并发说明](ELF_CONCURRENCY.md)；启动与 capability ABI 见 [用户 init/shell](USER_INIT_SHELL.md)。

### 3.3 四条重要数据路径

| 路径 | 实际步骤 | 核心边界 |
|---|---|---|
| 用户 ELF | VFS 读取 → PT_LOAD 校验/分配 → 用户页表 → iretq → syscall/抢占 → 回收 | 多个 ELF，固定 CPU 亲和；不是替换式 execve |
| VMO IPC | 创建页 → sender 映射/写入 → channel 传 handle → receiver 映射 | 相同 backing；任务局部 handle 数值可能不同 |
| IRQ 驱动 | top half 记录 → bottom half 投递 channel → 唤醒 BLOCKED → ring3 recv | 可合并/背压，生命周期检查；不是任意硬件 DMA 驱动全栈 |
| 硬件网络 probe | DMA TX → QEMU user peer ARP/echo → DMA RX → XDP/nft → 协议/内容校验 | 当前主要是 debug 中 BSP 轮询，不是产品后台 socket 服务 |

## 4. 当前能运行的最小系统与硬件条件

### 4.1 已验证的基础配置，不是最低硬件承诺

常用 freestanding 验收配置为 x86_64 QEMU、KVM、`-m 512 -cpu host -smp 4`。
已验证单节点和双节点；UEFI/GOP 产品路径使用 OVMF，也有用户侧 VMware
UEFI 产品启动/run ELF/poweroff 验收记录。具体新设备的 VMware 行为仍要
独立验证，不能把 QEMU 的设备 PASS 扩展成所有 VMware/真实硬件 PASS。

P0b 补充：VirtualBox 不提供 TSC-deadline 时，BSP 通过 PIT/TSC 参考标定
APIC countdown 频率，BSP/AP 共用标定值和周期 arm 路径，目标约1ms；
不再仅写死假定1GHz的count，也不让AP处于未arm状态。debug逐CPU测
128次IRQ稳态平均间隔，以标定TSC换算并检查平均误差<5%。这不保证每次
IRQ jitter<5%，不包含电源管理/hrtimer框架。详见 [P0b定时器](P0B_TIMER.md)。

没有测出完整产品/全量 debug 的最小 RAM 或最低 vCPU 数。
`boot/kmain.c` 的 2 MiB BSS fallback 只是应急分支；默认 RAM disk 自身就要
8 MiB，不能据此写成“2 MiB RAM 足够运行系统”。

| 缺失的环境能力 | 当前行为 |
|---|---|
| 没有 virtio-blk/AHCI 数据盘 | 使用 RAM COW 根，重启不持久 |
| 有受支持真实盘，但卷非空且不识别/损坏 | 拒绝自动格式化/挂载；产品保留拒绝启动策略，不悄悄换 RAM 冒充成功 |
| 没有受支持 NIC | 明确 skip，保留 lo 与其他测试；产品 nic unavailable |
| 没有 GOP/FB | 明确 skip，串口日志仍在；不是没有 framebuffer 就使全量失败 |
| 没有可用 PS/2/i8042 | 明确 skip；首版 shell 没有 serial input/USB input 替代栈 |
| 没有 VMX/必要 EPT 能力 | 硬件 guest 路径 skip，不把无虚拟化硬件判成整机失败 |
| 只有 SVM | 能探测；没有与 Intel 路径对等的 VMRUN guest 实现 |
| 没有可识别 ACPI S5 | 关机命令同步后打印原因并 safe-hlt；不保证宿主 VM 窗口自动关闭 |
| 没有有效 NUMA 表 | 单 node0 回退，保留原分配语义 |

KVM 不可用时可以尝试 TCG，但旧阶段存在 TCG 回归限制，不能保证全部
selftest 自动等价 PASS。无 VMX 路径与“整套 TCG 工作负载已验”是两件事。

### 4.2 当前用户可见功能

| 功能 | 可直接使用的状态 |
|---|---|
| shell | PS/2 输入；FB + serial 输出；help/echo/ls/cat/run/exit/poweroff/shutdown |
| 静态应用 | CapMix syscall ABI 的 little-endian x86_64 ET_EXEC，可抢占，正常 exit/reclaim |
| 文件持久化 | virtio-blk 或 AHCI 上的 COW 根，已验 sync/remount/跨启动读取 |
| 网络 | lo/NIC UDP syscall；后台 RX 线程、DHCP/静态回退；debug ARP/ICMP/UDP payload 验证 |
| 用户驱动 | 测试 ELF 已验 IRQ channel 收取、阻塞/唤醒、崩溃隔离 |
| 观测 | 授权 session 的 ring3 trace read/config；无权限拒绝 |
| 虚拟机 | 一次真实 VMLAUNCH → VMCALL / EPT violation → 完整 teardown 演示 |

没有完整 shell 作业控制、进程管道、用户应用 SDK、动态链接器、桌面、
guest 内图形安装器、完整网络服务或 Linux 客户机。

## 5. 当前代码的容量上限

下表是**当前默认构建**限制，不是 x86_64 的理论极限，也不是经验证可持续
并发吞吐量。实际可用量还受内存、commit、保留槽、根元数据和运行路径限制。
改大宏需要连同数组、生命周期、同步和测试一起评估。

### 5.1 CPU、内存与核心对象

| 项目 | 当前上限/策略 | 定义位置与说明 |
|---|---|---|
| CPU 槽位 | 8 | `config.h: CM_MAX_CPUS`；产品 AP 调度另有限制 |
| thread 槽位 | 128 | `CM_MAX_THREADS`；内核线程/测试也占用 |
| task 槽位 | 64 | `CM_MAX_TASKS`，不等同 proc 数量 |
| proc 槽位 | 32 | `proc.h: CM_MAX_PROCS`；不是 32 个并发 ELF |
| capability 对象池 | 4096 | `CM_MAX_CAPS`；不是每任务 4096 |
| 每 task 句柄表 | 256 槽 | `CM_HANDLES_PER_TASK`；保留/已占用槽会减少可安装数 |
| channel 池 | 256 | `CM_MAX_CHANNELS` |
| 每端 channel 队列 | 8 条 | `ipc/channel.c: CM_CHAN_DEPTH` |
| 基本页 | 4 KiB | `types.h: CM_PAGE_SIZE` |
| buddy 最大 order | 11，即单块 8 MiB | `CM_BUDDY_MAX_ORDER`；`size = 4096 × 2^order` |
| hosted buddy arena | 16 MiB | `mm/buddy.c`；宿主进程总 RSS 并不限于 16 MiB |
| freestanding 主池 | 最多 64 MiB | `boot/kmain.c`；不是使用全部客机 RAM |
| NUMA 节点 | 8 | `numa.h` |
| 每远端节点新增内存 | 最多 64 MiB 的最大交集 | SRAT enabled memory ∩ Limine usable RAM |
| 当前注册 RAM 的粗上界 | 主池 + 7 个远端池，最多约 512 MiB | 来源为现有每节点 cap；并非任意 512 MiB 连续内存都能加入 |
| NUMA 亲和范围 | 64 | CPU affinity 记录也有 64 项实现限制；Limine usable snapshot 128 项 |
| 页表逻辑级数 | max=5，默认 active=4 | 未用上层 folding；遵循 bootloader LA57，不在线任意切换 |
| commit 预留 | 6% 策略 | `CM_COMMIT_RESERVE_PCT`；实际 limit 看统计，不用机器 RAM 直接替代 |

48/57-bit 虚拟地址架构能力不能等价为本内核可用 RAM 或 ELF 地址范围。
当前 ELF 低半窗口、buddy pool、DMA 能力检查比架构地址宽度更直接地约束使用。

### 5.2 VMO、ELF 与文件系统

| 项目 | 默认上限 | 含义 |
|---|---|---|
| 每 VMO | 128 页 = 512 KiB | eager resident backing，不是完整可换页 pager 对象 |
| 每 VMAR | 32 个映射记录 | `vm.h: CM_VMAR_MAX_MAPS` |
| ELF program headers | 16 | `elf.h` |
| ELF backing 映射页 | 128 页，含栈 | 还要额外付出页表/内核栈等资源 |
| ELF 地址窗口 | 0x400000..0x800000 | PT_LOAD 上界为栈起点 0x7fc000；不是任意地址 ET_EXEC |
| ELF 用户栈 | 4 页 = 16 KiB | 栈顶 0x800000，初始 RSP 0x7fffe0 |
| ELF 并发 | 全局 32 proc 槽，包含 PID1/退出记录 | shell 最多 8 后台 + 1 前台；受任务/页/commit 等约束 |
| 每 proc fd | 32 槽 | fd 适配到 capability，表大小不等于开放给 ELF 的完整文件 API |
| VFS mount 槽 | 16 | `fs/vfs.c` |
| VFS open-file 槽 | 64 | 全局池 |
| VFS 路径缓冲 | 128 字节 | 不保证 128 个可见字符都可用，需留 NUL/内部处理空间 |
| 文件名 | 57 字节 | 字节限制，不是 57 个任意 UTF-8 汉字 |
| COW inode 槽 | 256 | 包括目录/根等，不能说 256 个普通文件全部可用 |
| snapshot | 8 | `CM_MAX_SNAPS` |
| 文件系统块 | 4 KiB | `FS_BS` |
| 单文件寻址粗上界 | 268 块 = 1,097,728 字节 | 12 direct + 256 indirect；磁盘实际空闲/快照占用会先限制 |

块后端目前暴露前 **8 MiB、16384 个 512B 扇区** 的固定卷布局：

| 区域 | 起始 LBA | 扇区数 | 大小 |
|---|---:|---:|---:|
| raw/selftest 保留区 | 0 | 64 | 32 KiB |
| swap 区 | 64 | 8192 | 4 MiB |
| COW FS 区 | 8256 | 8128 | 4,161,536 字节，约 3.97 MiB |

因此“64 MiB SATA 盘已挂载”不表示获得 64 MiB 文件系统，也不表示全部
3.97 MiB 都可做文件内容：superblock、DUP metadata、目录、快照等占用空间。
没有分区解析，也没有自动扩大文件系统。

### 5.3 网络、I/O、trace 和虚拟化

| 项目 | 当前上限 | 实際意义 |
|---|---|---|
| iface 表 | 4 | lo、硬件 NIC、测试 iface 共享 |
| 包缓冲池/单帧 | 64 / 1518B | `net.h`；不提供 jumbo/VLAN 全栈 |
| lo UDP endpoint | 16 | 原生 PORT capability |
| 每 UDP 队列 | 8 个 datagram | 非阻塞，满/空按错误码返回 |
| 每 UDP payload | 1472B | lo/vnet0/enet0，非阻塞，静态邻居/网关；非完整 BSD socket |
| 受限 BPF | 64 条指令，4 个程序 | 简化 ISA，无 JIT |
| nft 规则 | 16 | 简化规则模型 |
| blk queues / depth | 4 / 每队列 32 | 当前硬件仍同步单 in-flight；不是 128 个硬件并发命令 |
| uring entries | 256 | 自身 SQ/CQ 模型，不等同 Linux ABI |
| driver / IRQ / softirq 槽 | 16 / 32 / 64 | 虚拟 IRQ 与 broker 演示占用这些表 |
| e1000 RX/TX 环 | 每环 16 项 | 2048B RX backing，单 TX in-flight，18 包回卷已验 |
| AHCI | 单 SATA 盘、slot0、4 KiB bounce | 512B logical sector、LBA48、FLUSH_EXT |
| 静态 trace | 12 个 TP，512 项/CPU | 覆写，读者收到 lost 信息；不承诺无损 |
| legacy obs 环 | 配置宏 2048 项 | 与静态 TP 环不同，不能混用容量数字 |
| job / members | 16 / 每 job 16 | 资源对象/记账机制 |
| VM / vCPU / VFIO 表 | 4 / 4 / 8 | 对象容量，不等于已支持 4 个真实并行 guest |
| 当前真实 EPT guest | 7 个 4 KiB backing 页 | 单次进入退出；其余 GPA 无映射 |
| shell 行/cat | 255 字符 / 最多 4096B | 过长行拒绝，不执行截断命令；cat 非文本替换为 `.` |

## 6. 对象、权限、生命周期与错误码

### 6.1 四个容易混淆的概念

| 概念 | 定义 | 不是什么 |
|---|---|---|
| object | 内核资源实体，有 type、id、refs、payload、可选 destroy callback | object id 不等于访问凭证 |
| handle | 某 task 表中的局部槽：object 引用 + rights | 同一个整数在不同任务中不必指向同一对象 |
| task | 权限/句柄/QoS/内存节点等资源归属容器 | 不一定对应一个 POSIX 进程 |
| thread | 被 EEVDF 调度的执行单位，含状态和 context/kernel stack | 不是一个新的独立句柄权限域 |
| proc | task 的进程包装：pid、main thread、fd、ELF、退出信息 | 与 task/thread 不可直接互换 |
| fd | proc.fd 中的索引，指向 FILE capability handle | 不是另一个无需 capability 的原生资源 ID |

对象种类在 `cap.h` 中包括 TASK、THREAD、CHANNEL、ENDPOINT、VMAR、VMO、
IRQ、TRACE、JOB、PORT、EVENT、FILE。枚举存在不证明每种对象都有完整
用户 API；部分初始化对象仍是 placeholder，具体接口会检查 backing/type。

### 6.2 rights 的定义

| right | 位 | 定义方向 |
|---|---:|---|
| READ | 0 | 读对象内容/消息/文件 |
| WRITE | 1 | 写对象内容/发送消息/写文件 |
| EXEC | 2 | 执行相关访问权 |
| TRANSFER | 3 | 把能力经授权 IPC 转移 |
| DUPLICATE | 4 | 创建受限副本 |
| DESTROY | 5 | 销毁相关授权 |
| SIGNAL | 6 | 通知/信号相关权利 |
| WAIT | 7 | 等待对象事件 |
| MAP | 8 | 建立内存映射 |
| INSPECT | 9 | 检查对象；TRACE_READ 当前复用此位 |

`CM_RIGHT_ALL=0x3ff` 是位集合，不是对任何对象执行任意行为的万能令牌。
是否有具体操作仍由对象类型、函数检查和生命周期决定。
`CAP_TRACE_READ`/`CM_RIGHT_TRACE_READ` 目前是 INSPECT 别名，不是新增第 10 位。

### 6.3 核心所有权规则

- `cm_obj_mint[_owned]` 创建对象；owned 版本关联 backing 析构。
- `cm_obj_retain/release` 管理稳定引用；最后引用触发已定义的资源释放。
- `cm_handle_install` 建立任务局部访问入口；成功/失败时的引用责任要按实现处理。
- `cm_handle_acquire(task,hid,type,need,&out)` 同时验证类型、权限并取得稳定引用；
  调用者必须 release，不能把一次 lookup 的裸指针当跨调度长期租约。
- duplicate 使用 mask 限制 rights，不能扩大原权限。
- channel 成功 send 会移动附件 handle；发送失败保持原 handle。
- recv 成功安装接收者局部 handle；安装失败保持队列内容供重试。
- VMAR space lease 保持映射对象存活；释放 lease 前必须退出使用该 CR3 的上下文。
- ELF 只能在其线程已离开 CPU 后回收映射/页表/commit；不能在用户态仍执行时释放。

### 6.4 错误码

`cm_error_t` 是有符号 32-bit 整数，`CM_OK=0`，错误为负；syscall 用
`int64_t` 返回，某些成功返回值是字节数、fd/handle 等，不统一为 0。
这些是 CapMix 数值，不能直接当 Linux errno 或 Win32 error。

| 错误码 | 值 | 含义 |
|---|---:|---|
| NOMEM | -1 | 资源分配失败 |
| INVAL | -2 | 参数、范围、格式或上下文不合法 |
| DENIED | -3 | 权限不足/授权拒绝 |
| EXIST | -4 | 对象或路径已存在 |
| AGAIN | -5 | 暂无数据/背压/快照竞争，可按接口策略重试 |
| NOSPC | -6 | 固定表、队列、文件容量等耗尽 |
| NOTSUP | -7 | 未实现/当前路径不允许 |
| BUSY | -8 | 已有 in-flight、镜像未回收或资源仍使用中 |
| DEAD | -9 | peer/任务/设备已失效或 offline |
| OVERCOMMIT | -10 | 超过可承诺 backing/limit |
| IO | -11 | I/O、checksum、DMA completion 等错误 |
| NOENT | -12 | 文件/对象不存在 |
| NOTEMPTY / ISDIR / NOTDIR / ROFS | -13 / -14 / -15 / -16 | 文件系统条件错误 |

AGAIN 不是“自动睡眠”，DENIED 不是“暂时忙”，NOTSUP 不是“重试会实现”。
等待 API、非阻塞 API和受限 ELF 白名单必须分别理解。

## 7. 子系统接口定义与使用语义

### 7.1 API 不等于用户 ABI

`include/capmix/` 同时包含内核 C 接口、shared 数据结构和 syscall 编号。
一个 C 函数公开在头文件中，不表示任意 ring3 ELF 可以调用它，也不表示
跨版本 binary layout 已冻结。下面的 C API 例子默认由可信内核控制器调用。

### 7.2 内存与 NUMA

| 接口 | 意义 | 关键约束 |
|---|---|---|
| `cm_page_alloc(order)` | 分配 `2^order` 个连续基本页 | 返回地址是当前内核可使用的 backing；跨节点轮询/耗尽回退 |
| `cm_page_free(ptr,order)` | 按原 order 归还块 | 参数单位不是 byte，也不是任意页数；配对错误会损坏分配器 |
| `cm_page_alloc_node(node,order)` | 严格从指定节点分配 | 不静默返回其他节点；无内存/耗尽可失败 |
| `cm_page_node(ptr)` | 查询分配归属 | 用于统计/验证真实 backing 节点 |
| `cm_numa_bind_task(task,node)` | 设置任务节点元数据 | 配合 node 分配 API；不迁移已有页，不把普通 alloc 改成严格绑定 |
| `cm_numa_stats/node_info/distance` | 查询节点池与亲和距离 | 单 node0 回退与多节点统计都可观察 |
| `cm_commit_charge/uncharge/add_backing` | 按页数承诺/释放，或增加 backing limit | 与 buddy 物理空闲页不是同一统计 |
| `cm_pressure_reclaim/tick` | 触发/推进压力回收 | clean cache → 压缩匿名页 → swap → jetsam/OOM 策略 |

commit 成功不保证随后任意高阶连续块都能分配：碎片、节点严格绑定和
设备 DMA 地址限制仍会导致分配失败，调用者必须处理 NOMEM/INVAL 等返回。

NUMA 使用 SRAT CPU/memory affinity 和 SLIT 距离；分配均衡目前是新分配
round-robin + remote fallback，不是持续监控负载并迁移旧页面的自动平衡器。
THP 的“never always”配置是策略选择，不能据此认定具备完整 THP 产品实现。

### 7.3 VMO / VMAR / channel

| 接口 | 定义 |
|---|---|
| `cm_vmo_create(task,bytes,&hid)` | 创建具有真实页 backing 的内存对象并安装 handle |
| `cm_vmo_size(task,hid,&bytes)` | 校验对象后查询大小 |
| `cm_vmar_create(task,base,bytes,&hid)` | 创建任务拥有的虚拟区域及页表构建对象 |
| `cm_vmar_map(task,vmar,vmo,virt,offset,bytes)` | 将 VMO 范围映射到该 VMAR；按对齐、边界与权限检查 |
| `cm_vmar_acquire_space/release_space` | 持有稳定映射/页表租约，防止提前释放 |
| `cm_vm_space_pointer` | hosted 下取得同一 backing 页指针，不复制数据；访问不跨页 |
| `cm_channel_create(owner,&a,&b)` | 创建相反两端并返回 owner 的局部 handle |
| `cm_ipc_send/recv/call` | 发送、接收、请求-响应；可与调度 handoff/等待交互 |
| `cm_ipc_try_recv` | 空队列返回 AGAIN，不停车 |
| `cm_ipc_send_deferred` | bottom-half 安全：入队/唤醒，不直接切进 peer |
| wait prepare/cancel | 在生命周期锁/队列同步下准备或撤销等待，避免丢唤醒 |

`cm_ipc_msg_t` 包含 trace、opcode、len、nhandles、最多 64B 小数据和 4 个附件
handle。opcode 是上层协议，不是自动变成 syscall。trace 是因果身份，不是权限。

当前 VMO/VMAR 仅支持共享 RW 映射，要求相应 READ/WRITE/MAP 权限；
不提供完整匿名 mmap、只读/私有 COW 映射、pager、MAP_FIXED 产品语义。
VMAR 作为 kernel-facing inactive space builder，也没有自动变成完整用户
地址空间调度 API。ELF loader 使用自己的镜像页表/页 backing，不把两套
接口未经验证地混成一个 mmap 系统。

典型零拷贝流程：发送方创建 VMO → VMAR 映射并填充数据 → 若想保留本地
能力先 duplicate → send 附件能力 → 接收方 recv 获得新局部 handle →
创建自己的 VMAR 并映射 → 在不同虚址读到相同内容；共享写可以被另一方看到。
引用与 lease 全部释放后才回收 backing。phase1 还验证生命周期和失败回滚。

### 7.4 调度与进程/ELF

| 接口/字段 | 定义 |
|---|---|
| `cm_task_create` | 建立权限/资源归属域 |
| `cm_thread_create` | 为 task 建立执行上下文与内核栈 |
| `cm_eevdf_pick` | 从可运行候选中按资格与虚拟截止期选择 |
| `cm_sched_tick/yield` | 时间记账/抢占触发、主动让出当前上下文 |
| block prepare / block / wake | 原子准备等待、停车、重新加入 runnable |
| QoS | Interactive、Initiated、Default、Utility、Background、Idle |
| turnstile | 沿 blocked_on 等等待关系传播优先级，缓解优先级反转 |
| `cm_posix_spawn` | `entry!=NULL` 为既有 C 入口；`entry==NULL` 从 path 加载 ELF |
| `cm_proc_reap_elf` | off-CPU 后回收镜像、页表和 commit；保留退出记录 |
| `cm_proc_release_product_elf` | 产品 PID1 专用，进一步回收完成的 proc/task 槽 |
| `cm_fork_compat` | 固定 NOTSUP，不能用于创建进程 |

ELF 支持 ET_EXEC、PT_LOAD、BSS 清零、入口校验、权限/范围拒绝，IF=1 时
真实 timer 可抢占。它并不支持 ET_DYN、PT_INTERP、ld.so、TLS、完整 auxv、
argv/envp 字符串或并行多个 ELF。初始 argc=0，argv/envp 为终止项。
拒绝 W+X 声明并对非 writable 段清 W，但尚无完整数据/栈 NX，不能声称
完整硬件 W^X。普通恶意用户故障的隔离/恢复也还不完整。

### 7.5 VFS / COW / blk

| 接口 | 意义 |
|---|---|
| `cm_vfs_init/mount/umount` | 初始化/挂载/卸载 root 或 filesystem ops |
| `cm_vfs_open/read/write/pread/close` | 文件实例与位置操作；read/write 的 byte 数/负错误码要分别处理 |
| `cm_vfs_fsync/sync` | 提交文件/根修改并走后端 FLUSH；不是仅更新 RAM cache |
| snapshot / clone / scrub 接口 | 保留/共享 COW 根、复制引用、检查/修复可冗余修复的 metadata |
| `cm_blk_rw(op,lba,nsect,buf)` | 同步扇区接口，LBA/nsect 为 512B sector 单位 |
| `cm_blk_submit/poll` | 提交 bio、取完成项；同步路径按 tag 取自己的完成，避免偷别人的 CQ |
| `cm_blk_flush` | RAM 为无持久语义的成功；真实后端要求支持其 FLUSH |
| `cm_blk_select_backend` | boot/test-controller 切换；要求无 pending I/O、正确卸载根 |
| `cm_blk_select(0/1)` | 保留旧含义：RAM/virtio，不用它的参数 2 来选择 AHCI |
| `cm_blk_is_disk/backend_name` | 如实识别非 RAM 根及具体设备后端 |

后端优先级是 virtio-blk → AHCI → RAM。检测到真实盘后，不因卷无法识别
就把它静默覆盖。当前自动 mkfs 前扫描保留 8 MiB 区域，确认全空才格式化。

COW 使用 A/B superblock generation、对象/块指针 checksum、metadata DUP、
snapshot/clone 和 scrub。数据通常是 single-copy：能检测损坏，不能无中
生有修复唯一副本。NOCOW 是明确的原地覆盖逃生路径，会弱化常规 COW/checksum
保障；`CM_FS_COMPRESS` 仍为 reserved，不能当成文件系统已启用透明压缩。
掉电验证通过注入崩溃/撕裂写、重挂及真实后端 FLUSH 路径进行，不等价于
所有磁盘固件、写缓存、电源故障场景的工业级认证。

### 7.6 DriverKit / IRQ / blocking recv

IRQ handler 注册通过能力通道关联接收任务和设备。top half 尽量只记录事件，
bottom half 再投递。满队列可 backpressure/coalesce；offline、peer close、
任务死亡会取消等待、丢弃 pending 并使相关 I/O 失败。

`cm_drv_receive` 为非阻塞接收；`cm_drv_receive_wait` / `DRV_RECV_WAIT` 可以
让注册驱动主线程 BLOCKED。投递后 wake，恢复时重新校验 task、device、
endpoint identity，再消费事件。

**首版等待没有自动超时或 POSIX 信号中断。** 控制器可用
`cm_drv_wait_cancel(pid)` 唤醒并返回 AGAIN；测试 helper 与生命周期取消
避免悬挂。不要把“selftest 有 helper”写成“接口内建有限超时”。

已证明的顺序是 sleeping → IRQ delivered → awakened → blocking recv。
已证明驱动 task 死亡后的 device offline、pending 清除、buddy 可用、
kernel_alive；这不覆盖内核态 AHCI/NIC 驱动自身出现任意程序错误的情况。

### 7.7 网络接口

| 接口层 | 当前意义 | 用户可见边界 |
|---|---|---|
| `cm_if_add/get/find` | 内核 iface 注册与查询 | 不是 POSIX ifconfig/netlink |
| `cm_net_primary_iface` | 已注册 vnet0 优先，否则 enet0 | 不实现运行期自动 failover/路由 |
| `cm_net_rx` | 包解析、计数、XDP/nft 分类入口 | ICMP probe 在分类之后再验证内容 |
| `cm_udp_open/bind/sendto/recv/close` | lo datagram capability | ≤1472B、lo/NIC 非阻塞，无 TCP/BSD socket 全家桶 |
| virtio/e1000 transmit/receive | 真实 NIC DMA 发送/轮询取帧 | kernel-facing，不是直接开放给任意 ELF 的 raw NIC syscall |
| `cm_net_probe(ifid,label,send,recv)` | 单邻居 ARP + ICMP echo 和 payload 校验 | BSP 有界 selftest，状态每次调用独立 |
| BPF/nft load/attach/rule API | 受限包策略和 verifier | 没有通用 eBPF JIT、动态 attach 或完整 nft 语言 |

virtio-net 只支持已实现的 legacy/transitional PCI；e1000 支持明确名单
82540EM/82545EM/82574L。注册并存时首选 vnet0，即使它报告 down 也如实
显示，不声称有自动故障切换。

硬件 NIC 当前固定 10.0.2.15/24 和 peer 10.0.2.2，适合隔离的 QEMU user
network。VMware NAT/桥接网段可能不同；link up 不等价于互联网可用。
产品初始化网卡但没有持续轮询/协议处理服务，lo UDP 不因此自动扩展到
真实 NIC。单次成功 probe 不是完整 TCP/IP 栈。

### 7.8 安全、trace 与虚拟化接口

`cm_cap_check/cm_sec_gate` 负责 capability 与策略检查；DAC、Seatbelt、SIP、
authz 是已有策略层。one-shot token 只用于匹配的授权操作，并被消费。
这些模块不意味着所有可能的新内核入口已经完整接上统一安全策略。

`cm_sha256` 是 one-shot，init/update/final 为 streaming。
`cm_sign_verify` 计算内容摘要，并检查 32B expected digest 和政策标志。
expected digest 必须来自可信策略，不应直接相信镜像自报摘要；内容在
验证与使用间需要保持一致。该接口是完整性校验，不是 RSA/ECDSA/Ed25519
签名、证书链、公证或 secure boot。ELF path 目前没有因 phase6 而自动加入签名门控。

`cm_trace_open` 由可信内核策略创建 session，限制 TP mask、subject task ID、
CPU mask，并给任务安装 TRACE handle。read/config 验证 type/right 和
session 授权；不允许因为用户提供一个整数就读取全局日志。subject=0 是
可信策略显式授予的 all-task 范围，而非默认任意可读；当前 subject 过滤
粒度是 task，不是对任意 inode/channel 的完整细粒度 tracing ACL。
默认 capture off；编译期 build mask 和运行期 enabled mask 控制开销。
记录含 TSC、CPU、task/thread、trace/parent、seq、TP 和三个参数。
静态环按 CPU 保序，但没有全球跨 CPU 总序保证；满环覆写并报告 lost。
NMI、不丢事件、动态 kprobe 等不在当前承诺中。实际配置中的“启用掩码”
是采集机制，capability 才是用户读取/修改的授权边界。

虚拟化有两层：job/VM/vCPU/VFIO 对象与记账，以及真实硬件 probe/VMCS/launch。
旧 `cm_vcpu_run()` 的 synthetic vmexits 不能当真实 VM-exit 计数。
当前 Intel 路径检查 firmware policy、fixed bits、VMXON、VMPTRLD、VMCS、
EPT/INVEPT 能力，进入 7 页 guest 后 VMCALL exit_reason=18；故意访问
未映射 GPA 0x8000 产生 EPT violation reason=48。真实与合成计数并列。

EPT guest 使用独立一级 identity 页表和二级 EPT，不再共用 host CR3。
清理顺序包括 VMCLEAR、EPT teardown/INVEPT、VMXOFF、恢复 host CR/MSR，
再释放硬件已不持有的页。无法确认释放安全时保留/隔离，不冒险 free。
这还不是 guest 持续退出循环、Linux boot、多 vCPU 调度或真正 VFIO DMA/IOMMU 透传。

### 7.9 控制台与关机

FB 是 Limine framebuffer 上的 CPU 文本绘制；保留串口镜像。无 GPU 加速、
鼠标、用户态输入栈。PS/2 是 i8042/IRQ1/scancode 到字符队列；shell 消费
该队列，串口目前只有输出镜像，没有 serial shell input。输入是受支持 scancode
转换的 ASCII 字符，尚无 Unicode/输入法或完整终端协议。

`exit` 同步根后进入控制台 idle，不是断电。`poweroff/shutdown` 同步失败
留在 shell；成功后尝试受支持 ACPI S5，失败/不可用时安全 halt。
S5 parser 仅支持可可靠识别的根静态 `_S5` Package 和 16-bit SystemIO PM1，
不执行完整 AML `_PTS/_GTS`；hardware-reduced/MMIO/动态对象等降级。

## 8. 用户态 syscall ABI 和开放范围

### 8.1 x86_64 寄存器约定

现有 `arch/x86_64/syscall.S` 的约定为：

| 寄存器 | 含义 |
|---|---|
| RAX | syscall number；返回值也放在 RAX |
| RDI | a0 |
| RSI | a1 |
| RDX | a2 |
| R10 | a3 |

C 分发器形式为 `cm_syscall(nr,a0,a1,a2,a3)`。
四个参数接口不是 Linux 的六参数 syscall ABI；`SYSCALL` 的 RCX/R11 行为
遵循 x86_64 指令及现有入口实现，用户汇编不能假设它们跨 syscall 保持。

### 8.2 编号与功能

| nr | 名称 | 作用 | 普通静态 ELF 当前状态 |
|---:|---|---|---|
| 0 | NOP | 无操作 | 允许 |
| 1 | EXIT | 记录退出码/终止执行 | 允许 |
| 2 | WRITE | stdout/stderr 或既有 fd 适配 | 允许 stdout/stderr 与 FILE fd；统一 copyin，≤1 MiB |
| 3 | SPAWN | native attr / 用户 request | 用户 CONTROL EXEC 门控、嵌套 copyin/out、返回 CHILD capability |
| 4 | HANDLE_CLOSE | 释放局部 handle | ELF 白名单拒绝 |
| 5 | CHANNEL_CREATE | 创建 channel 两端 | ELF 白名单拒绝 |
| 6 | IPC_SEND | 复制小消息并发送 | ELF 白名单拒绝，通用 copyin/out 尚未补全 |
| 7 | IPC_RECV | 接收消息 | 注册驱动 ELF 特许的非阻塞路径；普通 ELF 不开放 |
| 8 | CLOCK | 返回 `cm_ticks_now()` 的原始 TSC tick | 允许，非纳秒/UTC/完整 POSIX clock_gettime |
| 9 | TRACE_NEW | 新建因果 trace id | 不是获得 TRACE_READ 权限；ELF 白名单拒绝 |
| 10 | COMMIT_STAT | 查询资源承诺 | ELF 白名单拒绝 |
| 11 | OPEN | VFS open → FILE cap → fd | 允许；fd/FILE 权限及 copyin/out 校验 |
| 12 | READ | fd 适配读文件 | 允许；fd/FILE 权限及 copyin/out 校验 |
| 13 | CLOSE | fd 适配关闭 | 允许；fd/FILE 权限及 copyin/out 校验 |
| 14 | MKDIR | 创建目录 | 允许；fd/FILE 权限及 copyin/out 校验 |
| 15 | UNLINK | 删除路径 | 允许；fd/FILE 权限及 copyin/out 校验 |
| 16 | FSYNC | 提交文件 | 允许；fd/FILE 权限及 copyin/out 校验 |
| 17 | UDP_OPEN | 创建 lo UDP endpoint handle | 允许进入原生处理器，受对象/资源条件限制 |
| 18 | UDP_BIND | lo/port 绑定 | 允许，权限/参数检查 |
| 19 | UDP_SEND | 发送 bounded payload | 允许，≤1472B，统一 copyin/out |
| 20 | UDP_RECV | 收取 datagram | 允许，无数据 AGAIN，校验目标用户范围 |
| 21 | UDP_CLOSE | 关闭 UDP handle | 允许 |
| 22 | TRACE_READ | 读 session 中下一事件/更新 cursor | 允许进入，但无授权 TRACE capability 返回 DENIED |
| 23 | TRACE_CONFIG | 按 session 范围配置 mask | 允许进入，仍受授权检查 |
| 25 | UDP_SENDADDR | 指定目标 IPv4/端口的 UDP 发送 | 允许；有类型 endpoint capability + copyin |
| 26–30 | TCP_OPEN/CONNECT/SEND/RECV/CLOSE | 主动 TCP capability | 允许；copyin/out，单次≤4096B，有限等待/非阻塞 recv |
| 24 | DRV_RECV_WAIT | 注册驱动睡眠等待 IRQ 消息 | 受注册/endpoint/lifecycle 条件限制 |
| 31–37 | console/child wait/yield/readdir/boot/poweroff | 用户 init/shell 控制 ABI | 类型/right/owner 校验；见 USER_INIT_SHELL.md |
| 128 | FORK | POSIX compat 占位 | NOTSUP |
| 129 | EXECVE | 替换式 execve 编号占位 | 没有对应实现，NOTSUP |

“表中存在函数”与“ring3 可用”之间还有 `proc/syscall.c` 的 ELF whitelist。
旧 C-entry/嵌入 ring3 测试入口保留历史接口，不应当作任意不可信 ELF 的
可用 SDK。VMO 创建/VMAR map 也还没有在这里作为完整用户 syscall 集合开放。

当前 stdout/stderr WRITE 是日志适配：内部显示缓冲最多 127B，可能截断显示，
但返回请求长度；它不是完整字节流 TTY/标准 POSIX write 语义。
编写测试应用应使用短消息，并使用 CapMix 编号和寄存器约定。

### 8.3 常用 syscall 参数与返回值

参数依次使用 RDI/RSI/RDX/R10；不用的参数建议置零。下表以当前 ELF
处理器为准；success/error 要先按有符号返回值判断，不能把负错误当无符号长度。

| 调用 | a0 | a1 | a2 | a3 | 成功返回 |
|---|---|---|---|---|---|
| EXIT | exit code | 0 | 0 | 0 | 终止，不按普通应用函数继续使用返回值 |
| WRITE | fd=1/2 | 可读 user buffer | byte length | 0 | 请求长度；日志可能只显示前 127B |
| CLOCK | 0 | 0 | 0 | 0 | 原始 TSC tick，不是 LAPIC tick 计数 |
| UDP_OPEN | 0 | 0 | 0 | 0 | task-local endpoint handle |
| UDP_BIND | handle | lo iface ID | port 1..65535 | 0 | CM_OK |
| UDP_SEND | handle | destination port | readable user buffer | length 1..1472 | 已发送 byte length |
| UDP_RECV | handle | writable/readable `cm_udp_datagram_t` buffer | 0 | 0 | payload length；空队列 AGAIN |
| UDP_CLOSE | handle | 0 | 0 | 0 | CM_OK |
| TRACE_READ | authorized handle | CPU index | readable/writable `cm_trace_read_t` | 0 | CM_OK：一个 event；AGAIN：暂无可读 event |
| TRACE_CONFIG | authorized handle | requested TP bit mask | 0 | 0 | CM_OK；需 WRITE 且不超过 session allowed mask |
| DRV_RECV_WAIT | registered recv handle | writable/readable `cm_ipc_msg_t` | 0 | 0 | CM_OK 后 buffer 含 IRQ 消息；可因取消/死亡返回错误 |

TRACE_READ buffer 包含 cursor、累计 lost 和一条 event；初次使用将 cursor/lost
置零。AGAIN 也可能更新 cursor/lost，不表示“永远没有产生事件”。TSC 不提供
UTC，也不能只按其数值推导跨 CPU 严格全局顺序。lo iface ID 需由当前注册状态
或受控启动信息取得，不应把某个示例中的编号硬编码为跨版本 ABI。

汇编示意（仅说明 syscall 约定，链接仍需遵循 `tests/hello_elf.ld` 的范围）：

```asm
    mov $2, %eax                 # CM_SYS_WRITE
    mov $1, %edi                 # stdout
    lea message(%rip), %rsi
    mov $(message_end-message), %edx
    xor %r10d, %r10d
    syscall
    mov $1, %eax                 # CM_SYS_EXIT
    xor %edi, %edi               # exit 0
    syscall
message:
    .ascii "hello from CapMix\n"
message_end:
```

## 9. 阶段与产品里程碑状态

### 9.1 phase 是机制阶段，M 是产品里程碑

它们不是同一个编号序列；phase2/phase4 还有后来追加的执行期抢占、阻塞 IRQ
接收等工作。下面按当前代码状态汇总，不只引用首版阶段限制。

| 阶段 | 已有并验收 | 当前不能据此声称 |
|---|---|---|
| phase0 | boot、4/5-level folding、KPTI、node0 路径 | 任意架构/任意内存规模支持 |
| phase1 | VMO/VMAR 零拷贝 IPC，共享写、生命周期、rollback | 完整用户 mmap/pager 或所有权限组合 |
| phase2 | 静态 ET_EXEC，从 VFS 启动、CPL3、抢占、exit/reclaim、畸形拒绝 | 多 ELF 并发、Linux ABI、execve 替换、动态链接、完整 NX |
| phase3 | 内存压力管线、真实匿名页内容、COW/snapshot/checksum/scrub | 完整 APFS/Btrfs/XFS/生产无限卷 |
| phase4 | IRQ→用户 driver channel，背压/合并、阻塞唤醒、peer-close/offline/crash 隔离 | 所有硬件驱动用户态化、PCI/DMA runtime 完整产品 |
| phase5 | lo IPv4 UDP、用户 syscall、payload 与 tx/rx | 硬件 NIC 上通用 socket/TCP、路由/NAT |
| phase6 | SHA-256、NIST vectors、trusted digest accept/reject | 公钥签名/证书/公证/verified boot/真 KASLR |
| phase7 | VMX/SVM 探测，Intel VMXON/OFF、fault rollback、skip | SVM guest、VFIO 实际透传、Linux VM |
| phase8 | 静态 TP、per-CPU 原子环、trace_id、cap 门控 read/config、ring3 授权/拒绝 | 无损 trace、NMI、全局时间总序、动态探针 |
| phase9 + EPT | 真 VMLAUNCH/VMCALL=18、EPT violation=48、INVEPT/rollback | 完整 guest loop、设备模型、持久多 vCPU VM |
| NUMA 扩展 | SRAT/SLIT、真实 per-node pools、RR/耗尽回退、bind/stat | 跨节点页迁移、后台自动重平衡/热插拔 |

| 产品里程碑 | 当前交付 |
|---|---|
| M2 | FB 文本 console、PS/2 键回显、无 GOP skip、serial 保留 |
| M3 | virtio-blk、COW 真实盘持久，sync/remount/重启读回 |
| M4 | virtio-net、静态 IP、off-lo ARP/ICMP payload 校验 |
| M5 | 默认 product、原生 PID1/shell、命令、真实 ELF run、debug 独立 |
| M6 | product/debug 清晰、状态横幅、ACPI shutdown/safe-hlt、UEFI 文档 |
| M7 | AHCI/SATA 同步读写/restore、COW 根、virtio→AHCI→RAM |
| M8 | e1000/e1000e、静态 IP、ARP/ICMP、环复用、virtio→e1000 首选 NIC |

### 9.2 最新已观察到的验收证据

M8 完成时的 hosted、e1000、e1000e、双 NIC、82545EM 双节点、无 NIC 日志
均出现 `selftest result: PASS (0 failures)`。硬件 NIC 回卷测试出现
`probes=9 tx_delta=18 rx_delta=18`。双 NIC 日志出现 `net primary: vnet0`。
UEFI 产品路径出现 `nic=enet0/up`、`hello from elf`、`exit=0 reclaim=0`、
`root sync=0` 与 ACPI S5 request，QEMU 正常退出。

M7 已有 SATA read/write restore PASS、COW 跨启动 marker 验证、UEFI root=disk
运行 ELF；非空未知 SATA 卷拒绝格式化，测试前后整盘哈希不变。
这些是此前实际验收记录，**不是本文编写时重新运行了一轮全量测试**。
`/tmp` 日志可用于当前工作区复查，但不是永久归档或跨机器可依赖的路径。

PASS 的含义是这次配置下测试断言通过；skip 的含义是相应能力没有被执行。
“全量 PASS 且 VMX skipped”不能写成“真实 VMX 已验证”；“无 NIC PASS”
不能写成“网卡收发已验证”。产品模式不运行全量 suite，因此不要求它打印
全量 PASS，也不能替产品启动日志杜撰 PASS。

## 10. 安全、隔离和可靠性的实际边界

### 10.1 已有防线

- 句柄局部性、类型/right 检查和转移不扩权。
- KPTI 用户 shadow/full CR3 真实路径，默认配置开启。
- ELF 格式/范围校验与受限 pointer-bearing syscall 白名单。
- 观测 session 对 TP/subject/CPU 的授权边界，ring3 granted/denied 已验。
- SHA-256 内容完整性、可信摘要比较、政策标志拒绝。
- 用户驱动 lifecycle、等待取消、设备 offline、pending 清除。
- COW checksum、metadata DUP 与受限修复，真盘空卷保护和 FLUSH。
- guest 的 EPT 最小映射隔离，以及 VMX/EPT失败清理/页隔离。
- 设备 DMA completion 校验、有界等待、复位/禁 DMA 后释放或 quarantine。

### 10.2 明确没有完成的防线/能力

| 项目 | 当前事实 |
|---|---|
| SMEP/SMAP | 真实日志读取 CR4；当前验收通常 SMEP=0、SMAP=0，并非已开启 |
| KASLR | 未实现真实随机重定位，实际 `kaslr_slide=0`；头文件遗留常量不是开启证据 |
| NX/W^X | ELF 尚未完整启用数据/栈 NX；EPT guest 权限与用户 ELF 权限不能混算 |
| 公钥签名 | 无 signer authentication、X.509、吊销、公证、根信任链 |
| Secure Boot/SSV | 未建立 verified boot/签名系统卷链，VMware/OVMF 测试关闭 Secure Boot |
| POSIX ACL/SBPL | 有 DAC/Seatbelt 等覆盖层，没有完整 ACL/SBPL 编译器 |
| 恶意应用隔离 | 不完整的用户 fault 恢复、SDK/copyin/out 覆盖；不承诺运行任意不可信 ELF |
| 真实驱动完全隔离 | AHCI/virtio/e1000 驱动在内核，错误可影响内核；用户 blockd 演示不覆盖它们 |
| IOMMU/VFIO | attach/记账对象不是已实现 DMA 隔离/透传 |
| 生产认证 | 无形式化验证或独立安全审计，不以自测 PASS 替代 |

内核 C API 位于可信域；用户 init/shell 已是 CPL3，但持有 bootstrap 管理能力，
镜像来源仍由启动策略信任。不能从“能力系统默认拒绝”推导出整个内核 C 代码
已经 memory-safe 或所有入口均可对任意不可信调用者开放。

## 11. 构建、运行与验收

### 11.1 文档对应的构建入口

```bash
make test                          # hosted regression
make kernel                        # freestanding capmix.elf
make iso                           # 默认 product capmix.iso
make iso LIMINE_CONFIG=boot/limine-selftest.conf  # 完整 debug ISO
```

工具链主要是 gcc/make 与 ISO/引导打包所需工具；具体打包依赖和编译参数看
Makefile。执行 `make iso` 会覆盖当前 ISO，切到 debug 后需再次 `make iso`
恢复默认产品。不要只看 `CAPMIX_VERSION` 判断镜像处于何种模式。

cmdline 为 `capmix.mode=product` 或 `capmix.mode=selftest`；未识别到模式默认为
product，准确匹配 token，最后一个已识别模式生效。hosted 始终 selftest。

### 11.2 产品：静态 ELF + 可选盘/网

仅在专用文件不存在时创建新盘，已有盘不要重复 truncate/create：

```bash
qemu-img create -f raw /tmp/capmix-data.img 64M
make iso
qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso \
  -drive file=/tmp/capmix-data.img,if=virtio,format=raw \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial stdio -display gtk -no-reboot
```

在虚拟屏上捕获 PS/2 键盘输入后：

```text
help
ls /bin
run /bin/hello.elf
poweroff
```

缺失 `/bin/hello.elf` 时由 init 经 VFS 安装现有 build/test ELF；已有文件
保留。没有磁盘使用 RAM 根，同样可 run，但重启不会保留文件。
`-display none` 可看 serial 输出，却不表示 serial 可输入 shell；自动化
可通过 QEMU monitor `sendkey` 注入 PS/2 键。VMware UEFI 细节见 M6/M7/M8 文档。

AHCI 改用以下盘参数：

```bash
-drive id=disk,file=/tmp/capmix-sata.img,if=none,format=raw \
-device ich9-ahci,id=ahci \
-device ide-hd,drive=disk,bus=ahci.0
```

网卡可改为 `virtio-net-pci` 或 `e1000e`。双 NIC 各接独立 user netdev、
不同 MAC；两套 QEMU user network 才能同时使用固定地址而不混淆邻居。
宿主文件格式 raw/VMDK 与客户机控制器 SATA/virtio 是不同层次，不能互相替代。

### 11.3 debug 标准验收

```bash
make kernel 2>&1 | grep -iE 'warn|error'
make iso LIMINE_CONFIG=boot/limine-selftest.conf

timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -serial stdio -display none \
  -no-reboot -d cpu_reset 2>&1 | tee /tmp/capmix_run.log

make iso                           # 恢复默认 product
```

`grep` 没有匹配时会返回 1，它不等于编译失败；需要保留/检查 make 本身退出状态。
可使用日志或 bash `PIPESTATUS` 来区分。既有 `.note.GNU-stack` 开头 warning
按当前项目验收约定忽略，但不能用它掩盖其他新增编译错误。

自测结束后 kernel idle/hlt，QEMU 被 timeout 结束的 124 是预期停机方式；
以日志完整 PASS 和具体硬件断言为准，而不是把 124 自动当测试失败。
双节点 QEMU 版本若不支持旧式 `-numa node,mem=...`，使用显式 backend：

```bash
-object memory-backend-ram,size=256M,id=m0 \
-object memory-backend-ram,size=256M,id=m1 \
-numa node,nodeid=0,cpus=0-1,memdev=m0 \
-numa node,nodeid=1,cpus=2-3,memdev=m1
```

对硬件路径要增加相应盘/NIC 参数，并确认出现 tx/rx、persist、vmexit=real
等具体日志；只看到一个总 PASS 不足以证明每个可选硬件路径都执行过。

## 12. 后续发展的上限与优先事项

### 12.1 设计路线可以继续走向什么

现有骨架允许继续扩展为支持多个原生用户进程、真实用户驱动服务、持续
网络收发、较完整资源控制与隔离 VM 的系统。这需要实现层和 ABI 持续演进，
而不是单纯把容量宏改大或加几个 syscall 编号。

| 优先方向 | 要补的关键工作 | 完成前的限制 |
|---|---|---|
| 多用户进程/SMP | 多独立地址空间、每 CPU user-entry 状态、调度/迁移/退出同步 | 已支持多个 ELF、固定亲和与双 ELF overlap；尚无用户迁移 |
| 用户 SDK/ABI | 安全 copyin/out、完整文件/VMO/channel 用户接口、参数启动、libc 子集 | 不能运行普通 Linux ELF，也不能从 ELF 自行调用全套 VFS |
| 完整故障隔离 | user page fault 转进程失败、异常栈/信号/恢复、更多负面测试 | 不能保证任意坏 ELF 只伤自己 |
| init/shell 用户态化 | 用户 console/input 服务与 fd/IPC 接口、原生 init ELF | 已是静态用户 ELF；尚无动态链接/完整服务管理 |
| 驱动 runtime | DMA/IOMMU 授权、设备资源对象、真实 IRQ/完成与用户服务 | 当前硬件驱动是内核最小实现 |
| 网络产品化 | 后台 RX、邻居超时、路由、真实 NIC UDP socket，之后 TCP 可靠性/续租 | 已具备后台 RX/NIC UDP；邻居无到期、无完整路由；最小主动 TCP 内核 API 和用户 ELF syscall 26–30 已支持，无重传/拥塞控制；DHCP 无续租 |
| 文件系统规模 | 可变卷布局、更多寻址、并发元数据、断电/恢复覆盖 | 固定前 8 MiB，自有小 COWFS |
| 安全链 | 公钥认证、可信 manifest/签名门控、NX、KASLR、审核策略覆盖 | digest 不能认证发布者，现有策略并非全覆盖 |
| NUMA | 使用更多真实 RAM、负载观测、页迁移、热插拔与调度协同 | 当前 RR 新分配与耗尽回退 |
| 虚拟化 | 持续 VM-exit loop、guest interrupts/device model、vCPU 调度、IOMMU | 当前单次 tiny guest，不是可用 Linux hypervisor |
| 观测工程化 | ABI 版本、导出器、跨 CPU 时间关联、丢失策略/压力测试 | 512 项环、允许覆写、不保证总序 |
| 硬件生态 | 更多 NIC/storage、NVMe、USB 输入、平台 quirks | 当前明确设备名单，不是通用驱动生态 |

### 12.2 不应承诺的“上限”

现在没有可靠证据给出最大百万 IOPS、百万包/秒、最大用户数量、最大
稳定运行天数、相对 Linux/Windows/macOS 的性能百分比或生产 SLA。
IPC RTT、计数、压力测试提供的是具体测试证据，不能外推出这些指标。

真正的扩展上限由内存布局、同步复杂度、驱动行为、安全模型和测试范围
共同决定。优先补齐生命周期/故障路径/用户接口，比宣称“架构可无限扩展”更有意义。

## 13. 源码和文档索引

| 主题 | 接口/实现 | 详细记录 |
|---|---|---|
| 配置/上限 | [config.h](../include/capmix/config.h)、[types.h](../include/capmix/types.h) | 本文第 5 节 |
| cap/IPC | [cap.h](../include/capmix/cap.h)、[ipc.h](../include/capmix/ipc.h)、`cap/`、`ipc/channel.c` | [phase1](PHASE1.md) |
| VMO/VMAR | [vm.h](../include/capmix/vm.h)、`mm/vm.c` | [phase1](PHASE1.md) |
| paging/KPTI | `arch/x86_64/paging.c`、入口汇编 | [phase0](PHASE0.md) |
| NUMA | [numa.h](../include/capmix/numa.h)、`mm/numa.c`、`mm/buddy.c` | [NUMA](NUMA.md) |
| ELF/ABI | [proc.h](../include/capmix/proc.h)、[syscall.h](../include/capmix/syscall.h)、`proc/elf.c`、`proc/syscall.c` | [phase2](PHASE2.md) |
| init/shell | `proc/init.c`、`user/shell.c`、Limine 配置 | [M5](M5_INIT_SHELL.md) |
| FS/storage | [fs.h](../include/capmix/fs.h)、[io.h](../include/capmix/io.h)、`fs/`、`io/blk.c` | [M3](M3_PERSIST.md)、[M7](M7_AHCI.md) |
| IRQ/用户驱动 | `io/driverkit.c`、`io/irq.c`、`boot/phase4_test.c` | [phase4](PHASE4.md)，含 blocking continuation |
| 网络 | [net.h](../include/capmix/net.h)、`net/` | [phase5](PHASE5.md)、[M4](M4_VIRTIO_NET.md)、[M8](M8_E1000.md) |
| SHA/政策 | [security.h](../include/capmix/security.h)、`security/`、`lib/sha256.c` | [phase6](PHASE6.md) |
| VMX/EPT | [virt.h](../include/capmix/virt.h)、`virt/hardware.c`、`virt/vmcs.c`、`virt/ept.c` | [phase7](PHASE7.md)、[phase9](PHASE9.md)、[EPT](EPT.md) |
| trace | [trace.h](../include/capmix/trace.h)、`obs/static_trace.c` | [phase8](PHASE8.md) |
| FB/PS2/power | `console/`、`arch/x86_64/power.c` | [M2](M2_CONSOLE.md)、[M6](M6_PRODUCT_BOOT.md) |

更新本总览时，应同时更新数值来源、ring3 whitelist、最新运行限制和验收
证据，不能只把状态表全部改成 PASS。阶段早期文档可能保留历史限制，
后续 continuation 与当前源码优先；参数/API 发生变化时必须重新核对。

磁盘安装续作已提供宿主 blank-only MBR/FAT32 工具、独立 CapMix 卷 offset 与 AHCI/virtio 无 ISO 冷启动。不是 guest 安装 GUI 或真实设备安装器；两次冷启动和拒绝覆盖步骤见 [INSTALL_DISK.md](INSTALL_DISK.md)。
