# CapMix

从零写的 **capability 混合内核**，选型严格对齐《从 0 写操作系统内核：混合选型清单》。

> 地基用 capability（seL4/Zircon），IPC 用 L4 的快速同步消息，
> 文件/网络留在内核（XNU 教训），驱动赶去用户态（DriverKit），
> 内存走 Windows no-overcommit + macOS 四级压缩管线 + Linux OOM 兜底，
> 调度走 EEVDF + QoS + turnstile，观测从第 1 阶段就把 trace 埋进去。

当前状态：**phase0–9、NUMA/EPT 与 M2–M8 已有可运行验收记录**；默认 ISO 进入产品 init/shell，`make test` 和独立 selftest ISO 执行回归。

静态 ELF argv/envp、初始栈与最小 crt 见 [ELF_ARGV](docs/ELF_ARGV.md)。

用户 ELF TCP syscall 26–30 与 shell 测试方式见 [用户网络 ABI](docs/USER_NETWORK_ABI.md)。

最小主动 TCP 的内核接口、真实 echo 验收和限制见 [P1d 文档](docs/P1D_TCP.md)。

最小 DHCP、静态回退和配置说明见 [P1c 文档](docs/P1C_DHCP.md)。

实网卡 UDP 与后台 RX 的接口、限制和 echo 验收命令见 [P1b 文档](docs/P1B_UDP_NIC.md)。

详细说明见 [内核总览：理念、接口、上限/下限与当前状态](docs/KERNEL_OVERVIEW.zh-CN.md)。其中明确区分已实现机制、用户可调用接口、容量限制和后续目标。

## 现在能跑什么

```
make test
```

会在 Linux 用户态拉起内核子系统（不需要 QEMU），验证：

1. 伙伴分配 / 合并、SLUB 小对象、**no-overcommit** 超限拒绝
2. 四级内存压力水位
3. capability 对象 + 句柄安装 / 转移 / 释放
4. 双向 channel、L4 风格同步 handoff
5. EEVDF + QoS band + 后台线程
6. 跨任务 IPC 往返，**每条消息带 trace_id**
7. 1 万轮 IPC RTT 微基准（骨架生死指标）

## 目录

```
include/capmix/   内核接口、共享结构与 syscall 编号
arch/x86_64/      x86_64 页表、入口、ring3、中断、VMX 汇编
boot/             宿主机/Limine 入口、产品/debug 分流与测试
mm/               buddy / slab / commit / pressure
cap/              对象与句柄
ipc/              channel + sync IPC
sched/            EEVDF + QoS + turnstile
obs/              ETW 风格环形事件 + trace_id
fs/ io/ security/ VFS/COW、设备驱动、网络与安全机制
docs/            总览、阶段记录和产品里程碑说明
```

## 早期实施顺序（当前状态以总览为准）

| 阶段 | 内容 | 关键验证 |
|---|---|---|
| 0 | boot + 串口 + 物理页 + 页表 | 能打印 ← **hosted 已通过** |
| 1 | capability IPC + 线程 + EEVDF | **IPC RTT 测出来** ← **hosted 已通过** |
| 2 | 用户态 + syscall + 第一个进程 | hello world |
| 3 | 压力管线 + VFS + 一个 COW 卷 | 压满不死 |
| 4 | 中断上下半 + 用户态驱动 + 块层 | 驱动崩、内核不崩 |
| 5 | 网络 + eBPF/XDP | |
| 6 | 沙箱 / 签名 / SIP | |
| 7 | KVM + job/cgroup | |
| 8 | 观测补全（埋点已从阶段 1 开始） | |

## 构建

依赖：`gcc`、`make`。

```
make test      # 推荐，当前主路径
make clean
```

`make kernel` 生成 freestanding ELF，`make iso` 打包默认产品 ISO。
完整 debug 镜像使用 `make iso LIMINE_CONFIG=boot/limine-selftest.conf`；运行方法与硬件限制见总览。

## 语言选择

清单允许 C 或 Rust framekernel。本树用 **C11** 把对象模型先钉死，
原因：现成工具链即可跑通阶段 1 的 IPC/调度验收。
Rust 安全边界（Asterinas 路线）可以在 `cap/` 与 `mm/` 稳定后以 frame
层替换 unsafe 部分，不推翻骨架。

静态 ELF 多进程并发、固定 CPU 亲和、`run ... &` 与退出/关机等待语义：
[ELF_CONCURRENCY.md](docs/ELF_CONCURRENCY.md)。

默认产品已使用 CPL=3 的 `/sbin/init.elf` 与 `/bin/shell.elf`；启动 capability、用户控制 ABI 与限制见 [USER_INIT_SHELL.md](docs/USER_INIT_SHELL.md)。
