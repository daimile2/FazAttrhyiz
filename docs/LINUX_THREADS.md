# 有界 Linux clone 线程与 futex（2026-10-17）

> 证据归档：见 [tests/evidence 索引与脱敏规则](../tests/evidence/README.md)。旧矩阵保留首版轮次；本轮fd白名单放宽的实际结果见文末新增矩阵。命令中的 `/tmp` 是新运行的输出位置。

双轨不变：默认 `capmix.linux=0`；仅判定为Linux的ELF走persona。不是通用musl
pthread、完整POSIX线程、完整Linux clone/futex兼容。原生ABI及原生fork=NOTSUP保留。

## 支持的接口

| x86-64 nr | 接口 | 本轮范围 |
|---:|---|---|
| 56 | clone | flags/child_stack/parent_tid/child_tid/tls，继承当前syscall返回现场，子返回0，父返回TID |
| 202 | futex | WAIT=0、WAKE=1，可加PRIVATE_FLAG=128；同一地址空间内的32位对齐字 |
| 24 | sched_yield | 让出当前调度机会并恢复自己的用户入口现场 |
| 186 | gettid | 当前线程TID；leader的TID等于TGID |
| 39 / 110 | getpid / getppid | 所有线程返回组TGID/组父PID |
| 218 | set_tid_address | 登记当前线程clear-child-tid地址，返回当前TID |
| 60 | exit | 只退出当前线程；最后一个线程执行组资源清理 |
| 231 | exit_group | 请求整个组退出，唤醒futex等待者；各线程安全退场后统一回收 |
| 158 | arch_prctl | 沿用SET_FS/GET_FS，每个线程有独立FS上下文 |

原生syscall表不变。Linux204查询可用CPU集合，允许当前TID或TGID；309仍查询
实际CPU/NUMA节点。组内固定在leader执行CPU，独立线程组可分别位于不同CPU。
不增加运行中迁移和跨CPU共享同一mm的TLB策略。

### clone旗标与资源界限

第一版必须同时带：

```
CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD = 0x10f00
```

额外可选 `CLONE_SETTLS=0x80000`、`CLONE_PARENT_SETTID=0x100000`、
`CLONE_CHILD_CLEARTID=0x200000`、`CLONE_CHILD_SETTID=0x1000000`。
黄金join组合为 `0x1390f00`。低字节退出信号必须为0。
缺必需位、其他旗标/组合（含clone3、vfork、SYSVSEM、PIDFD、NEW*）仍ENOSYS并计数。
clone3本身仍未实现。不是把任何clone组合都转成fork。

- 至多8个执行记录/组，含leader；与进程共用32个全局执行槽，不扩大MAX。
  第一版已退出成员记录保留到组最终回收，因此是整个组生命周期累计8个，
  不承诺反复创建/销毁无限线程。达到任一上限或生命周期门竞争返回EAGAIN；
  调用者须处理/重试，黄金探针对竞争做有界yield重试。
- 用户自己准备栈和TLS；child_stack须16字节对齐，顶端下方16字节可写，
  否则EFAULT。没有自动栈/TLS分配、guard page或完整用户故障信号处理。
  用户必须保证实际使用的整个栈范围足够；这里只验证ABI能检查的栈顶。
- SETTLS只设置用户低canonical范围FS base，内核不复制/分配TCB、DTV或pthread结构。
  超出用户范围EPERM；不带SETTLS则继承父线程FS。上下文切换复用现有MSR_FS_BASE保存。
- 带TID写入/清除旗标时，传入地址须是已映射可写4字节，否则EFAULT；
  初始TID在发布成员之前写入共享镜像。退出时clear-child-tid置零，再WAKE一个同mm等待者；
  set_tid_address可重新登记；退出时坏地址忽略写入故障，不触发内核崩溃。
  单线程同样处理登记；成功exec清除旧登记。

Linux ENOSYS/write等计数按执行记录记录；组leader退出摘要不自动聚合成员计数。

