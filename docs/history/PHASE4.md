# 阶段 4：IRQ capability 通道与用户驱动接收

> 历史阶段记录：保留当时实现与验收边界；后续功能可能已扩展。当前状态见
> [内核总览](../KERNEL_OVERVIEW.zh-CN.md)，操作测试见 [VirtualBox手册](../VIRTUALBOX_TESTING.zh-CN.md)。

## 注册与事件路径

`cm_drv_bind_channel(name, irq, pid, send_hid, recv_hid)` 是内核协调器使用的注册
API。进程必须存在、存活并有有效主线程；两个句柄必须属于该进程，指向同一
channel 的相反 endpoint。发送端要求 WRITE|DUPLICATE，接收端要求 READ。
注册后框架在独立 `driver-irq-broker` task 中持有 WRITE-only 副本；调用方可关闭
原发送句柄。IRQ 已被占用、进程已有在线绑定或句柄权限/配对错误时注册失败，
撤销已建立的 broker 引用，不覆盖原 handler。

现有 `cm_drv_bind()` 保留为兼容包装：自动创建 channel，再调用上述注册 API。
注册入口本轮由内核 bootstrap 协调，不新增用户态注册 syscall。

路径为：

```text
virtual IRQ / existing IRQ inject
    -> fixed softirq queue (top half)
    -> cm_irq_softirq_flush()
    -> driver bottom half
    -> broker WRITE capability / deferred channel send
    -> driver endpoint / restricted IPC_RECV
```

Top half 只做计数和固定队列写入，不分配、不记录日志、不创建 trace、不调用
IPC 或调度。短 spinlock + 本地 IRQ masking 保护注册表及队列。队列最多 64 项，
溢出的事件明确丢弃并计入 `cm_irq_overflow_count()`。

队列项保留 handler、arg 和注册 generation。release/rebind 后旧项按 stale 丢弃，
计入 `cm_irq_stale_count()`，不会调用新 handler。flush 采用有限队列快照预算，
回调在 IRQ 队列锁外执行。driver descriptor 为固定存储，不释放回调 arg。

Channel 投递使用 `cm_ipc_send_deferred()`：复用既有排队、权限及唤醒，但禁止
直接切换到 peer。普通 send/recv 的行为和 VMO capability 传递语义保持原样。
用户驱动代码不在 IRQ bottom half 的调用栈执行。

## 消息与背压

`CM_DRV_IRQ_EVENT = 0x495251`，message len=24、nhandles=0，data 内容：

```c
typedef struct {
    uint32_t irq, driver_id;
    uint64_t sequence, count;
} cm_drv_irq_packet_t;
```

sequence 是该包所含的首个 bottom-half 事件序号，count 是合并事件数量。
channel 满时保留 pending count，后续事件合并到 pending，不阻塞 bottom half。
每次 softirq flush 结束都会重试；没有新 top-half 工作时 flush 也能重试。
因此 top queue 接受的事件不会仅因 channel 满而丢失；top queue 自身溢出仍会丢失。
不支持每个硬件边沿的独立时间戳、IRQ ACK 协议或 DMA 描述符。

`delivered` 计数 channel 包数，`received` 计数成功 recv 包数，`irqs` 是进入 driver
bottom half 的事件数，`pending` 是未投递事件数量，`backpressure` 是满队列尝试数。
`dropped` 包含离线时取消的 pending 事件，不是全系统 IRQ 丢失总数。

## 用户态接收与生命期

只有已注册在线驱动的 ELF 进程可使用 `CM_SYS_IPC_RECV=7` 的新分支。它校验：

- 缓冲区完整覆盖已加载、可读写的 ELF 用户页；拒绝内核地址、RX text 和缺页。
- 句柄必须是该注册的接收 endpoint；数值相同但 object ID 已变化也不能复用注册。
- 收到消息采用非阻塞 poll，空队列返回 AGAIN，不进入 ELF 阻塞/恢复路径。

其他 ELF 的 syscall 白名单不变。ELF 加载器、BSP 串行执行/抢占限制、KPTI/VMO
设计与入口汇编均未修改。首版测试在进入驱动前注入并 flush 虚拟 IRQ；没有
自动 softirq worker，也没有实现 serial ELF 等待未来 IRQ 的阻塞模型。

驱动 EXIT、进程 kill、`cm_drv_crash()` 或检测到接收 capability 关闭时，框架将
设备置 offline、解除 IRQ、释放 broker endpoint，取消 pending 投递。进程退出/
模拟 crash 时同时关闭其 task 句柄，释放排队消息。ELF 镜像仍遵守阶段 2 的
off-CPU 回收规则，不在 syscall/crash 路径中提前归还镜像 commit。

当前只有一个 ramdisk 的全局 online 状态，绑定描述符最多 16 项。离线描述符
保留诊断记录；不是多设备驱动重启管理器。显式注册新的存活 handler 会重新
attach ramdisk，之后该 handler 死亡仍使它 offline；没有后台自动恢复。
crash helper 仍是模拟 SIGSEGV 式进程死亡，不声称已经实现真实用户 #PF 隔离。

