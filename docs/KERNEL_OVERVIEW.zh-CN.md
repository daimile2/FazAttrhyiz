# FazAttrhyiz 内核总览：设计理念、接口定义、能力边界与当前状态

现名 **FazAttrhyiz**（原 CapMix），原生发布后缀为 `.faz`；[更名与兼容说明](FAZATTRHYIZ.md)。本文历史实现名称和内部 `cm_*` 接口保留。

**更新日期：2026-10-15；覆盖 phase0–9、NUMA/EPT、M2–M8、P0a/P0b、P1a–P1d及后续用户态产品功能。**

本文面向使用者、内核开发者和评审者，回答四个问题：CapMix 想构建什么，
现在真正能做什么，各接口代表什么，以及当前实现的下限与上限在哪里。
参数以本仓库源码为准；阶段文档用于追踪实现细节和验收过程。

当前版本宏仍是 `0.8.0-phase8`，属于历史命名，不代表 phase9 或 M8 尚未完成。
本文不修改内核版本号，也不把里程碑编号当作稳定 ABI 版本。

本次更新纳入：buddy SMP soak、定时器标定、文件 syscall/copyin/out、后台 NIC UDP、
DHCP、主动 TCP及RTO、用户网络 ABI、argv/envp、ELF并发/run&、用户态 init/shell、
无 ISO 磁盘安装、kill/有界关机、外部 GCC SDK、e1000/virtio-net RX IRQ、
hello/nc/cat/ls/http-get 示例。以下状态、边界和接口均按这些续作修订；验收证据见第9节。

文档分类见 [docs导航](README.md)，VBox实操见 [VirtualBox测试手册](VIRTUALBOX_TESTING.zh-CN.md)。

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
| freestanding | Limine引导的独立内核，支持ISO或安装盘，在QEMU/兼容虚拟机中运行 | ring3、KPTI、抢占、中断、真实设备模拟器收发、VMX/EPT、产品启动 | 全部真实硬件兼容性、生产安全认证、无限规模运行 |

**当前下限**指已经连通并验收的基本使用闭环：引导 → 挂根 → PID1 →
用户态文本 shell → 加载带 argv 的 CapMix ABI 静态 ELF → 并发执行/退出/回收 → 同步/关机。
安装盘可不挂 ISO 冷启动；用户程序已能操作持久文件，使用 UDP/TCP，并运行 HTTP GET 示例。
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
| 网络 | `net/` | iface、lo/NIC UDP、主动 TCP/RTO、DHCP、XDP/nft、两类 NIC、IRQ/后台 RX |
| 安全 | `security/`、`lib/sha256.c` | capability gate、策略覆盖、一次授权、摘要验证 |
| 虚拟化 | `virt/` | job、VM/vCPU/VFIO 对象，VMX/VMCS/EPT 最小真实路径 |
| 观测/控制台 | `obs/`、`console/` | per-CPU trace、授权读取、FB 文本、PS/2 输入 |

### 3.2 产品启动路径

```text
ISO或安装盘ESP中的 Limine → 串口与 cmdline/内存/FB 快照 → 内核自有页表
       → 内存/NUMA/ACPI、cap/sched/proc/syscall、块设备与 COW 根
       → 网络/安全/虚拟化基础初始化、LAPIC/AP 启动
       → bootstrap 供应缺失 ELF → CPL3 PID1 init 正式挂根/配置网络 → CPL3 shell
       → run PATH [ARGS] [&] → 真实 CPL3 静态 ELF → exit/kill → off-CPU 回收
```

默认产品模式不依赖先跑完整 phase 测试。初始化基础子系统不等于执行其
破坏性 selftest。debug 才会建立真实根 → scratch RAM 根 → 恢复真实根
的测试流程，隔离压力/COW/设备 offline 测试。

产品 PID1 和 shell 已分别是 `/sbin/init.elf`、`/bin/shell.elf`，均真实 CPL=3。
用户 init/shell 固定 BSP；产品打开 AP 调度，shell
将普通 ELF round-robin 固定到 online CPU，`run ... &` 可后台运行；
`run -i` 交互子进程固定 BSP并获受限控制台权限，禁止与 `&` 组合。入口隔离、
overlap 与回收语义见 [ELF 并发说明](ELF_CONCURRENCY.md)；启动与 capability ABI 见 [用户 init/shell](USER_INIT_SHELL.md)。

### 3.3 四条重要数据路径

| 路径 | 实际步骤 | 核心边界 |
|---|---|---|
| 用户 ELF | VFS 读取 → PT_LOAD 校验/分配 → 用户页表 → iretq → syscall/抢占 → 回收 | 多个 ELF，固定 CPU 亲和；不是替换式 execve |
| VMO IPC | 创建页 → sender 映射/写入 → channel 传 handle → receiver 映射 | 相同 backing；任务局部 handle 数值可能不同 |
| IRQ 驱动 | top half 记录 → bottom half 投递 channel → 唤醒 BLOCKED → ring3 recv | 可合并/背压，生命周期检查；不是任意硬件 DMA 驱动全栈 |
| 硬件网络 | UDP/TCP capability syscall → DMA TX → 对端 → RX IRQ → BLOCKED worker唤醒 → ARP/UDP/TCP ingress → 用户 recv | 已有产品后台路径；未知/冲突IRQ路由回退轮询，非完整BSD socket |

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
| shell | CPL3 ELF，PS/2输入、FB/serial输出；help/echo/ls/cat/run/run -i/run &/kill/exit/poweroff |
| 静态应用 | little-endian x86_64 ET_EXEC，argv/envp、可抢占、多ELF固定亲和并发、off-CPU reap |
| 文件持久化 | virtio-blk/AHCI COW根，用户ELF open/read/write/close/mkdir/unlink/fsync，跨启动读取 |
| 网络 | lo/NIC UDP、主动TCP用户syscall、连接级RTO、DHCP/静态回退、e1000/virtio-net RX IRQ与后台线程 |
| 安装/引导 | blank-only宿主安装工具，Limine BIOS/UEFI + 独立COW根，AHCI/virtio盘无ISO冷启动 |
| 开发/示例 | 宿主GCC + CapMix crt/链接脚本；hello、nc、cat、ls、http-get可安装并shell run |
| 用户驱动 | 测试 ELF 已验 IRQ channel 收取、阻塞/唤醒、崩溃隔离 |
| 观测 | 授权 session 的 ring3 trace read/config；无权限拒绝 |
| 虚拟机 | 一次真实 VMLAUNCH → VMCALL / EPT violation → 完整 teardown 演示 |