成员不是独立进程：TGID相同、共享task/capspace、同一个ELF/mm及同一fd表；
每个成员有独立TID、寄存器、syscall frame、FS、内核栈和既有窗口内独立入口栈。
`cm_proc_current`按实际调度线程定位执行记录，不再仅依task取第一个记录。

### 受控fd白名单与仍冻结范围

一个镜像创建过线程后，直到组退出，暂拒绝以下操作（EOPNOTSUPP）：

- fork/wait4、替换式execve；原有未创建线程的fork/COW/exec不变。
- brk、mmap、munmap、mprotect；不支持线程运行中变更映射。
- open/openat、用户fcntl（包括F_DUPFD）、pipe/pipe2及socket操作仍拒绝；dup3未实现，返回ENOSYS并计数。
- close/dup/dup2只按下表放宽FILE/console；pipe/socket fd仍禁止关闭、复制或被替换。
  带有已打开Linux socket的进程也暂拒绝clone（EOPNOTSUPP），避免把阻塞socket析构带入线程组退场路径。
  clone真正共享fd表，未偷偷复制；已有console/FILE可read/write/readv/writev/seek，
  共享偏移沿用VFS同步。socket读写也明确拒绝。

本轮新增白名单（Linux persona；仅创建过线程的组，默认Linux开关仍关）：

| 调用/资源 | 允许 | 明确边界/错误 |
| --- | --- | --- |
| dup(32) | 从现有FILE或console复制，返回最低空闲fd | 结果须≥3；若最低空位在0–2则EOPNOTSUPP，不跳过它伪造Linux语义；满表EMFILE |
| dup2(33) | FILE/console之间复制或替换；别名共享原open-description和偏移；同fd≥3可返回原fd | 目标须≥3；源或目标若为pipe/socket则EOPNOTSUPP；无效源/越界目标EBADF |
| close(3) | 关闭≥3的FILE/console别名，对所有组成员可见 | 有效fd0–2、pipe/socket为EOPNOTSUPP；无效/重复关闭为EBADF |
| 已有read/write/readv/writev/lseek/fstat/getdents等 | 沿用已有FILE/console权限及copyin/out；FILE包含VFS文件/目录描述符 | 不新增open或fcntl；socket I/O仍拒绝；原有pipe I/O保持，不新增pipe fd |
| fork/wait4/execve | **仍拒绝EOPNOTSUPP** | 创建过线程后，成员退出也不解除限制 |
| brk/mmap/munmap/mprotect | **仍拒绝EOPNOTSUPP** | 包括查询式brk；不做运行中映射扩缩或权限修改 |

fd表仍32项，没有扩大线程/映射/commit容量。所有FILE复制仍经现有capability的DUPLICATE权限检查，
不提升原句柄权限。成功dup/dup2清新fd的CLOEXEC；本轮不允许用fcntl修改它。
保留fd0–2是明确的首版限制：stdio_mask仍为每执行记录元数据，不声称已支持线程间重绑定标准流。
没有创建线程的Linux进程继续使用原来完整的有界fd实现；CapMix原生ABI不走新门控。

组leader保存一个非阻塞fd操作门（owner=TID）。所有会借用/修改这些FILE fd的Linux调用都在门内执行，
竞争者立即返回EAGAIN(-11)，由用户有界yield/重试；不忙等自旋锁，不保证公平或阻塞dup/close语义。
同owner的writev→write嵌套可重入，深度归零后release。错误返回也释放门。
门只保留owner标志，不持cap/VFS自旋锁跨I/O、调度或析构，既有kill续体退出纪律不变。
原有阻塞pipe读写不占门，因为pipe槽本轮禁止修改，避免读者睡眠时阻挡写者或其FILE操作。
其他成员退出不会清空仍活跃线程的fd；最后成员统一关闭资源、确认门空闲，再按既有规则离CPU回收。
这不是通用pthread/通用Linux多线程文件接口，也不增加跨CPU迁移。

