# 现在什么是真的，什么还不是

按机制说话，不按阶段编号。

## 真的

| 机制 | 口径 |
|---|---|
| WKdm-lite | 4096 → 更短 blob，解压 memcmp 相等 |
| L2 | 读页压缩，原页还 buddy |
| L3 | blob 经 blk-mq 写 ramdisk（仍无 swap-in） |
| 4 级页表 walker | 分配并填写 PML4…PT；**不加载 CR3** |
| fiber 切换 | `switch.S` 8 个 callee-saved + rsp/rip |
| 调度接线 | `cm_sched_tick` → `cm_arch_ctx_swap`；entry 跑在线程 8K 栈上 |
| IPC 直切 | send 发现 recv waiter 时 swap 到对方 |
| Limine mmap/hhdm | 读 `.response`；有 HHDM 就喂 buddy（virt = phys + offset） |
| 死线程 | `cm_thread_reap` 还栈、槽位置 0 |
| 跨栈 RTT | `trace=off` 才是同特权级纯切换成本；hosted 无 MMU/ring 切换 |
| 观测税 | `trace=on` 与 `trace=off` 的差 |

## 仍然不是

- 抢占：无 PIT/APIC，tick 只在 `run_until_idle` 里被叫
- ring3 / sysret / 每进程 CR3
- 跨地址空间 IPC（无 CR3 重载、无 TLB/PCID）
- VMX/SVM/EPT、真 VFIO
- SHA-256 证书链
- swap-in / 缺页换回
- SLUB `kfree`（仍泄漏）
- 自建页表生效（walker 有，CR3 未加载；裸机分页仍靠 Limine）

## 跨 AS IPC 口径（2026-09-18）

`run_direct` 测到的 ~603 cycles/rtt 是 **ring0 跨地址空间**（2×CR3 + 4×IPC），不含 syscall/sysret。真正的 user→user IPC 还要叠加两次系统调用，现在没测。

obs tax 以 `trace=on` − `trace=off` 为准；rdtsc 实测约 32 cycles，不是 VM-exit。PV clock 不做。

## RTT 怎么读

hosted `trace=off` 的跨栈数字 = 同特权级、同地址空间、8 寄存器 fiber 切换。

不是 L4 上界也不是下界。缺：CR3/TLB、syscall/sysret。`ucontext` 已不用，不会再高估 SIGMASK/FPU。

QEMU + ring3 + 独立页表之后，那个数字才能当骨架生死线。