已有最小 freestanding SDK；没有完整 libc、shell作业控制、进程管道、动态链接器、
桌面、guest内图形安装器、通用TCP服务端或Linux客户机。

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
| ELF backing 映射页 | 原生128页；可选Linux L5为2048页/8MiB，含栈 | 还要额外付出页表/内核栈等资源 |
| ELF 地址窗口 | 0x400000..0x800000 | PT_LOAD 上界为栈起点 0x7fc000；不是任意地址 ET_EXEC |
| ELF 用户栈 | 4 页 = 16 KiB | 栈顶0x800000；初始RSP随argv/envp大小变化并保持16B对齐 |
| argv/envp | 各最多32项，每项255B+NUL，总字符串4096B含NUL | argc≥1；缺argv时默认镜像路径；不自动继承任意环境 |
| ELF 并发 | 全局 32 proc 槽，包含 PID1/退出记录 | shell 最多 8 后台 + 1 前台；受任务/页/commit 等约束 |
| 每 proc fd | 64 槽（0..63；见 [FD_TABLE](FD_TABLE.md)） | FILE capability适配；标准fd保留，非完整POSIX描述符功能 |
| 文件单次read/write | 1 MiB含 | 完整用户范围先校验，再用4KiB临时页分块copyin/out；实际映射限制可更小 |
| VFS mount 槽 | 16 | `fs/vfs.c` |
| VFS open-file 槽 | 64 | 全局池 |
| VFS 路径缓冲 | 128 字节 | 不保证 128 个可见字符都可用，需留 NUL/内部处理空间 |
| 文件名 | 57 字节 | 字节限制，不是 57 个任意 UTF-8 汉字 |
| COW inode 槽 | 256 | 包括目录/根等，不能说 256 个普通文件全部可用 |
| snapshot | 8 | `CM_MAX_SNAPS` |
| 文件系统块 | 4 KiB | `FS_BS` |
| 单文件寻址粗上界 | 268 块 = 1,097,728 字节 | 12 direct + 256 indirect；磁盘实际空闲/快照占用会先限制 |

块层向根服务暴露 **8 MiB、16384个512B扇区** 的固定逻辑卷；RAM/旧数据盘base=0，
安装盘base=2048。下表是卷内逻辑LBA，不是安装盘物理LBA：

| 区域 | 起始 LBA | 扇区数 | 大小 |
|---|---:|---:|---:|
| raw/selftest 保留区 | 0 | 64 | 32 KiB |
| swap 区 | 64 | 8192 | 4 MiB |
| COW FS 区 | 8256 | 8128 | 4,161,536 字节，约 3.97 MiB |

因此“64 MiB SATA 盘已挂载”不表示获得 64 MiB 文件系统，也不表示全部
3.97 MiB 都可做文件内容：superblock、DUP metadata、目录、快照等占用空间。
已实现专用固定MBR安装布局解析，不是任意GPT/MBR分区发现器，也没有自动扩大文件系统。
安装镜像默认/最小128MiB：LBA2048起8MiB CapMix卷，LBA18432起64MiB FAT32 ESP。
COW物理起点为10304；ESP只供引导，运行期VFS根为COW。详见 [安装布局](INSTALL_DISK.md)。

### 5.3 网络、I/O、trace 和虚拟化

| 项目 | 当前上限 | 实際意义 |
|---|---|---|
| iface 表 | 4 | lo、硬件 NIC、测试 iface 共享 |
| 包缓冲池/单帧 | 64 / 1518B | `net.h`；不提供 jumbo/VLAN 全栈 |
| UDP endpoint | 16 | lo/vnet0/enet0共用，原生PORT capability |
| 每 UDP 队列 | 8 个 datagram | 非阻塞，满/空按错误码返回 |
| 每 UDP payload | 1472B | lo/vnet0/enet0，非阻塞；NIC地址可用DHCP lease，非完整BSD socket |
| TCP连接槽 / RX窗口 | 32 / 默认每连接16KiB（编译可选4–32KiB） | 主动/被动/监听共享32槽；backlog≤8，无窗口缩放 |
| TCP MSS / 单次用户I/O | 536B / 4096B | MSS可受对端更小值限制；一次仅一段待确认 |
| TCP重传 | 初始200ms，最大1600ms，最多4次重传 | SYN/数据/FIN；指数退避，API总期限可能先终止，无拥塞控制 |
| RX预算/等待 | 16帧/接口，idle BLOCKED | 已路由IRQ通知为主；10ms推进RTO/回退，100ms IRQ watchdog |
| NIC IRQ路由 | 0x42 e1000、0x43 virtio-net | 已验证i440FX INTx及受限VBox ICH9 e1000；共享GSI后注册者回退，无MSI-X/共享fanout |
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
IRQ、TRACE、JOB、PORT、EVENT、FILE，以及CONSOLE、CONTROL、BOOT、CHILD。枚举存在不证明每种对象都有完整
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