CLONE_FS对应当前固定cwd等有限FS状态；SIGHAND共享已有默认/忽略元数据，
不提供信号处理器执行。不能据此声称普通pthread初始化/分配器/网络线程可用。
成员退出不关闭共享fd、不销毁task/mm；所有成员退场后仅组leader资源清理一次。
最后一个线程前leader可以先exit，剩余线程继续使用mm/FILE。
首版组状态保留leader普通exit码，exit_group/kill可覆盖；普通成员exit码不代表组码，
也不把数字137本身误认为kill。不是完整Linux退出状态边角兼容。

用户栈来自调用者已经映射并计入commit的页；clone不复制/重复charge该mm。
新内核线程栈由现有有界buddy分配，失败回滚；入口栈复用原32执行槽的静态窗口。
最终reap须确认每个线程已离开CPU/入口栈，再清成员记录，mm只销毁一次。
原有驻留COW及副本reserve/commit记账不变；COW仍不压缩/换出。

## futex语义

- key=(mm leader身份, user VA)，不是裸VA或内核指针；两个组同VA互不唤醒。
  普通0/1与PRIVATE变体均限同mm；无跨进程共享futex。
- 锁内重新copyin比较32位值并登记等待、prepare-block，释放锁后切出；
  WAKE和超时使用同锁注销并唤醒，配合现有早唤醒CPU claim纪律，避免丢失唤醒。
- WAIT值不等→EAGAIN；未对齐→EINVAL；坏uaddr/timeout→EFAULT。
  WAKE返回实际唤醒数；负count→EINVAL。
- timeout是Linux x86-64的16字节相对timespec(sec,nsec)，固定宽度与偏移断言；
  sec=0..60，nsec=0..999999999。范围外EINVAL；零时间/到期ETIMEDOUT；NULL无限等待。
- 超时和kill取消在现有调度器runnable检查的非切换poll中处理；不是hrtimer，
  不承诺硬实时精度。没有等待者时仅一个atomic分支，不持锁扫描。
- exit_group/kill唤醒阻塞成员，内部WAIT返回EINTR后按现有syscall展开纪律退出，
  不强制丢弃持有资源的内核续体。
- BITSET/REQUEUE/PI/WAKE_OP、CLOCK_REALTIME等仍ENOSYS+计数；无robust-list、
  优先级继承futex、信号全集或完整公平性保证。

## 固定黄金探针与复现

`tests/linux_threads.S`是仓库自有手写_start，无libc和pthread依赖。
固定ELF为 `tests/fixtures/linux-threads.elf`，SHA-256：

```
0306559518f8fbeb4e5600a2b3adc7b4217e7f5e79ae0d8ca9df9c5d35dcb22e
```

配方为GNU GCC13.3.0/Binutils2.42，`tests/linux_hello.ld`及Linux OSABI标记脚本：

```bash
make linux-threads-verify
```

该目标从源码重建，mark-linux，再cmp逐字节并验SHA。源码显式`.file`，
避免临时目标文件名进入ELF符号表；无build-id和时间戳依赖。已连续重建比较两次。
正常kernel构建核对manifest后嵌入，selftest还对嵌入字节计算SHA-256打印，
宿主断言与仓库固定ELF一致。不得只比较stdout就忽略错误版本探针。

```bash
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
python3 scripts/linux-enosys.py --log /tmp/futex.log
python3 tests/check_linux_threads.py /tmp/futex.log
python3 scripts/linux-enosys.py --dual --log /tmp/futex-numa.log
python3 tests/check_linux_threads.py /tmp/futex-numa.log
```

selftest注入 `/bin/linux-threads.elf`；这不是默认产品/root安装包新增文件。
黄金用例join、leader、group、ap、pair在CPL3覆盖：

- private用户栈及寄存器、独立FS、TGID/TID、父/子TID写入；
- 实际WAIT阻塞→WAKE→恢复、超时/EAGAIN/EFAULT/EINVAL；
- clear-child-tid后join、leader先退出而子线程继续write；
- 8线程上限和第9次EAGAIN、等待七个成员进入停泊路径后exit_group=9取消等待者；
- CPU1运行同一探针；CPU0/1两个组同VA并发，隔离与回收；
- 子线程dup/close对父可见、别名共享偏移、FILE↔console替换、递归writev、满fd表EMFILE及关闭别名后原fd可用；
- pipe保持可读写且禁止关闭/替换；fd0–2、非法fd、open/fcntl/socket及fork/exec/映射变更拒绝，成员退出后仍冻结；
- 每个leader有两次故意ENOSYS（不支持clone组合、futex op），最终各组页数与commit回基线。

