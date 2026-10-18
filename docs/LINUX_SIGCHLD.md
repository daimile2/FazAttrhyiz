# Linux SIGCHLD 最小投递

本轮只有 SIGCHLD 的合并 pending 位、默认处置与自动回收子集。
没有用户信号栈、trampoline 或任意 handler 回调；不是完整 Linux/POSIX 信号框架。
Linux persona 仍默认关闭，原生 CapMix ABI 不增加信号 syscall。

## 处置与可观察接口

面向 Linux `fork` 创建的子进程：发布退出状态后，在下一个 Linux syscall、阻塞 wait4 的重查、
或产品 PID1 的回收轮询中处理事件。生命周期 gate 忙时延迟重试，不在退出栈上释放镜像。
信号状态在父进程中，标准 SIGCHLD 合并为一个 bit，不是逐事件队列；退出状态仍按子进程分别保存。

| SIGCHLD 处置 | 阻塞时 | 未阻塞或解除阻塞 | wait4 |
|---|---|---|---|
| SIG_DFL=0 | pending bit 16 | 默认忽略并清 pending，不执行 handler | 保留退出记录，可获取状态 |
| 显式 SIG_IGN=1 | 不留 pending | 忽略 | 子进程退出后自动回收，ECHILD |
| SIG_DFL + SA_NOCLDWAIT=2 | pending bit 16 | 默认忽略 | 子进程退出后自动回收，ECHILD |
| 用户 handler 地址 | EOPNOTSUPP | EOPNOTSUPP | 不改变已配置处置 |