buddy 的 free-list、split/coalesce、arena注册、RR cursor及计数由同一IRQ-safe自旋锁
串行化；不是每CPU缓存方案，不用于NMI。P0a的4 CPU soak为每CPU10000轮、
总160000次分配/释放，以页owner与内容标记检查重复、损坏和泄漏，并核对逐节点快照。
hosted单CPU测试不能替代此SMP证据。详见 [P0a](P0A_BUDDY_SMP.md)。

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
| `cm_proc_release_child` | 校验parent/child关系后，回收off-CPU的子进程记录、task与入口栈资源 |
| `cm_proc_request_kill` | 发布终止请求；不直接远程释放活跃栈/页表 |
| `cm_fork_compat` | 固定 NOTSUP，不能用于创建进程 |

ELF 支持 ET_EXEC、PT_LOAD、BSS 清零、入口校验、权限/范围拒绝，IF=1 时
真实timer可抢占；已支持argv/envp栈布局、argc≥1、多个ELF执行时间重叠和
同CPU阻塞syscall现场隔离。每镜像固定CPU亲和，不做运行期迁移；
用户SPAWN经CONTROL capability授权和嵌套指针快照，父进程等待/回收走CHILD handle。
它不支持ET_DYN、PT_INTERP、ld.so、完整TLS/auxv或替换式execve。
argv/envp、crt与并发容量见 [ELF_ARGV](ELF_ARGV.md)、[ELF_CONCURRENCY](ELF_CONCURRENCY.md)。
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
就把它静默覆盖。自动mkfs前扫描所选逻辑卷的8MiB窗口，确认全空才格式化；
安装盘的窗口先经专用MBR严格校验再加base偏移，不扫描/覆盖ESP来冒充空卷。

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
| `cm_if_add/get/find` | 内核iface注册/查询 | 非POSIX ifconfig/netlink |
| `cm_net_primary_iface` | vnet0优先，否则enet0 | TCP AUTO选up的可用主接口；非完整运行期路由/failover |
| `cm_udp_open/bind/sendaddr/recv/close` | lo/NIC datagram capability | ≤1472B；RECV非阻塞，空队列AGAIN |
| `cm_tcp_open/connect/send/recv/close` | 主动TCP PORT capability | CONNECT/SEND/CLOSE有限等待；RECV非阻塞、EOF=0 |
| `cm_net_bg_start/stop` | EEVDF线程消费DMA RX，推进协议及RTO | 产品启动常驻；无需应用直接poll NIC |
| `cm_net_rx_notify` | IRQ原子发布接口通知bit | deferred hook在rq锁外唤醒BLOCKED worker；ISR不解析包/取协议锁 |
| virtio/e1000 transmit/receive | NIC同步TX和有界RX drain | 不向任意ELF开放raw DMA地址 |
| DHCP boot configure | DISCOVER→OFFER→REQUEST→ACK | lease地址/掩码/网关；超时保留原静态配置 |
| BPF/nft API | 受限verifier/包策略 | 非完整eBPF ISA、JIT或nft语言 |
| `cm_net_probe` | ARP/ICMP硬件自测 | 原有BSP有界probe仍保留，不代表产品RX仍只靠手动轮询 |

virtio-net仍为legacy/transitional PCI；e1000支持82540EM/82545EM/82574L名单。
e1000和virtio-net在已验证i440FX路由上使用真实INTx；e1000另支持受限的VBox ICH9 root-bus路由（见 [IRQ/性能证据](VBOX_ICH9_INTX.md)）。top half确认设备中断、
计数、notify、EOI，worker处理ARP/UDP/TCP/DHCP并入队。virtio IRQ模式下TX/RX
轮询不再读取read-to-clear ISR，防止误清RX通知；TX仍同步并请求抑制中断。
未知平台路由、无IOAPIC或GSI冲突时明确fallback，后注册设备不覆盖已有IRQ路由。
worker空闲BLOCKED，约10ms唤醒推进RTO/无IRQ轮询；IRQ接口另有100ms健康检查。
这不代表整机AP scheduler或shell已完全停止轮询。详见 [RX IRQ](NET_RX_IRQ.md)。

默认静态回退为10.0.2.15/24、网关10.0.2.2；成功DHCP ACK后使用lease配置。
`capmix.net=static`/`capmix.net=dhcp`用于选择策略（见 [P1c](P1C_DHCP.md)）。
无网卡skip，超时fallback；没有完整续租/到期调度、DNS或完整路由/NAT。
VMware NAT/桥接网段可能不同，link up不保证互联网可达。

TCP状态子集包括SYN_SENT、ESTABLISHED、FIN_WAIT_1/2、CLOSING、TIME_WAIT、
CLOSE_WAIT、LAST_ACK及错误/关闭态。SYN、数据、FIN保存一个待确认段，初始RTO
200ms，退避至1600ms；原始发送加最多4次重传，耗尽/总期限失败并尽力RST，
CLOSE或最后引用/进程退出回收。部分SEND返回已确认字节，不能无条件重发整个请求。
无RTT自适应、拥塞控制、fast retransmit、SACK、乱序队列、完整零窗口探测、
IPv6或listen/accept；TIME_WAIT=100ms，并非完整互联网TCP实现。
详见 [TCP ABI](USER_NETWORK_ABI.md)、[RTO](TCP_RTO.md)、[UDP](P1B_UDP_NIC.md)。

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

用户shell已支持前台 `>`、`>>`、`<` 和普通ELF标准I/O绑定；
重定向与 `run -i`/后台`&`互斥，暂不提供pipe。覆盖/追加、权限与冷启动验收见
[SHELL](SHELL.md)。提示符/reap日志不写入重定向文件，应用stdout单独走FILE。


FB 是 Limine framebuffer 上的 CPU 文本绘制；保留串口镜像。无 GPU 加速、
鼠标、完整用户态输入服务。PS/2 是 i8042/IRQ1/scancode 到字符队列；shell 消费
该队列，串口目前只有输出镜像，没有 serial shell input。输入是受支持 scancode
转换的 ASCII 字符，尚无 Unicode/输入法或完整终端协议。