hosted验证共享mm/task/fd的机制、TGID/TID、clear-child-tid、拒绝项及回收，
还验证普通线程exit=137不会被误当整组kill，以及已有socket时clone拒绝；
以悬停owner模型确定性检查EAGAIN、不发布半成品fd、无stdio空洞绕过；guest验证真实线程行为。
明确skip硬件clone/真实阻塞。宿主脚本递归检查原有SMP、L0–L5、hush、ldso/TLS/W^X及TCP池。
不将hosted模型当成真实线程执行证明。

## 修改文件

- `include/capmix/linux_uapi.h`：固定timespec64布局与偏移断言。
- `proc/spawn.c`、`include/capmix/proc.h`：成员/组、共享fd元数据、clone、退出及reap。
- `proc/syscall.c`：线程退出桥接；`abi/linux.c`、`linux_file.c`、`linux_process.c`、
  `linux_signal.c`：号表、TID、exec边界与共享元数据。
- `abi/linux_futex.c`、`sched/eevdf.c`：有界等待表、prepare/wake及调度poll hook。
- `arch/x86_64/usermode.c`：允许leader线程先退出及准确记录resume入口/栈；
  不改KPTI策略、CR3切换或entry汇编。
- `boot/linux_threads_test.c`、`boot/kernel.c`：机制/硬件用例与注册。
- `tests/linux_threads.S`、固定ELF/哈希、`tests/check_linux_threads.py`、`Makefile`、`.gitignore`：重建、固定ELF收录及验收。
- 本文及导航/旧限制文档：更新支持边界。

## 首版环境矩阵（历史证据）

日期2026-10-17。QEMU 8.2.2/KVM、512MiB、4 vCPU；双NUMA各256MiB。
本节开启配置的guest匹配首版黄金SHA-256：`6442d7b9e331bad1a5aaff8338e889df6643e2600f57b64ef9079253babc225c`。
它们不是本轮新黄金的运行证据；当前SHA及本轮矩阵分别见上方和文末。

| 环境 | 实际结果 | 日志 |
|---|---|---|
| hosted，Linux+interp开 | 全量PASS (0 failures)；共享资源/errno/普通exit137/clear-tid/回收机制通过，硬件明确skip | [futex-final-hosted-on.log](../tests/evidence/logs/futex-final-hosted-on.log) |
| hosted，Linux运行时关 / 编译关 | 两份全量PASS (0 failures) | [futex-final-hosted-off.log](../tests/evidence/logs/futex-final-hosted-off.log)、[futex-final-hosted-compiled-off.log](../tests/evidence/logs/futex-final-hosted-compiled-off.log) |
| QEMU单 / 双NUMA，Linux+interp开，无NIC | 两份全量PASS (0 failures)；join/leader/group/AP/同VA隔离、页/commit回收及原有黄金链断言通过 | [futex-final-on.log](../tests/evidence/logs/futex-final-on.log)、[futex-final-dual.log](../tests/evidence/logs/futex-final-dual.log) |
| QEMU Linux开、interp关 | 全量PASS (0 failures)，静态线程/hush通过，动态黄金按gate跳过 | [futex-final-static.log](../tests/evidence/logs/futex-final-static.log) |
| QEMU e1000 / virtio-net，Linux+interp开 | 两份全量PASS (0 failures)，线程/hush/静态/动态及TCP multi8/UDP/DHCP断言通过 | [futex-final-e1000.log](../tests/evidence/logs/futex-final-e1000.log)、[futex-final-virtio-net-pci.log](../tests/evidence/logs/futex-final-virtio-net-pci.log) |
| QEMU Linux运行时关 / 编译关 | 两份全量PASS (0 failures)，原生保留 | [futex-final-off.log](../tests/evidence/logs/futex-final-off.log)、[futex-final-compiled-off.log](../tests/evidence/logs/futex-final-compiled-off.log) |
| VirtualBox7.2.20r175154，UEFI/4CPU/无NIC，Linux+interp开 | 全量PASS (0 failures)，全部硬件线程用例与原黄金链断言通过 | [futex-final-vbox.log](../tests/evidence/logs/futex-final-vbox.log) |
| QEMU默认产品ISO，Linux关 | 原生user shell/hello exit=0/poweroff通过 | [futex-final-default-product.log](../tests/evidence/logs/futex-final-default-product.log) |
| QEMU新安装virtio盘/e1000，无ISO，Linux+interp开，两次冷启动 | 原生重定向持久化/cat/nc/http-get/httpd、Linux TCP/hush/动态/TLS与poweroff通过 | [futex-final-product-first.log](../tests/evidence/logs/futex-final-product-first.log)、[futex-final-product-second.log](../tests/evidence/logs/futex-final-product-second.log) |

