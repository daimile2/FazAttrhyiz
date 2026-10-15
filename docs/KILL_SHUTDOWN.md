# 后台 kill 与有界关机

## 使用

产品默认用户态 shell 支持 `kill <pid>`。PID 从 `run /bin/spin.elf &` 的
`background start` 日志取得；例如首次启动通常是 PID 3，不能把这个数字当固定 ABI。
`/bin/spin.elf` 打印启动消息后在 CPL=3 无限循环，不调用 yield，供验收抢占终止。

```text
run /bin/spin.elf &
kill 3
poweroff
```

kill 只针对本 shell 保存的后台任务。无效数字返回 INVAL，不在后台表的 PID 返回
NOENT；不支持任意 PID、信号编号或完整 POSIX signal。请求被接受不等于已经回收。
正常终止日志为 `user kill retired`，随后 `shell child done: ... exit=137 reap=ok`。
137 是本实现的强制终止退出码，未实现 SIGKILL 的 POSIX 信号体系。

## 安全终止

CHILD capability 增加 SIGNAL right，保留 WAIT；syscall 38 要求该 right，并检查
capability 所属进程和目标 parent_pid。不能杀 PID1、自身或别的控制器的子进程。
句柄在 wait/reap 时消费，旧句柄再 kill 被拒绝。用户的数字 PID 不直接成为内核授权。

终止请求通过原子 kill_pending 发布。目标 CPU 在调度器接回已停止的用户上下文后
执行与 exit 共用的资源关闭，再走原有 off-CPU thread/image/task 回收。
没有远程直接释放正在执行的栈或页表。若目标正在 syscall 中，记录 syscall_depth，
让内核 continuation 正常展开；在 syscall 返回时处理请求，不丢弃持锁现场、临时缓冲
或 endpoint 引用。尚未首次进入 ring3 的任务在入口检查请求。

首版不能立即中断所有阻塞内核调用；无限阻塞的 syscall 可以保留终止请求，直到其
返回。纯用户态死循环由已有 tick 抢占处理，不依赖用户主动调用 syscall。
KPTI、buddy 锁、时钟标定与磁盘布局均未更改。

## 关机上限与失败策略

`poweroff`/`shutdown` 和 `exit` 先 reap 已退出任务；有后台时最多正常等待 5 秒。
超时向每个后台任务提交 kill，随后最多等 2 秒收尾。时间使用 syscall 8 的 TSC
和 syscall 39 暴露的现有标定 TSC Hz，不引入新标定方法。每阶段还有 2,000,000 次
循环上限，标定不可用时仍有限；极端机器可能早于时间期限结束。
这些期限约束等待阶段；正常磁盘 sync 和 ACPI 自身仍使用原有超时行为。

- 后台全部回收：正常 sync 根文件系统，授权 BOOT capability 后 ACPI S5。
- 7 秒等待预算耗尽：`exit` 返回 BUSY，留在 shell；`poweroff` 明确打印
  `deadline exhausted`，使用 BOOT/DESTROY 授权的 FORCE 模式关机。
- FORCE 且仍有子进程：不释放活跃内核现场，打印 `live contexts retained` 和
  `root sync skipped`，跳过可能等待活跃调用锁的 sync。此应急路径不保证未同步数据
  持久化。没有 ACPI 或 S5 返回时禁用 AP 调度并安全 hlt；不是重新启动或继续调度。

## 新 ABI

| syscall | 参数 a,b,c,d | 返回/权限 |
| --- | --- | --- |
| 38 CHILD_KILL | child_handle,0,0,0 | 0=request accepted；CHILD/SIGNAL + owner 检查 |
| 39 TSC_HZ | 0,0,0,0 | 已有标定 Hz，0 表示不可用；只读数值 |
| 37 POWEROFF | boot_handle,flags,0,0 | flags=0 正常；flags=2 FORCE；BOOT/DESTROY |

非法保留参数返回 INVAL，无 capability/right 返回 DENIED。
FORCE 没有活跃子进程时仍执行正常 sync。普通 ELF 不默认获得 CHILD 或 BOOT 权限。

## 验证与日志

`make test` hosted 校验拒绝/请求/回收；hosted 不执行 ring3，因此明确 skip 无限循环。
`make iso LIMINE_CONFIG=boot/limine-selftest.conf` 的全量测试在多核启动真实无限循环
ELF，校验退出码 137、抢占计数、弱 SIGNAL 权限拒绝、句柄消费，以及前后空闲页快照
无泄漏。日志包含 `user kill selftest: infinite ELF stopped`、`reap=ok`、
`cap-denied=verified` 和 `selftest result: PASS (0 failures)`。

产品中也可连续启动两个 spin 后直接 poweroff，验证
`grace=5s` → `timeout; kill background; kill-wait=2s` →
`killed and reaped background` → `root sync=0` → `ACPI S5 request`。
PS/2 输入可以用 QEMU monitor `sendkey` 自动注入。

已有持久根中的 `/bin/shell.elf` 不被启动时覆盖。升级验收应使用新安装盘/空白测试根，
或者显式更新磁盘上的 shell ELF；仅换启动内核不会替换既有用户 shell。