`kill PID` 仅查本shell保存的后台任务，以CHILD/SIGNAL发终止请求；PID本身不是授权。
用户死循环由tick抢占后终止；若在syscall中，等continuation展开再处理请求，
不远程free活跃栈。正常强制终止exit=137，并非完整POSIX signal。

`exit`/`poweroff`/`shutdown`先正常等待后台最多5秒，超时发kill再等最多2秒。
全部reap后正常sync；`exit`仍未收尾则BUSY留在shell，poweroff则明确FORCE降级：
保留活跃现场、跳过可能等待锁的sync并关机，不保证未同步数据持久。
正常sync失败留在shell；成功尝试ACPI S5，失败/不可用时安全halt。
时间使用原TSC与syscall39 Hz，各等待阶段另有迭代上限。正常exit结束shell、
由PID1等待回收后退出并回到内核idle，不自动断电。详见 [kill/关机](KILL_SHUTDOWN.md)。
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
| 14 | MKDIR | 创建目录 | 允许；路径copyinstr和VFS检查 |
| 15 | UNLINK | 删除路径 | 允许；路径copyinstr和VFS检查 |
| 16 | FSYNC | 提交文件 | 允许；fd/FILE 权限及 copyin/out 校验 |
| 17 | UDP_OPEN | 创建 lo/NIC UDP endpoint handle | 允许进入原生处理器，受对象/资源条件限制 |
| 18 | UDP_BIND | lo/vnet0/enet0与端口绑定 | 允许，权限/参数检查 |
| 19 | UDP_SEND | 发送 bounded payload | 允许，≤1472B，统一 copyin/out |
| 20 | UDP_RECV | 收取 datagram | 允许，无数据 AGAIN，校验目标用户范围 |
| 21 | UDP_CLOSE | 关闭 UDP handle | 允许 |
| 22 | TRACE_READ | 读 session 中下一事件/更新 cursor | 允许进入，但无授权 TRACE capability 返回 DENIED |
| 23 | TRACE_CONFIG | 按 session 范围配置 mask | 允许进入，仍受授权检查 |
| 25 | UDP_SENDADDR | 指定目标 IPv4/端口的 UDP 发送 | 允许；有类型 endpoint capability + copyin |
| 26–30 | TCP_OPEN/CONNECT/SEND/RECV/CLOSE | 主动 TCP capability | 允许；copyin/out，单次≤4096B，有限等待/非阻塞 recv |
| 24 | DRV_RECV_WAIT | 注册驱动睡眠等待 IRQ 消息 | 受注册/endpoint/lifecycle 条件限制 |
| 31–37 | console/child wait/yield/readdir/boot/poweroff | 用户init/shell控制ABI | 类型/right/owner校验；console可显式受限委托 |
| 38 | CHILD_KILL | 子进程终止请求 | CHILD/SIGNAL + owner/parent校验，0表示请求已接受 |
| 39 | TSC_HZ | 查询已有标定TSC频率 | 无参数，返回Hz；0表示不可用，不是重新标定 |
| 128 | FORK | POSIX compat 占位 | NOTSUP |
| 129 | EXECVE | 替换式 execve 编号占位 | 没有对应实现，NOTSUP |

“表中存在函数”与“ring3 可用”之间还有 `proc/syscall.c` 的 ELF whitelist。
旧 C-entry/嵌入 ring3 测试入口保留历史接口，不应当作任意不可信 ELF 的
接口。已有独立CapMix SDK，见8.4；VMO创建/VMAR map仍没有完整用户syscall集合。

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
| UDP_BIND | handle | lo/NIC iface ID | port 1..65535 | 0 | CM_OK |
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

### 8.4 SDK、用户控制与可运行示例

`sdk/crt0.S`、`sdk/capmix.ld`、`sdk/include/capmix.h`可由宿主GCC构建静态ET_EXEC：
`-static -nostdlib -no-pie`，使用CapMix编号和寄存器约定，不链接Linux/glibc。
crt从栈读argc/argv/envp，以SysV C约定调用main，返回后syscall EXIT。
禁用red-zone、SIMD/FPU依赖、栈保护与PIE；没有printf/malloc、ld.so或完整libc。
直接 `gcc -static hello.c` 生成的Linux程序不能因此运行。详见 [外部ELF](EXTERNAL_ELF.md)。

| 用户接口 | 定义和权限 |
|---|---|
| SPAWN 3 | CONTROL/EXEC + 32B request(path/argv/envp指针、flags、cpu)，返回16B child结果 |
| CHILD_WAIT 33 | CHILD/WAIT + 32B结果；非阻塞flag=1；完成后消耗handle并reap |
| CHILD_KILL 38 | CHILD/SIGNAL + owner；接受请求不等于已完成回收 |
| CONSOLE_READ/WRITE 31/32 | CONSOLE READ/WRITE；READ空队列AGAIN，WRITE受4096B上限与copyin检查 |
| BOOT_CONTROL 36 | PID1专用挂根/网络配置；普通子程序无BOOT管理权 |
| POWEROFF 37 | BOOT/DESTROY；flags0正常、2为受控FORCE |
| READDIR 35 / TSC_HZ 39 | 目录项查询 / 已有时钟频率；仍有用户范围与参数校验 |

用户PID1获CONTROL/CONSOLE/BOOT bootstrap能力，shell获受限管理组合。
普通run不自动获得这些管理权限；`run -i PATH`通过spawn flags=2显式委托
CONSOLE READ|WRITE到子进程handle1，不带DUPLICATE/CONTROL/BOOT，并固定BSP。
shell要求前台使用，拒绝 `run -i ... &`；父进程需console DUPLICATE权。
诊断fd1/fd2与原始console能力不同，普通程序可打印但不能默认读键盘。