## selftest

`tests/blockd_elf.S` 复用阶段 2 的静态链接布局，构建成 `build/blockd.elf`，经
`scripts/embed-elf.py` 内嵌，再由 selftest 经 VFS 写入 `/bin/blockd.elf`。
进程经既有 path-based posix_spawn 启动。测试 bootstrap 创建、注册 channel，
在镜像配置区写入 receive handle 和 driver ID，再注入 IRQ 12。

真实用户代码在 CPL=3：

1. 验证内核指针、RX 接收缓冲和错误 endpoint 被拒绝。
2. 接收包，校验 opcode、length、IRQ、driver ID、sequence 和 count。
3. 将 recv count 与 CS 的 CPL 保存到 BSS，验证空队列返回 AGAIN。
4. 打印 `driver irq recv`，exit 0。

hosted 验证同一 channel 的收发和生命周期，明确不执行 CPL=3。
额外测试包括权限/配对拒绝、IRQ 占用、重复绑定、67 次注入中的 3 次 top overflow、
64 个事件合并为 9 个包（最后包 count=56）、空 flush 重试、关闭/复用句柄拒绝、
显式 rebind、在线驱动模拟 crash、stale IRQ 丢弃，以及离线 I/O 和 buddy 可用性。
crash 后同时断言同步 blk helper 和 uring completion 返回 DEAD，kernel_alive=1。

关键日志：

```text
drv irq registered
drv irq delivered
drv irq recv
user[...]: driver irq recv
driver irq user ok: com.capmix.blockd CPL=3 recv_count=1 exit=0
driver exit isolation ok
driver irq backpressure ok
driver irq peer-close ok
drv crash isolated
driver crash isolation ok: task dead device offline I/O=DEAD buddy usable kernel_alive=1
phase4 selftest: PASS (0 failures)
```

默认 hosted 与四核 KVM 实际日志均为全量 `selftest result: PASS (0 failures)`。
四核 KVM 实际执行 blockd ELF，记录 CPL=3 接收成功；phase0/1/2 与 phase3 压力/COW
保持通过。内核构建没有新增 warning/error。

```sh
make test
make kernel 2>&1 | grep -iE 'warn|error'
make iso
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -serial stdio -display none \
  -no-reboot -d cpu_reset 2>&1 | tee /tmp/capmix_run.log
```

## Blocking IRQ receive (phase4 continuation)

`CM_SYS_DRV_RECV_WAIT` (24) takes `(recv_handle, writable cm_ipc_msg_t*, 0, 0)`.
It is limited to the registered driver's main thread on BSP, retaining the
existing serial ELF execution model. Syscall 7 remains a nonblocking driver
receive. The wait registers a receiver and marks the thread BLOCKED atomically
under the channel lock, then drops all capability/driver locks before yielding.
Deferred channel delivery wakes the thread; on resume the driver lifecycle and
endpoint identity are rechecked before dequeuing and incrementing recv_count.
An owned endpoint reference protects cancellation and close from stale pointers.

There is no automatic timeout or POSIX signal interruption in this first version.
The kernel/controller may call `cm_drv_wait_cancel(pid)` to wake with `AGAIN`;
receiver close detected by the bottom-half retry/lifecycle check returns `DEAD`.
Offline/crash cancels the waiter, releases its reference and discards pending IRQs.
A crashed blocked thread is marked DEAD before cancellation and is never resumed.

`tests/blockd_wait_elf.S` is linked with the existing hello linker script and
embedded by Makefile as `build/blockd_wait_elf.inc`. The selftest writes this
fixture to COWFS `/bin/blockd.elf`, starts it through the existing ELF spawn path,
then observes BLOCKED before injecting IRQ 12. It verifies top-half does not wake,
bottom-half wakes, CPL=3 receives the expected packet, and exit remains isolated.
Hosted execution checks the same sleep/wake lifecycle with native test threads;
ring3 execution is explicitly skipped there. Cancellation, receiver close and
crash while blocked are tested separately. Each scheduling phase has at most 64
steps; failures crash/reap the driver rather than leaving a sleeping test behind.

Expected log sequence: `drv wait sleeping` -> `drv irq delivered` ->
`drv wait awakened` -> `drv blocking recv`; ring3 also prints
`driver irq wake ok`. Additional checks report `driver wait lifecycle verified`
(cancel/peer-close) and `driver blocked crash isolation ok`, followed by the
existing `driver crash isolation ok` and `kernel_alive=1`.

后续 ELF runner 已支持多镜像固定亲和并发；阻塞 IRQ recv 在另一同 CPU ELF
退出后恢复的现场隔离测试见 [ELF_CONCURRENCY.md](../ELF_CONCURRENCY.md)。