产品行不是全量selftest；通过条件是命令exit、输出、HTTP响应和冷启动读回。
VBox本轮无NIC，不把QEMU网络结果写成VBox网络PASS。
`make kernel`无新增warning/error，`make iso`成功；hosted保留既存arch未使用warning。
最终ISO为默认产品、Linux关闭。重建验证记录在 [futex-fixture-verify-final.log](../tests/evidence/logs/futex-fixture-verify-final.log)。
固定ELF通过`.gitignore`例外纳入候选文件，未依赖被忽略的本地build产物。

## fd白名单续作验收（2026-10-17）

新黄金复用join/leader/group/AP/双CPU独立组场景；每组文件路径含TGID，避免并行组互相截断文件。
父/子分别读到ab/cd/ef，共享偏移逐步推进；子关闭父创建的别名、创建的别名跨子退出保留；
父再验证FILE/console替换、writev重入、满表与回收。组结束后检查全部fd关闭、FILE/pipe capability句柄为零、
fd gate空闲，以及原有pages/commit/reap基线恢复。不是只比对输出就忽略退出码。

关键日志：

- `linux thread fd: shared dup/close/offset/full/reject ok`（正常join/AP/双组共4次）
- `linux thread fd mechanism verified: ... gate-contention=EAGAIN ...`（hosted机制模型）
- `linux thread fd cleanup verified: ... fd-empty=1 file/pipe-handles=0 gate=idle`
- `linux threads selftest: PASS (0 failures)`及全量`selftest result: PASS (0 failures)`

本轮新增/调整：`abi/linux.c`、`abi/linux_file.c`、`include/capmix/proc.h`、
`boot/linux_threads_test.c`、`tests/linux_threads.S`、固定ELF/哈希、`tests/check_linux_threads.py`及本文/相关限制说明。
未改KPTI策略、buddy锁、定时器、RTO算法、线程组8条累计上限或CPU亲和纪律。

### 本轮环境矩阵

所有开启行均匹配本轮黄金SHA与7288字节长度；静态hush、L0–L5、ldso/TLS/W^X、
原生phase0–9/NUMA/TRACE随对应全量及递归checker验收；硬件/网络skip仍按日志范围，不能扩大为实网PASS。