| 用户ELF | 作用与边界 |
|---|---|
| `/bin/hello-sdk.elf` | 外部GCC约定及argv打印；原`hello.elf`保留测试用途 |
| `/bin/nc.elf` | `run -i`后主动TCP连接、逐行键盘发送、网络回显；`/quit`关闭，空闲30秒退出124 |
| `/bin/cat.elf` | FILE syscall流式读持久根文本，最多显示1MiB，非文本替换为`.` |
| `/bin/ls.elf` | READDIR列目录，最多256项 |
| `/bin/http-get.elf` | hosts主机名/数字IPv4+端口+path，HTTP/1.0 GET，状态行与前1024B正文 |

HTTP客户端不做TLS/DNS/自动重定向/chunked解码；正文最多1MiB并drain至EOF，
头最多8192B，接收总10秒/空闲3秒超时；2xx退出0，3xx–5xx退出4，
参数错误2、协议/网络错误1、超时124。不等于通用浏览器或完整HTTP栈。
示例与SDK输出分块避免诊断WRITE的127B显示截断。详见 [EXAMPLES](EXAMPLES.md)。

`make examples iso`构建并嵌入初始示例；产品仅provision缺失文件并fsync，
已有磁盘文件不覆盖。`make install-disk DISK_IMAGE=新路径`准备空盘引导，
首次无ISO启动生成COW根/系统ELF，第二次冷启动读已有文件。另可关闭VM后用
`scripts/copy-to-disk.py`导入新路径到已初始化安装RAW盘：staging、锁、重挂读回，
拒绝覆盖/未知布局，不改ESP或安装布局。外部程序不必编进内核才能被run。

## 9. 阶段与产品里程碑状态

### 9.1 phase 是机制阶段，M 是产品里程碑

它们不是同一个编号序列；phase2/phase4 还有后来追加的执行期抢占、阻塞 IRQ
接收等工作。下面按当前代码状态汇总，不只引用首版阶段限制。

| 阶段 | 已有并验收 | 当前不能据此声称 |
|---|---|---|
| phase0 | boot、4/5-level folding、KPTI、node0 路径 | 任意架构/任意内存规模支持 |
| phase1 | VMO/VMAR 零拷贝 IPC，共享写、生命周期、rollback | 完整用户 mmap/pager 或所有权限组合 |
| phase2及续作 | 静态ET_EXEC、CPL3抢占、argv/envp、多ELF固定亲和并发、回收/畸形拒绝 | 无迁移、通用Linux兼容、替换式execve、动态链接或完整NX |
| phase3 | 内存压力管线、真实匿名页内容、COW/snapshot/checksum/scrub | 完整 APFS/Btrfs/XFS/生产无限卷 |
| phase4 | IRQ→用户 driver channel，背压/合并、阻塞唤醒、peer-close/offline/crash 隔离 | 所有硬件驱动用户态化、PCI/DMA runtime 完整产品 |
| phase5及P1续作 | lo/NIC UDP、DHCP、主动TCP用户ABI/RTO、IRQ后台RX | 无完整BSD socket、TCP服务端、拥塞控制、完整路由/NAT |
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
| M5及续作 | 默认product、CPL3 init/shell、run/&/-i/kill、安装盘冷启动，debug独立 |
| M6 | product/debug 清晰、状态横幅、ACPI shutdown/safe-hlt、UEFI 文档 |
| M7 | AHCI/SATA 同步读写/restore、COW 根、virtio→AHCI→RAM |
| M8 | e1000/e1000e、静态 IP、ARP/ICMP、环复用、virtio→e1000 首选 NIC |

| 续作 | 当前交付与边界 |
|---|---|
| P0a/P0b | buddy SMP一致性soak；BSP/AP共用PIT/TSC标定，周期tick目标1ms |
| P1a | 用户文件syscall、全范围copyin/out、持久根跨启动内容校验 |
| P1b/P1c | NIC UDP≤1472B、后台入队、DHCP ACK或静态fallback；无续租产品化 |
| P1d/RTO | 主动TCP、用户syscall26–30，SYN/数据/FIN有限重传及失败回收 |
| 用户进程 | argv/envp、同CPU阻塞现场隔离、双ELF overlap、off-CPU reap、run& |
| 产品控制 | 用户PID1/shell、CHILD kill、5+2秒关机等待/应急FORCE |
| 磁盘安装 | 宿主blank-only BIOS/UEFI安装；AHCI/virtio无ISO两次冷启动，不覆盖未知盘 |
| 外部SDK/示例 | GCC crt/链接脚本、离线导入；hello/nc/cat/ls/http-get |
| RX IRQ | e1000/virtio-net真实INTx、deferred wake、worker BLOCKED；未知/冲突路由fallback |

### 9.2 最新已观察到的验收证据

本次仅更新文档，没有重新编译或启动虚拟机。以下为先前实际运行的记录，
并已读取当前工作区仍存在的最新日志核对；不存在的旧日志只按阶段文档引用。

| 最新记录 | 已观察到的证据 |
|---|---|
| `/tmp/vnetirq-hosted.log`、`vnetirq-single.log`、`vnetirq-numa.log` | hosted/单节点/双NUMA全量PASS；vnet IRQ硬件delta=4、IRQ入队UDP=3、payload=1730 |
| `/tmp/vnetirq-e1000.log`、`vnetirq-fallback.log` | e1000独立IRQ回归、q35明确IRQ skip+轮询fallback，全量PASS |
| `/tmp/vnetirq-product.log` | 无ISO virtio盘、DHCP ACK、nc tx/rx各16B、exit0/reap、cat和关机 |
| `/tmp/httpget-hosted.log`、`httpget-debug.log` | hosted与QEMU debug全量PASS |
| `/tmp/httpget-product.log` | Python HTTP服务200，54B正文和5124B正文预览1024B均exit0；404 exit4、非法path exit2 |
| `/tmp/examples-product.log`、`examples-cold.log` | 无ISO盘四示例；nc 21B往返；冷启动读回73B持久文件，无NIC失败后仍可关机 |

