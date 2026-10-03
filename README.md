# CapMix

从零写的 **capability 混合内核**，选型严格对齐《从 0 写操作系统内核：混合选型清单》。

> 地基用 capability（seL4/Zircon），IPC 用 L4 的快速同步消息，
> 文件/网络留在内核（XNU 教训），驱动赶去用户态（DriverKit），
> 内存走 Windows no-overcommit + macOS 四级压缩管线 + Linux OOM 兜底，
> 调度走 EEVDF + QoS + turnstile，观测从第 1 阶段就把 trace 埋进去。

当前完成度：**阶段 0–8 可运行骨架**（宿主机 `make test`）。
裸机 QEMU 步骤见 `docs/QEMU.md`。

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
include/capmix/   公共 ABI（按层拆头文件）
arch/x86_64/      串口、链接脚本
boot/             宿主机入口 + 将来的 Limine kmain
mm/               buddy / slab / commit / pressure
cap/              对象与句柄
ipc/              channel + sync IPC
sched/            EEVDF + QoS + turnstile
obs/              ETW 风格环形事件 + trace_id
fs/ io/ security/ 阶段 3–6 的接口占位
docs/DESIGN.md    与选型清单的逐条对照
```

## 实施顺序（来自清单，未改）

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

裸机 ELF（`make kernel`）需要 freestanding 运行时与 Limine 镜像打包，
作为阶段 0 的下一步，不阻塞核心原语演进。

## 语言选择

清单允许 C 或 Rust framekernel。本树用 **C11** 把对象模型先钉死，
原因：现成工具链即可跑通阶段 1 的 IPC/调度验收。
Rust 安全边界（Asterinas 路线）可以在 `cap/` 与 `mm/` 稳定后以 frame
层替换 unsafe 部分，不推翻骨架。
