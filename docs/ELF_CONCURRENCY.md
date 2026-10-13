# 静态 ELF 多进程并发

## 范围与容量

静态 ET_EXEC 的每个进程拥有独立 full/shadow 页表、用户栈、argv/envp、fd/能力句柄表，以及独立的内核入口栈槽。移除单 pending ELF；多个用户镜像可以同时处于 runnable/running/blocked 状态。保持原 KPTI full/shadow 策略、buddy 锁和时钟标定。

全局进程记录上限 `CM_MAX_PROCS=32`，包括 PID1 和 native/selftest 进程。因此产品只有 PID1 占用进程记录时，最多有 31 个 ELF 记录（退出但未回收的记录仍占槽）。任务、线程、能力、commit 和可用物理页可能使实际容量更低。shell 最多跟踪 8 个后台 ELF，外加 1 个前台 ELF。超过容量返回 `CM_ERR_NOSPC`；commit 不足仍返回原 `CM_ERR_OVERCOMMIT`。这不是无限进程系统。

## 亲和与发布

创建/最终回收由 BSP 控制。原生 `cm_posix_spawn` 新增可选 `cpu_set/cpu`：`cpu_set=0` 保持默认 BSP；设为 1 时固定在指定的 online CPU 编号上，不做用户线程迁移。非法 CPU 返回 `CM_ERR_INVAL`，不改变调用方 out_pid。产品 PID1 固定 BSP，shell 将成功创建的 ELF 按 online CPU round-robin 分配；只有一个 CPU 时是同 CPU 抢占/阻塞并发。

先加载/准备参数、创建 task/thread，再 release-publish 完整进程元数据；EEVDF 在 publish 前不选择该任务。AP 使用与 BSP 相同的调度入口，SYSCALL MSR 在各 CPU 首次执行 ELF 时初始化。

创建仍通过可信的内核 controller API；没有新增用户态 spawn ABI，也没有把 fork 改为可用。

## 入口与回收

每个 ELF 使用 16 KiB 独立静态入口栈，索引对应进程记录。32 个进程槽共 512 KiB，另保留原 8 CPU 的 128 KiB 入口栈用于既有路径。它们仍在原 supervisor-only `.entry.data` KPTI 窗口中，不从 buddy 逐次申请。复用槽前必须完成 off-CPU 回收，因此不会有动态入口栈页泄漏；固定常驻开销不会随运行次数增长。

SYSCALL frame 仍在该线程 native kernel stack 上。上下文切换（IF=0）同时保存/恢复 entry ABI、user_rsp、full/shadow CR3、退出标志和 SWAPGS 两侧状态；TSS.rsp0 指向被恢复 ELF 的私有入口栈。退出标志不再由不同 ELF 共享。native worker 迁移时 GS CPU 地址重绑定到目标 CPU；当前仍不提供 GS/TLS 用户 ABI。

线程分配预留与回收清空使用既有调度锁，避免 BSP 复用 AP 正在清空的线程槽。创建失败释放已准备的镜像、页表、commit 与未发布 task。退出清理 fd/网络/驱动/trace 能力；仅在 thread 已离开 CPU 并回收后销毁 image。`cm_proc_reap_elf` 保留退出记录，`cm_proc_release_elf`（BSP selftest 或产品 PID1）再回收独占 task/记录；外部 task 引用存在时返回 BUSY，避免悬空 capability。

日志区分：

- `exec reap: ... image/pages/commit reclaimed entry-stack inactive`
- `exec record reap: ... task/entry-stack slot recycled`

## Shell 语义

```
run /bin/concurrent.elf &
run /bin/concurrent.elf &
run /bin/argtest.elf foreground hello
poweroff
```

末尾独立 `&` token 使 run 在创建成功后立即返回提示符。argv 在发布前复制，后台程序不持有 shell 行缓冲指针；`&` 不传给程序。仍只支持空白分词，没有引号、管道、重定向、jobs/kill 或完整作业控制。

普通 run 等待自己的 ELF 退出/回收，同时回收已完成后台任务；其他后台进程继续运行。后台退出由 PID1 在输入循环、前台等待、关机等待时回收，记录 PID、exit 和 `reap=ok`。spawn/加载失败只返回错误，不产生后台 job；后台槽满不先启动一个无人跟踪的进程。

`exit`、`poweroff/shutdown` 都等待所有后台任务完成，再同步根文件系统。首版没有强制终止或超时：无限循环程序会使退出/关机等待持续，用户需运行能自行结束的 ELF。运行期 fault 隔离不是本轮新增功能。

## 自测与验收

`tests/concurrent_elf.S` 经 Makefile 生成 `build/concurrent.elf` 和嵌入数组；debug 写入 VFS `/bin/concurrent.elf`，产品仅在缺失时 provision。默认执行约 100 亿 TSC cycles（持续时间依 CPU 频率而定），便于后台手工验收；debug 将两个镜像分别改为标定频率的 200 ms，且赋不同私有 token。

自测首先耗尽剩余进程槽，验证容量失败后释放所有未执行镜像、页表/native 栈/commit 并比较 free/commit 基线。然后：

1. 两 ELF 固定 CPU 0/1（单 CPU 则都固定 CPU 0），读回**用户态** start/end TSC，断言 `max(start)<min(end)`；校验 CPL=3、heartbeat、不同 token、寄存器/栈、preempt 和 exit=0。多 CPU 表示跨 CPU 执行重叠；单 CPU 表示时间片交叠。
2. 驱动 ELF 固定 CPU 0，进入 syscall 24 真实 BLOCKED；另一 CPU 0 ELF 执行并 exit；然后注入虚拟 IRQ。原 syscall 恢复后校验 user_rsp 上的 canary、callee-save registers、返回值和消息，recv=1、CPL=3、exit=0。另一 ELF 的退出标志不能污染阻塞现场。
3. off-CPU 回收镜像/任务后，检查 free page 与 commit 基线。测试恢复其临时驱动对 toy block-device online 状态的影响，后续 IRQ/文件系统测试仍独立运行。

关键日志：`elf create/failure/reap verified`、`elf concurrent overlap verified`、`elf same-cpu blocked syscall isolation verified`、`elf concurrency selftest: PASS (0 failures)`。hosted 验证双镜像/容量/回收，明确跳过 ring3 execution/overlap，不假称执行用户 ELF。

debug 配置仍显式使用 `make iso LIMINE_CONFIG=boot/limine-selftest.conf`。网络回归用 `boot/limine-tcp-selftest.conf`，宿主启动 `scripts/tcp-echo.py` 与 `scripts/udp-echo.py`。默认 `make iso` 是产品 shell。

新版 QEMU 的双节点配置示例（旧 `-numa node,mem=256M` 可能不受支持）：

```
-object memory-backend-ram,id=m0,size=256M \
-object memory-backend-ram,id=m1,size=256M \
-numa node,nodeid=0,cpus=0-1,memdev=m0 \
-numa node,nodeid=1,cpus=2-3,memdev=m1
```

入口/上下文改动涉及 `syscall.S`、`ctx.c`、TSS.rsp0 与 usermode IRET 边界，属于 AGENTS.md 标注的人审关键路径。本轮没有修改 VMX/EPT/TRACE、NUMA、buddy 锁、timer calibration 或 shell 命令集（只扩展 run 的后台语义）。
