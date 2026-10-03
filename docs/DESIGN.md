# CapMix 与选型清单的逐层对照

原则保持不变：**骨架只选一次，其余每层独立取三家最优，先读冲突清单。**

## 第 0 层 骨架 — 已锁定

| 清单要求 | CapMix 落地 |
|---|---|
| 一等公民 capability | `cap/object.c` + `cap/handle.c`，句柄带 rights |
| 文件/网络留内核 | `fs/` `io/` 在内核地址空间（阶段 3–5 填肉） |
| 驱动赶用户态 | `io/uring.c` 注释与 DriverKit 路线；内核只留薄总线 |
| IPC 必须快 | `ipc/channel.c` 同步 rendezvous + 对端已等待则直接 handoff |
| 观测从第 0 天埋 | 每条 IPC 消息强制 `trace_id` |

未选纯宏内核、未选纯微内核、未把 FS 做成用户态服务器。

## 第 1 层 内存

| 清单 | 文件 |
|---|---|
| 伙伴系统 + SLUB | `mm/buddy.c` `mm/slab.c` |
| no-overcommit + 强制 commit | `mm/commit.c`（超限返回 `CM_ERR_OVERCOMMIT`） |
| 8 态页 | `cm_page_state_t` |
| 压力 L1–L4 | `mm/pressure.c` |
| THP 不 always | `CM_THP_ALWAYS 0` |

## 第 2 层 调度

| 清单 | 文件 |
|---|---|
| EEVDF | `sched/eevdf.c`（lag / eligible / vdeadline） |
| QoS band | `cm_qos_t` |
| turnstile 沿 IPC 链抬升 | `cm_turnstile_propagate` |
| 线程为调度单位 | `cm_thread_t` 挂在 `cm_task_t` 下 |
| aging 防 BACKGROUND 饿死 | `g_aging[]`，冲突 #8 |

## 第 3 层 IPC

同步发送、小消息就地拷贝、handoff 唤醒、trace 贯穿。
POSIX fd 兼容层（fdio）刻意不做原生统一——冲突 #3。

## 第 2 阶段进程（已落地）

posix_spawn 一次完成 commit 预扣、VMAR/VMO cap、用户线程；fork 返回 NOTSUP。

## 第 4–8 层

接口已经按清单形状切开（`fs.h` COW / `io.h` uring / `security.h` Seatbelt+SIP /
`sched` 里 job/coalition 字段 / obs 的 ETW 环）。实现按 `建议实施顺序` 往下填。

## 冲突清单落实

1. 不把 FS 做成微内核服务器。
2. 主路径是 `posix_spawn`；`fork` 只做兼容并拒绝。
3. capability 与 POSIX 分层，不合并原语。
4. 文件系统走 COW，预留 `nocow`。
5. eBPF 预留关停（阶段 5）。
6. THP 默认 madvise。
7. 观测是 `CM_OBJ_TRACE` capability，不是全局开关。
8. QoS + aging + 保底（aging 计数器已就位）。