默认忽略与显式 SIG_IGN 的回收行为不同，不能把两者都处理成“丢弃退出状态”。
Linux 定义参考：[sigaction(2)](https://man7.org/linux/man-pages/man2/sigaction.2.html)、
[wait(2)](https://man7.org/linux/man-pages/man2/wait.2.html)。这里只承诺表内子集。

- `rt_sigaction` nr=13：SIGCHLD 接受 DFL/IGN、SA_NOCLDWAIT 与已有 SA_RESTORER 元数据。
  SA_RESTORER 指针不调用；action mask 非零、其他 flags 或 handler 地址返回 -95。
  非 SIGCHLD 仍保留原有 DFL/IGN 元数据行为，不产生新的投递能力。
- `rt_sigprocmask` nr=14：kernel sigset 大小 8；SIGKILL/SIGSTOP 不可阻塞。
  解除阻塞执行默认忽略，不建立用户调用帧。
- `rt_sigpending` nr=127：参数 `(uint64_t *set, size_t=8)`，返回当前阻塞的共享 pending SIGCHLD。
  查询不消费 pending；非法指针 -14、错误 sigset 大小 -22。
- `wait4` nr=61：默认处置保留原有状态编码；IGN/NOCLDWAIT 在子进程仍运行时可等待，
  退出后返回 -10（ECHILD）。WNOHANG 在仍有运行子进程时返回 0。
  生命周期 gate 暂忙可返回 -11（EAGAIN）；阻塞 wait4 则让出后重查。
- `rt_sigreturn` nr=15、sigsuspend/sigtimedwait/signalfd、kill 信号发送等没有因此实现；
  未实现项保持 ENOSYS 并计数，不以成功替代。

## 回收与有界限制

事件扫描、自动回收和 wait4 领取退出记录使用同一生命周期 gate，防止两个 CPU 同时取走记录。
先以 release/acquire 观察 `exited`，再等线程离开 CPU、入口栈不活动，才释放页表、COW reserve、commit、
task 和进程记录。自动回收不是退出线程在自己的栈上销毁自身。
如果 gate 忙、线程尚在 CPU 或 task 仍被外部引用，稍后安全点重试。
“无 zombie”指没有可供 wait4 领取的退出状态；内部回收可能短暂延迟。

首版限制：

- 必须在 fork 前设置 SIGCHLD 处置。父进程存在尚未回收的 fork 子进程时，改变处置返回 -16（EBUSY）。
  查询或重复设置同一策略允许；避免为动态处置变化引入另一套退出竞态。
- threaded group 中不允许设置 SIGCHLD 处置（-95），不承诺多线程信号路由/公平唤醒；
  clone 线程结束不单独产生 SIGCHLD，fork 子进程的线程组整体结束才产生一次。
- fork 继承 mask/处置，pending 不继承；现有 exec 在无用户 handler 的前提下保留该处置元数据。
- 只处理终止事件；没有 STOP/CONT 通知、siginfo payload、异步用户 handler、SA_RESTART 语义或信号导致的 syscall 中断。
- 通知在上述安全点处理，不承诺墙钟延迟、独立信号 worker 或空闲时立即回收。
- 不改 CapMix shell 的 run&/kill/poweroff 协议；这些不是 POSIX 信号实现。

## hush 配置评估

固定 BusyBox 1.36.0 的 `user/linux/busybox-sh.config` 仍关闭：
`HUSH_INTERACTIVE`、`HUSH_JOB`、`HUSH_WAIT`、`HUSH_TRAP`、`HUSH_KILL`，不重编黄金二进制。
源码 hush.c 会安装 SIGCHLD handler 更新内部计数，交互式作业还依赖进程组、控制终端、STOP/CONT、
sigsuspend 等。pending bit 加 wait4 **不足以开启 HUSH_JOB**，不能据此宣称 hush 作业控制可用。
现有 `busybox-sh-signals.patch` 的失败检查仍需保留：handler 安装失败不得读取未初始化 old action。
本轮没有去掉补丁、放宽 handler 拒绝或启用 JOB/TRAP。
前台简单命令/管道/子 shell 沿用 wait4，既有 hush 黄金测试照常回归。

## 自测

`boot/linux_sigchld_test.c` 的 hosted/freestanding 机制用例验证：
默认可 wait、阻塞 pending、显式 IGN、NOCLDWAIT、ECHILD、安全离栈回收、commit/页数对称。
`tests/linux_sigchld.c` 是无 libc 的 Linux x86-64 ring3 探针，真实 fork 两次验证合并、
wait4 状态 41/42、解除阻塞清位、IGN/NOCLDWAIT 自动回收、活动子进程下 EBUSY、
坏指针/大小错误、用户 handler 拒绝以及 sigreturn=-ENOSYS（恰好计数一次）。

```sh
make test
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
# 用现有 QEMU serial 路径运行；不配置宿主网络 peer 时可 -nic none，wire 用例明确 skip。
python3 tests/check_sigchld.py /path/to/serial.log
# hosted:
python3 tests/check_sigchld.py --hosted --interp-off /path/to/hosted.log
```

日志关键字：`linux SIGCHLD:`、`linux SIGCHLD autoreap:`、`SIGCHLD mechanism:`、
`SIGCHLD guest:`、`SIGCHLD selftest: PASS (0 failures)`。
检查脚本同时调用既有黄金链检查器，要求全量 `selftest result: PASS (0 failures)`；
不将 handler 拒绝、未实现 syscall 或 hosted 没执行 ring3 当作功能成功。

### 2026-10-18 环境矩阵

| 环境 | 结果与日志 |
|---|---|
| hosted，Linux 开、interp 关 | 全量 0 failures；[日志](../tests/evidence/logs/sigchld-hosted-on.log)、[黄金链检查](../tests/evidence/logs/sigchld-hosted-on-check.log)，ring3 明确 skip |
| hosted，Linux 关 | 全量 0 failures；[日志](../tests/evidence/logs/sigchld-hosted-off.log)、[检查](../tests/evidence/logs/sigchld-hosted-off-check.log)，SIGCHLD 明确 skip |
| QEMU 8.2.2 KVM，4 CPU，Linux+interp 开，无 NIC | 全量 0 failures；[日志](../tests/evidence/logs/sigchld-qemu-on.log)、[检查](../tests/evidence/logs/sigchld-qemu-on-check.log)，真实 fork/exit、pending、autoreap、页/commit 回收断言均通过 |
| QEMU KVM，4 CPU，Linux 关，无 NIC | 全量 0 failures；[日志](../tests/evidence/logs/sigchld-qemu-off.log)、[检查](../tests/evidence/logs/sigchld-qemu-off-check.log) |
| VBox 7.2.20，UEFI，4 CPU，Linux+interp 开，无 NIC | 全量 0 failures；[日志](../tests/evidence/logs/sigchld-vbox-on.log)、[检查](../tests/evidence/logs/sigchld-vbox-on-check.log)，hush、ldso/TLS、线程/迁移黄金链通过 |
| 默认产品 ISO，Linux 默认关 | [shell 提示符烟测](../tests/evidence/logs/sigchld-product-boot.log)，闲置 timeout=124，非全量 selftest |
| freestanding 构建 | [kernel](../tests/evidence/logs/sigchld-kernel.log)、[ISO](../tests/evidence/logs/sigchld-iso.log)，无新增 warning/error |

无 NIC 的全量运行不冒充真实网络 wire 验收。本轮未改网络核心，也未开启 hush JOB/TRAP。
