# 用户态 PID1 与 shell（静态 ELF）

## 产品启动

默认 `make iso` 进入产品路径：

1. 内核初始化既有设备/VFS，临时挂载所选根：virtio-blk 优先，其次 AHCI，无盘则 RAM COW。
2. Bootstrap 仅补入缺失的 `/sbin/init.elf`、`/bin/shell.elf` 和已有测试 ELF，逐个 fsync；已有文件不覆盖。内核经原 ELF loader 创建 PID1，注入启动 capability，然后开启原 SMP 调度和 RX worker。
3. `/sbin/init.elf` 真实 CPL=3 执行。它请求一次正式根挂载：内核检查无打开文件，sync/umount `/`，重挂已有 COW 卷并挂回 `/`。这不是假称用户态直接执行块 I/O，也不是支持任意 mount 类型/设备参数的完整 mount API。
4. init 请求既有 DHCP/static 配置，打印 root/backend/NIC，然后通过 capability 创建 `/bin/shell.elf`，等待并回收 shell。
5. shell 真实 CPL=3 执行，读取 PS/2 解码字符、输出 FB + serial、创建并回收用户 ELF。内核不运行产品命令解析器。

如果 bootstrap 挂根、ELF 或 capability 准备失败，记录错误并保留内核控制台；不悄悄回退到内核 shell。正式挂根失败时 init 返回非零，由内核收尾。没有 ACPI S5 的平台沿用原安全 hlt 降级。

init/shell 都固定 CPU0。普通子 ELF 沿用原固定亲和和 round-robin，无用户线程迁移。本轮没有修改 KPTI、buddy 锁、时钟标定、CR3/GS/入口栈或 switch/syscall 汇编。`net/runtime.c` 仅将停止 join 从固定 512 步改为最多 2 秒/1000 万步（hosted 保持 512 步），允许 AP 释放最后的 worker claim；RX 循环保持原样。

## 构建与镜像

`user/crt0.S` + `user/runtime.c` + `user/init_main.c` / `shell_main.c`，通过 `user/user.ld` 链成静态 ET_EXEC：

- `build/user-init.elf` → `build/user_init.inc` → VFS `/sbin/init.elf`
- `build/user-shell.elf` → `build/user_shell.inc` → VFS `/bin/shell.elf`

仍是 CapMix ABI，不是 Linux ELF ABI；没有动态链接器/libc/TLS。动态对齐 text/data 段，保留原 ELF loader 的 W+X 拒绝和地址/页数约束。

磁盘中已有 init/shell 不会被升级或覆盖。开发验收应使用新 scratch disk、删除旧 bootstrap 文件后重新启动，或通过受控文件工具显式替换；单纯重建 ISO 不会覆盖已有磁盘 ELF。内核嵌入数组负责初始 provisioning，不是 verified boot/签名认证。

debug 仍用 `make iso LIMINE_CONFIG=boot/limine-selftest.conf`；所有 phase 自测照常运行。旧 `user/shell.c` C-entry shell 仅保留 parser/debug 测试，产品入口中的 native PID1 已删除。

## Capability 与用户 ABI

对象类型新增 CONSOLE、CONTROL、BOOT、CHILD，能力值是任务局部 handle。bootstrap 已知句柄布局：1=CONTROL、2=CONSOLE、3=BOOT；这些不是全局授权开关。

- init：CONTROL EXEC/WAIT/DUPLICATE，CONSOLE READ/WRITE，BOOT MAP/WRITE/DESTROY。
- shell：CONTROL EXEC/WAIT，CONSOLE READ/WRITE，BOOT DESTROY。它不能正式挂根或重新配置网络。
- 普通子程序：不继承上述管理能力；继续使用原 fdio 文件、网络 syscall。缺失/错类型/缺 right 的控制操作返回 DENIED。
- CHILD：WAIT/SIGNAL，绑定 owner PID 与 child PID。只有持有本进程子 capability 才可等待并回收；成功 wait 消费该句柄。没有 user handle-close/dup 的新增接口。