| 环境 | 结果 | 证据 |
| --- | --- | --- |
| hosted Linux+interp开 | 全量0 failures；共享fd门控/争用EAGAIN/拒绝项/资源回收机制，硬件clone明确skip | [日志](../tests/evidence/logs/threadfd-hosted-on-final.log)、[断言](../tests/evidence/logs/threadfd-hosted-on-assertions.log) |
| hosted Linux运行关 / 编译关 | 两份全量0 failures，原生保留 | [运行关](../tests/evidence/logs/threadfd-hosted-off-final.log)、[编译关](../tests/evidence/logs/threadfd-hosted-compiled-off.log) |
| QEMU8.2.2/KVM，512MiB/SMP4，无NIC，Linux+interp开，单节点/双NUMA2×256MiB | 两份全量0 failures；真实线程白名单/拒绝/句柄及页commit回收、hush/静态/动态黄金通过 | [单节点](../tests/evidence/logs/threadfd-on-final.log)、[双节点](../tests/evidence/logs/threadfd-dual-final.log) |
| QEMU同配置，Linux开/interp关 | 全量0 failures；静态hush/线程通过，动态明确gate skip | [日志](../tests/evidence/logs/threadfd-static-final.log) |
| QEMU Linux运行关 / 编译关 | 两份全量0 failures，原生保留 | [运行关](../tests/evidence/logs/threadfd-off-final.log)、[编译关](../tests/evidence/logs/threadfd-compiled-off-final.log) |
| VBox7.2.20r175154，UEFI/PIIX3/512MiB/4CPU，无NIC，Linux+interp开 | 修复后两次全量0 failures；线程/hush/静态/动态黄金及回收checker通过；未测VBox网络 | [首次通过](../tests/evidence/logs/threadfd-vbox-fixed.log)、[复测](../tests/evidence/logs/threadfd-vbox-repeat.log)、[断言](../tests/evidence/logs/threadfd-vbox-repeat-assertions.log) |
| QEMU标准串口命令，默认e1000/user-net，Linux开/interp关，已启动宿主回显端 | 全量0 failures；线程/hush、Linux-net、UDP、native TCP/multi8/RTO通过 | [日志](../tests/evidence/logs/threadfd-qemu-standard.log)、[断言](../tests/evidence/logs/threadfd-standard-assertions.log) |
| QEMU默认产品ISO，Linux关 | 用户shell、native hello exit=0、ACPI关机通过；不是全量selftest | [日志](../tests/evidence/logs/threadfd-default-product.log)、[断言](../tests/evidence/logs/threadfd-default-product-assertions.log) |

VBox初轮真实失败与诊断日志保留：[初轮](../tests/evidence/logs/threadfd-vbox-first-failure.log)、
[阶段诊断](../tests/evidence/logs/threadfd-vbox-diag.log)。诊断返回-11，分发链显示fork先竞争生命周期门，
再在其内部检查grouped；另组clone可令禁止项先返回EAGAIN，违反探针要求的确定性EOPNOTSUPP。
已在Linux dispatch前置fork/exec/wait4禁止检查，不修改fork/COW生命周期算法，也未放宽拒绝断言。
修复后两个独立冷启动通过，旧失败不改写为PASS。新增黄金失败会打印stage及返回值，便于继续定位。

标准QEMU命令使用的网络selftest配置含echo=1。初次未启动宿主TCP/UDP/HTTP测试端，
网络出现5 failures，日志[单独保留](../tests/evidence/logs/threadfd-standard-missing-peers.log)；
补齐9998 TCP、9999 UDP与8000 HTTP测试端后，同内核/同ISO重跑通过。没有为该配置错误改网络测试或RTO。

构建记录：[fixture复现](../tests/evidence/logs/threadfd-fixture-final.log)、
[kernel](../tests/evidence/logs/threadfd-kernel-final.log)、[诊断输出](../tests/evidence/logs/threadfd-kernel-diagnostics.log)、
[ISO](../tests/evidence/logs/threadfd-iso-on.log)。最终恢复默认产品ISO，Linux默认关。

独立单线程进程新增受控跨CPU迁移，见 [LINUX_MIGRATION.md](LINUX_MIGRATION.md)。
本文件的线程组仍固定同CPU，即使其他成员退出也不放宽；迁移请求返回EOPNOTSUPP。


### SIGCHLD 后续边界

[最小 SIGCHLD](LINUX_SIGCHLD.md) 不提供线程信号 handler 或任意线程路由。
线程组中拒绝设置 SIGCHLD action；普通 clone 线程退出不发送 SIGCHLD。
进程级 fork 子进程整体结束的通知与自动回收，不等同于完整 pthread/作业控制。