M8完成时的hosted、e1000、e1000e、双NIC、82545EM双节点、无NIC日志
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
| 恶意应用隔离 | 文件/网络/控制ABI已有copyin/out，但用户fault恢复与完整接口覆盖仍不足；不承诺任意不可信ELF |
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
run /bin/hello-sdk.elf first second
run /bin/ls.elf /bin
run /bin/cat.elf /example.txt
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

已有磁盘可继续作为ISO启动的数据根；完整从盘启动另用安装器，**不要把已有数据盘传给格式化工具**：

```bash
make install-disk DISK_IMAGE=/tmp/capmix-new-system.img
qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -drive file=/tmp/capmix-new-system.img,if=virtio,format=raw \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -serial stdio -display gtk -no-reboot
```

无 `-cdrom`；关机后同命令冷启动，检查从盘读取`/sbin/init.elf`与`root=disk`。
安装器仅接受新路径或全零普通镜像，拒绝已安装/非空未知盘；运行期不自动覆盖旧shell。

宿主 `python3 scripts/tcp-echo.py` 后可 `run -i /bin/nc.elf 10.0.2.2 9998`。
宿主 `python3 -m http.server 8000 --bind 127.0.0.1` 后可
`run /bin/http-get.elf 10.0.2.2 8000 /`；只用`run`，不需交互控制台授权。
外部程序构建/离线导入及请求限制见 [SDK](EXTERNAL_ELF.md)、[示例](EXAMPLES.md)。

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

启用实际UDP/TCP echo与RTO丢包测试时，先启动宿主UDP/TCP echo服务，
使用 `boot/limine-tcp-rto-selftest.conf` 并添加实际NIC参数；确认
`irq rx verified`、`tcp rto loss recovery`、用户TCP ELF与全量PASS。
该配置依赖服务，不能把连不上服务改算skip。详细命令见 [TCP_RTO](TCP_RTO.md)。

## 12. 后续发展的上限与优先事项

### 12.1 设计路线可以继续走向什么

现有骨架可在已经具备的并发用户进程、持续网络收发和磁盘启动基础上，
继续完善真实用户驱动服务、资源控制与隔离VM。这需要实现层和 ABI 持续演进，
而不是单纯把容量宏改大或加几个 syscall 编号。

| 优先方向 | 要补的关键工作 | 完成前的限制 |
|---|---|---|
| 多用户进程/SMP | CPU迁移/负载均衡、容量扩展与更多退出同步压力覆盖 | 已支持多个 ELF、固定亲和与双 ELF overlap；尚无用户迁移 |
| 用户SDK/ABI | 扩展libc/VMO/channel/poll接口与ABI版本管理 | 已有GCC SDK、argv、文件/网络syscall；可选Linux L0/L1文件子集；无通用Linux兼容或完整POSIX |
| 完整故障隔离 | user page fault 转进程失败、异常栈/信号/恢复、更多负面测试 | 不能保证任意坏 ELF 只伤自己 |
| 用户服务管理 | 在现有用户init/shell之上扩展console/input服务、fd/IPC及服务生命周期 | 已是静态用户 ELF；尚无动态链接/完整服务管理 |
| 驱动 runtime | DMA/IOMMU 授权、设备资源对象、真实 IRQ/完成与用户服务 | 当前硬件驱动是内核最小实现 |
| 网络产品化 | TCP流控/拥塞、listen/accept、邻居到期、路由、DHCP续租及平台IRQ路由扩展 | 已有NIC IRQ、UDP/TCP用户ABI、有限RTO和HTTP示例；仍非完整互联网协议栈 |
| 文件系统规模 | 可变卷/扩容、更多寻址、并发元数据、断电/恢复覆盖 | 旧盘base0或安装盘base2048的8MiB卷，自有小COWFS |
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
| cap/IPC | [cap.h](../include/capmix/cap.h)、[ipc.h](../include/capmix/ipc.h)、`cap/`、`ipc/channel.c` | [phase1](history/PHASE1.md) |
| VMO/VMAR | [vm.h](../include/capmix/vm.h)、`mm/vm.c` | [phase1](history/PHASE1.md) |
| paging/KPTI | `arch/x86_64/paging.c`、入口汇编 | [phase0](history/PHASE0.md) |
| NUMA | [numa.h](../include/capmix/numa.h)、`mm/numa.c`、`mm/buddy.c` | [NUMA](NUMA.md) |
| ELF/ABI | [proc.h](../include/capmix/proc.h)、[syscall.h](../include/capmix/syscall.h)、`proc/elf.c`、`proc/syscall.c` | [phase2](history/PHASE2.md) |
| init/shell/control | `proc/init.c`、`user/init_main.c`、`user/shell_main.c`、`proc/control.c` | [用户init/shell](USER_INIT_SHELL.md)、[kill/关机](KILL_SHUTDOWN.md) |
| FS/storage | [fs.h](../include/capmix/fs.h)、[io.h](../include/capmix/io.h)、`fs/`、`io/blk.c` | [M3](history/M3_PERSIST.md)、[M7](history/M7_AHCI.md) |
| IRQ/用户驱动 | `io/driverkit.c`、`io/irq.c`、`boot/phase4_test.c` | [phase4](history/PHASE4.md)，含 blocking continuation |
| 网络 | [net.h](../include/capmix/net.h)、[tcp.h](../include/capmix/tcp.h)、`net/` | [UDP](P1B_UDP_NIC.md)、[DHCP](P1C_DHCP.md)、[TCP用户ABI](USER_NETWORK_ABI.md)、[RTO](TCP_RTO.md)、[RX IRQ](NET_RX_IRQ.md) |
| 用户文件/参数/并发 | `proc/uaccess.c`、`proc/elf_args.c`、`proc/spawn.c` | [文件ABI](P1A_ELF_FILES.md)、[argv](ELF_ARGV.md)、[并发](ELF_CONCURRENCY.md) |
| buddy/timer | `mm/buddy.c`、`arch/x86_64/lapic.c` | [SMP soak](P0A_BUDDY_SMP.md)、[标定](P0B_TIMER.md) |
| 安装/SDK/示例 | `scripts/install-disk.py`、`scripts/copy-to-disk.py`、`sdk/`、`examples/` | [安装](INSTALL_DISK.md)、[外部ELF](EXTERNAL_ELF.md)、[示例](EXAMPLES.md) |
| SHA/政策 | [security.h](../include/capmix/security.h)、`security/`、`lib/sha256.c` | [phase6](history/PHASE6.md) |
| VMX/EPT | [virt.h](../include/capmix/virt.h)、`virt/hardware.c`、`virt/vmcs.c`、`virt/ept.c` | [phase7](history/PHASE7.md)、[phase9](history/PHASE9.md)、[EPT](EPT.md) |
| trace | [trace.h](../include/capmix/trace.h)、`obs/static_trace.c` | [phase8](history/PHASE8.md) |
| FB/PS2/power | `console/`、`arch/x86_64/power.c` | [M2](history/M2_CONSOLE.md)、[M6](history/M6_PRODUCT_BOOT.md) |