| 号 | 调用及参数（rax=号；rdi/rsi/rdx/r10） | 结果与约束 |
|---|---|---|
| 3 | SPAWN(control_h, request_ptr, child_out_ptr, 0) | request 的 path/argv/envp 先完整 copyin；成功返回 0，输出 `{pid,handle,cpu}`；原 native C-entry 调用仍使用旧可信 attr API |
| 31 | CONSOLE_READ(console_h, char_out_ptr, 1, 0) | 非阻塞；成功 0，空队列 AGAIN；校验输出后才消费字符 |
| 32 | CONSOLE_WRITE(console_h, bytes_ptr, length, 0) | 0..4096B，完整输出，返回字节数；沿用既有 console，串口消息锁保护每次调用；锁忙为 AGAIN |
| 33 | CHILD_WAIT(child_h, wait_out_ptr, flags, 0) | flags=1 非阻塞未完成为 AGAIN；flags=0 协作 yield 等待；成功返回 0、输出 exit/preempts/cpu 并回收 off-CPU 镜像/task/记录 |
| 34 | YIELD(0,0,0,0) | 普通 ELF 可用，无 privileged capability；沿用现有上下文恢复 |
| 35 | READDIR(path_ptr, index, dirent_out_ptr, 0) | 与原公开 VFS 路径访问一致；输出 `cm_dirent_t`；末尾 NOENT；index≤UINT32_MAX |
| 36 | BOOT_CONTROL(boot_h, operation, bootinfo_out_ptr, 0) | operation=0 根正式挂载（MAP、PID1、BSP、一次性）；1 既有网络配置（WRITE、PID1、根已完成）；输出 disk/cpus/backend/NIC/ABI version=1 |
| 37 | POWEROFF(boot_h,flags,0,0) | DESTROY；flags=0 正常 sync，活跃子进程 BUSY；flags=2 应急关机，活跃子进程时明确跳过 sync |
| 38 | CHILD_KILL(child_h,0,0,0) | CHILD/SIGNAL + owner；提交终止请求 |
| 39 | TSC_HZ(0,0,0,0) | 已有标定 TSC Hz，只读 |

`cm_user_spawn_t` 固定 32B：三个 uint64 用户地址（path、argv、envp），uint32 flags、int32 cpu。cpu=-1 为自动 round-robin，显式 CPU 必须在线。flags=0 普通 ELF；`CM_SPAWN_SHELL=1` 只允许 PID1 的可委派控制能力创建 `/bin/shell.elf`，为这个 CPU0 子进程安装 shell bundle；不是任意子进程的权限继承。

`cm_user_child_t` 16B；`cm_user_wait_t` 32B；`cm_user_boot_t` 56B，定义见 `include/capmix/control.h`。路径≤127B+NUL；argv/envp 各≤32项，每项≤255B+NUL，字符串合计≤4096B。没有把用户嵌套指针交给原生内核 spawn。失败保持输出不变，释放参数快照、child capability 和未运行镜像；无权操作 DENIED、非法参数/指针 INVAL、容量不足 NOSPC、commit 不足 OVERCOMMIT。

## Shell 与生命周期

保留 help/echo/ls/cat/run/`&`/kill/exit/poweroff/shutdown，输入 256B 行缓冲与退格。只做空白分词、末尾独立 `&`，没有引号/重定向/管道/作业控制。cat 最多 4096B；ls 最多 256 项。

shell 管理 8 个后台任务 + 1 个前台。全局 32 proc 槽包含用户 init、用户 shell 及退出记录，所以产品最多 30 个额外 ELF 记录，实际还受 task/thread/handle/page/commit 约束。前台等待期间继续回收后台；后台 argv/envp 已复制，不借用输入行。

退出/关机正常等待最多 5 秒，再请求终止并等最多 2 秒；仍未回收则 exit 返回 BUSY，poweroff 明确记录并应急关机。`kill <pid>` 仅针对本 shell 后台表。详见 [kill 与关机超时](KILL_SHUTDOWN.md)。shell exit 后 init 消费 shell child handle、回收 shell，再自身 exit。内核监督收尾回收 PID1 并停止 RX worker，保留控制台；正常路径不再启动 native shell。

父进程提前 exit 会释放 CHILD capability；BSP supervisor 检测父进程死亡/记录消失，待孤儿自然 exit 且 off-CPU 后回收其镜像/task。不会强杀无限运行的孤儿，也没有完整 POSIX reparent/waitpid 通知语义。外部 task 引用阻止回收时保持 BUSY。用户 fault→进程失败的完整隔离不是本轮新增能力。

## 观察与验收

产品日志应含 `init ELF: pid=1 CPL=3`、`user init: root mount finalized`、`shell ELF ready: CPL=3`、`user spawn: parent=2`、`shell child done: ... exit=0 reap=ok`。正常 exit 后应含 `init: shell reaped status=0` 和 `init: PID1 exited; kernel console idle`；关机应含 `user poweroff: capability authorized root sync=0`。

`boot/control_test.c` 验证默认拒绝、授权、非法/嵌套用户指针、缺 WAIT、能力消费、poweroff 的 live-child BUSY、父退出后的孤儿回收，以及 free-page 基线。hosted 不伪称执行 ring3，产品 QEMU 才证明两个 C 语言镜像实际 CPL=3。

建议产品命令：

```
help
run /bin/tcpwire.elf &
run /bin/filetest.elf &
run /bin/concurrent.elf &
run /bin/udpwire.elf
cat /p1a/user.txt
exit
```

网络 echo 仍需宿主 `scripts/tcp-echo.py` / `scripts/udp-echo.py` 和 QEMU user netdev；无网卡行为继承原程序的 skip/NOENT。二次启动再跑 filetest 可观察 `p1a previous boot payload verified`。默认产品不跑完整 phase；debug PASS 应从独立 debug 日志判断。

普通 `run /bin/shell.elf` 不委派管理 bundle，该镜像检查缺失控制台能力后 exit=1；不会进入无限 DENIED 轮询。