更新本总览时，应同时更新数值来源、ring3 whitelist、最新运行限制和验收
证据，不能只把状态表全部改成 PASS。阶段早期文档可能保留历史限制，
后续 continuation 与当前源码优先；参数/API 发生变化时必须重新核对。

磁盘安装续作已提供宿主 blank-only MBR/FAT32 工具、独立 CapMix 卷 offset 与 AHCI/virtio 无 ISO 冷启动。不是 guest 安装 GUI 或真实设备安装器；两次冷启动和拒绝覆盖步骤见 [INSTALL_DISK.md](INSTALL_DISK.md)。

### 用户态最小 HTTP 服务端续作

已补 listen/accept syscall40/41与 `/bin/httpd.elf`，支持通过宿主端口转发读取HTTP/1.0 200。
accept非阻塞，连接池32槽、监听backlog≤8；支持队列满拒绝及close/kill回收。
不做本机TCP回环、TLS、线程池或完整HTTP解析，详见 [HTTPD.md](HTTPD.md)。

### Linux ABI L0 双轨续作

新增可关闭的Linux静态persona，仅对明确标记的Linux静态x86-64 ET_EXEC启用独立
write/exit/arch_prctl分发、Linux auxv与FS基址上下文。产品默认关闭，原生工具不删除、不改名；
新原生软件建议`.faz`或`/bin/cm/`。不是任意静态glibc兼容层，详见 [LINUX_ABI_L0.md](LINUX_ABI_L0.md)。

### Linux ABI L1 文件续作

增加可关闭的文件 syscall 翻译、固定 Linux v6.6 stat/dirent64 布局与偏移断言。
仅承诺「定制 musl 静态 BusyBox 子集 + 有界 BSS 分配」的 echo/cat/ls；
每进程 64 KiB arena、标量整数 printf 配置，未实现 mmap/brk/fork/socket/动态链接。
产品默认关闭，原生 ABI 与已有 /bin 不删除不改名，详见 [LINUX_ABI_L1](LINUX_ABI_L1.md)。

### Linux ABI L2 内存与进程续作

可选 Linux persona 增加驻留 COW fork、有限 brk、wait4 和同 PID execve；
fork 提前扣 commit 并预留潜在副本物理页，COW 页不参与压缩/swap。Linux现支持AP固定执行位置及fork/wait/reap（见 [Linux SMP](LINUX_SMP.md)），
L2首版每镜像128页；[L5](LINUX_ABI_L5.md)已将Linux窗口扩到16MiB、映射额度扩到8MiB，原生仍128页。产品默认关闭。原生 fork 保持 NOTSUP，原生 ELF SMP 并发保留。
L1 的定制 BusyBox/64KiB BSS 分配器未替换，不代表通用 musl 或 sh 已可用；详见 [LINUX_ABI_L2](LINUX_ABI_L2.md)。

### Linux ABI L3 网络续作

默认关闭的 Linux persona 增加 IPv4 TCP/UDP socket 映射：主动连接、bind/listen/accept、
read/write/close 与六参数 sendto/recvfrom。复用原生能力与网络后端；共享 fd 的最后引用
负责关闭，fork/exec CLOEXEC 与回收可验证。仍无 DNS/TLS/通用 socket 兼容；
有界等待、4096 字节 TCP 单次 I/O、1472 字节 UDP 与不支持项详见 [LINUX_ABI_L3](LINUX_ABI_L3.md)。
原生 shell/httpd/nc/http-get 与文件 ABI 保留。

### Linux ABI L4a 解释器启动试验

在显式 Linux 与解释器开关下，内核可映射带 PT_INTERP 的固定 ET_EXEC 探针及
固定基址的 PIC ET_DYN 解释器；auxv 保留主程序 AT_ENTRY/PHDR、增加 AT_BASE，
实际先进入解释器再跳到主程序。spawn/替换式 execve、缺解释器旧镜像存活与回收有测试。
这只是启动交接：没有 PT_DYNAMIC、重定位、共享库、真实 musl/glibc ld.so、动态TLS；
固定地址不是ASLR。默认产品两层开关仍关闭，原生ABI保留。详见 [LINUX_ABI_L4](LINUX_ABI_L4.md)。

### Linux ABI L5 匿名内存续作

Linux persona支持有界eager PRIVATE匿名mmap/munmap，16MiB VA窗口、8MiB总payload额度。
COW reserve与backing各拥有独立commit页，分裂只转换所有权，退出/失败对称回收。
COW页仍驻留，不接压缩/swap。原生ABI与128页额度不变，产品默认关闭Linux。
固定未改libc/分配器的musl1.2.5静态malloc/mmap探针可用，不代表通用musl/glibc；
L1仍为定制BusyBox+BSS，L4a仍非真动态链接。详见[LINUX_ABI_L5](LINUX_ABI_L5.md)。

### 固定静态 BusyBox sh 验收

独立BusyBox1.36.0/musl1.2.5 hush子集支持已测的sh -c、变量/条件、子shell、
fork/exec/wait及有界capability pipe；补getppid和wait4 WUNTRACED选项。
产品默认仍关Linux；L1旧fixture及原生shell不替换。信号调用仍ENOSYS，不支持交互作业控制、
完整POSIX或通用glibc；窗口及commit沿用L5，详见[LINUX_BUSYBOX_SH](LINUX_BUSYBOX_SH.md)。

### 最小真实用户态动态链接

可选Linux+interp双开路径新增仓库自有ld-capmix.so.1：实际读取DT_NEEDED共享库、
解析SysV动态符号、执行RELATIVE/GLOB_DAT/JUMP_SLOT/64重定位后进入黄金main。
原L4a仍只是独立交接探针。TLS 已续作至有界 DSO initial-exec（见下节）；无构造器、完整libc或任意Linux动态生态，库暂缺W^X/RELRO；
默认双轨策略不变，详见[LINUX_ABI_LDSO](LINUX_ABI_LDSO.md)。

### Linux 动态路径的最小 TLS 续作

自有 ld.so 已支持启动时 DSO initial-exec TLS（Variant II、TPOFF64），
最多三个 TLS 库，每库 64KiB；主程序及解释器自身 PT_TLS 仍拒绝。
FS base 复用 L0 上下文保存/恢复，默认 Linux/解释器关闭。
不包含 DTV、dlopen/pthread 或通用 glibc TLS，详见 [LINUX_ABI_TLS](LINUX_ABI_TLS.md)。

### 真实动态加载的 W^X 与 RELRO 续作

真实 ld.so 路径默认以 RW+NX 装载 DSO，重定位后按段应用 RX/R/RW，
保护主程序、库及解释器的约定 GNU_RELRO 页；无 RELRO 旧程序可运行并有日志。
旧版解释器缺少 CapMixWX v1 协议 note 时在加载阶段拒绝，须显式更新磁盘解释器。
原生、静态 Linux 与 L4a 保留旧权限行为，不宣称全系统 W^X；
不注入无法隔离的故意 #PF。详见 [LINUX_ABI_WX_RELRO](LINUX_ABI_WX_RELRO.md)。

### TCP 连接池与 RX 窗口续作

全局 TCP 共享池为 32 槽，单监听器 backlog 最大 8；默认每槽 RX 16KiB，
编译可选 4–32KiB。静态驻留池约 534KiB，不重复收取匿名 commit；池满明确失败。
MSS、单待确认段、RTO、原生/Linux I/O 与 fd 上限不变。
新增池满/复用、8 路被动 payload、RX 环回及 8 路实网卡并发测试。
详见 [TCP_POOL](TCP_POOL.md)。

### 有界Linux线程续作

已有同CPU线程组clone、进程内FUTEX_WAIT/WAKE、独立FS与最后线程回收；
每组累计最多8个执行记录、固定同CPU；映射变更和fork/exec仍拒绝，fd仅允许[受控FILE/console dup/close](LINUX_THREADS.md#受控fd白名单与仍冻结范围)。
固定静态探针与完整边界见 [LINUX_THREADS](LINUX_THREADS.md)，不宣称通用pthread。

### Linux 单线程受控跨 CPU 迁移

新增 Linux 203 `sched_setaffinity` 的单目标、自身迁移子集；syscall 完整退栈后，
旧 CPU 调度器重建入口现场，目标 CPU 恢复用户寄存器与 FS/TLS。
线程组、fork 子进程/未回收子进程仍拒绝；单线程持socket/pipe可在返回安全点迁移，
同任务dup允许，外部同端点别名或在途引用返回EAGAIN，见 [迁移限制与锁序](LINUX_MIGRATION.md)。没有自动负载均衡。
跨 CPU 往返、失败回滚、迁移后真实 fork/COW 与页/commit 回收已在 QEMU 和 VBox
本轮验证。范围、禁止点与仓库证据见 [LINUX_MIGRATION.md](LINUX_MIGRATION.md)。

### 静态解释器展示品：MicroPython

Linux opt-in 产品路径可运行钉住的 MicroPython1.24.1 + musl1.2.5 静态标量子集，
支持打印、整数/大整数、最小脚本文件与退出码。固定256KiB GC、8KiB源码缓冲，
禁用浮点、线程、外部模块、JIT和REPL；不是通用Python/Linux兼容。
配方、哈希、限制和本轮证据见 [LINUX_MICROPYTHON.md](LINUX_MICROPYTHON.md)。
默认Linux仍关闭，原生/hush/ldso黄金保留。


### Hosts-only stub 补充

原生 `nc/http-get` 与 Linux 静态 HTTP 探针现可使用共享用户库读取 `/etc/hosts`，
支持别名、ASCII大小写匹配，未命中回退数字IPv4；无记录明确失败。
这不包含 DNS 网络客户端或 libc 全局 getaddrinfo 替换。接口、边界与测试见
[HOSTS_RESOLVER.md](HOSTS_RESOLVER.md)。
