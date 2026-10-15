# Linux ABI L2：有界驻留内存、COW fork 与替换式 execve

L2 在可选 Linux persona 中加入可运行的 COW fork、brk、wait4 和 execve。
原生 CapMix ABI 仍优先 posix_spawn，原生 fork 仍 NOTSUP。产品默认 Linux 关闭，
开启和关闭均不删除任何原生或 Linux 文件，沿用 [L0](LINUX_ABI_L0.md) 的镜像识别、
FS_BASE/auxv 和 [L1](LINUX_ABI_L1.md) 的 FILE capability 翻译。

这不是通用 Linux/musl/glibc 兼容层，也未证明 BusyBox sh 已可用。
L1 的 **定制 musl 静态 BusyBox 子集 + 有界 BSS 分配**（echo/cat/ls）继续保留，
没有偷偷替换其 64KiB 分配器或固定 ELF。L2 的 brk 由独立手写静态 ELF 验证，
后续可逐步为新的定制 libc 配置提供分配来源。

## syscall 与边界

号表沿用 Linux v6.6 x86-64；每条调用只在 `CM_ABI_LINUX` 分支生效：

| 号 | 接口 | 支持与错误 |
| --- | --- | --- |
| 57 | fork() | 父返回子 PID，子从同一 syscall 后返回 0；资源槽不足 EAGAIN，commit/物理预留不足 ENOMEM，FILE 缺 DUPLICATE right 则 EACCES |
| 12 | brk(address) | 0 查询当前 break；有界扩展/收缩；失败返回旧 break，符合原始 Linux brk 约定，不返回 -ENOMEM |
| 59 | execve(path,argv,envp) | 同 PID 替换为明确标记的 Linux 静态 ET_EXEC，支持 argv/envp；成功不回旧入口。不存在 ENOENT，坏指针 EFAULT，格式/动态/跨 persona 镜像 ENOEXEC，参数过大 E2BIG，资源不足 ENOMEM |
| 61 | wait4(pid,status,options,rusage) | pid>0 或 -1，options=0 阻塞等待或 WNOHANG=1；未完成返回0，无子 ECHILD；只回收自己 fork 的子进程，status 是 exit_code低8位左移8位；rusage非NULL EOPNOTSUPP，其他选项 EINVAL |
| 9 | mmap(...) | 本轮全部 EOPNOTSUPP（Linux95，也称 ENOTSUP），包括文件映射、MAP_SHARED 和匿名 mmap；选择 brk 作为有限匿名内存入口 |

未实现的其他号继续 ENOSYS+计数。没有 clone/vfork、线程/futex、信号、socket 转译（后续 L3 另见 [L3](LINUX_ABI_L3.md)）、
动态链接、通用 TLS 或任意文件 mmap。wait4 的阻塞实现为让出调度器后重查，
不是产品化的事件等待队列；kill_pending 时可结束等待，不实现完整信号语义。
CPL3 自测有 5 秒上限；普通 wait4 不额外制造超时。

所有 Linux persona 进程首版固定 **BSP CPU0**，覆盖 shell 对普通命令的 CPU轮转选择，
启动日志为 `linux L2 affinity: requested cpu=N -> BSP0`。理由是沿用现有 BSP
创建/最终回收纪律，避免把 task/proc 发布与回收改造成新的 SMP 生命周期。
子与父固定同 CPU、允许 tick 抢占与阻塞现场切换；原生 ELF 的多 CPU 固定亲和和
run& 保持原策略。底层 fork/wait4 若在 AP 被直接调用则 EAGAIN。

## 页所有权与 COW

Linux 镜像的 PT_LOAD（含私有 BSS）、栈和 brk 页使用驻留 backing 引用表。
原生 ELF 仍走原独占页销毁路径。Linux fork 创建独立 full/shadow 页表，
沿用现有 KPTI 窗口与低半页表别名策略，不改变入口映射权限政策。

fork 成功前完成子进程资源准备；双方原可写叶 PTE 清 W，逻辑 PF_W 保留。
只读代码保持只读共享。父只收到子 PID；子复制用户通用寄存器、RSP、返回 RIP/RFLAGS
和 FS_BASE，RAX=0。不复制内核调用栈，不让两个线程返回同一个内核 continuation。

用户写触发真实 #PF 时，仅对 **present+write+user、CPL3、当前 Linux 镜像逻辑可写页**
尝试 COW。恢复通路保存全部通用寄存器与原 CR3，分裂/提升 PTE 后恢复并重试原指令。
其他 fault 仍走现有 fatal reporter；本轮没有实现任意非法用户访问的全面进程隔离。

内核 copyout 也先执行同一 COW 处理，再写入 backing；不能借 HHDM 绕过父子隔离。
FILE read、GET_FS、stat/dirent、wait status 等因此使用同一原则。
进程族只运行在同 CPU，更新 PTE 后失效本地 TLB；用户 shadow/full CR3 的切换继续刷新
相应地址空间。本轮不提供跨 CPU COW 共享族、TLB shootdown 或线程迁移。

## no-overcommit、预留与 swap/压缩

fork **预扣子镜像全部页的 commit**，只读页也保守计入。每个可写共享 backing 若有
R 个映射引用，就持有 **R−1 个实际 buddy 预留页**：

1. 每次 fork 为每个可写页再预留一个物理页；不足则撤销部分共享引用、预留页和 commit。
2. 写分裂从预留链取一页并复制4096B，减少共享引用，新页独占；不在 fault 中临时申请物理页，
   不再次增加 commit。只剩一个引用时直接恢复 W。
3. 销毁共享引用时，若仍多于一个引用，释放一个多余预留页；最后一个引用释放实际 backing。
   嵌套 fork、先父退出、brk 收缩和 exec 替换遵守同一规则。

本轮降低 fork 时立即复制的数据量；可写页的潜在副本仍占实际 RAM 预算，
只读页共享可节省物理页。这不是依赖后续回收才能兑现的 COW 承诺。

引用表预留了进程上限及每 CPU 暂存 exec 镜像的容量，防止承诺后因缺引用描述符而无法分裂。
引用表和 commit 计数有 IRQ-safe 自旋同步，commit 加法检查溢出；没有改 buddy 锁设计。
页表、镜像元数据和线程资源仍逐项检查 buddy/对象分配结果，失败不发布半成品进程。

**本轮 Linux ELF/COW/brk 页及副本预留全部常驻，不加入 `mm/anon.c` 的 LRU、压缩池或 swap。**
现有全局 commit limit 可包含 swap backing，但不能据此假定 L2 常驻页一定可分配：
fork/brk/exec 还必须拿到真实物理页；拿不到就同步失败并退回资源。
因此不支持换出 COW 页，不会压缩后留下无效的用户 PTE/HHDM backing 指针。
现有 anon 压力管线、swap 和文件 COW 仍独立工作。
L2 页也不计入现有 anon-owner RSS/jetsam 候选统计，不能宣称已实现统一进程 RSS/换页。

全局进程槽 **32**（包含原生 init/shell、退出未回收的记录），每镜像最多 **128页/512KiB**，
包括 PT_LOAD、四页栈和 brk，地址窗口仍为 **0x400000–0x800000**，栈从顶部占16KiB。
brk 从最高 PT_LOAD 末端向上增长，不能撞栈；超过页数/地址/commit/可用物理页限制返回旧 break。
收缩释放完整 heap 页，重新分配的完整页清零；不保证同一保留页内收缩再增长的字节被清零。
文件 fd 上限仍32。没有为了 L2 上调 buddy MAX、进程数或 ELF 页数。

## FILE 继承、退出和 exec 事务

fork 只继承 FILE fd 与 L1 控制台别名，保留读写权限和 CLOEXEC；FILE 重复引用共享同一
open-file offset。**不绕过 DUPLICATE right**：任一 FILE 无该 right 则整个 fork 返回 EACCES，
即使该 fd 带 CLOEXEC（fork 时仍需继承）。原生 shell 委派的重定向 stdio 当前通常仅有
READ/WRITE，所以 `run ... > file` 的程序若再 fork，可能被此安全边界拒绝。
不是完整 Unix fd 继承兼容；非 FILE 控制/启动能力不复制到 Linux 子进程。

退出关闭自己的 fd、记录 exit，再由父 wait4 在 off-CPU 后销毁镜像/页表/任务。
不 wait 的子记录仍占有资源，属于有界 zombie；产品 PID1 循环沿用现有 orphan reap，
本轮没有新增完整 Linux PID1/signal/作业控制。

execve 首先在内核独立快照 path/argv/envp；沿用128字节路径、32个argv/32个env、
每串256字节及合计4096字节限制。只接受 Linux 静态镜像，拒绝原生目标及动态目标。
路径必须绝对且不搜索PATH；相对路径返回ENOENT。argv为空沿用加载器默认 `{path,NULL}`，
argc至少1，envp为空表示空环境。旧镜像始终可用，直到新镜像加载/commit/参数全部准备成功。因此存在旧+新同时驻留的峰值，
内存不足可以拒绝 exec；不采用“先毁旧再尝试加载”的模式。

提交时保持 PID、parent、task、线程/亲和与非 CLOEXEC fd，关闭 CLOEXEC，FS_BASE 清0，
安装新 RIP/RSP/argv/auxv，更新该线程 full/shadow CR3 后再销毁旧镜像。
成功通过该进程自己的入口栈返回新 `_start`，不 spawn 新 PID。
失败前不关闭 fd、不改变 FS 或原用户返回现场；返回错误后旧程序可继续执行。

## 构建、测试及证据

开关与 L0/L1 相同：产品默认运行时关闭；`capmix.linux=1` 启用，`capmix.linux=0` 优先关闭；
`LINUX_ABI=0` 时不可打开。禁用不触碰盘上数据或原生 `/bin`。

```bash
make test
make kernel 2>&1 | grep -iE 'warn|error'
make iso LIMINE_CONFIG=boot/limine-linux-selftest.conf
# 需要网络全量回归时，分别启动 scripts/tcp-echo.py 和 scripts/udp-echo.py
# 使用原 SMP4 QEMU 串口命令并可加 e1000/virtio-net
python3 tests/check_linux_l2.py /tmp/linux-l2-qemu-on-final.log
make test LINUX_ABI=0
make iso LINUX_ABI=0 LIMINE_CONFIG=boot/limine-linux-selftest.conf
python3 tests/check_linux_l2.py --disabled /tmp/linux-l2-qemu-compiled-off-final.log
```

`boot/linux_l2_test.c` 的 hosted 与 freestanding 机制测试检查：父子 PTE RO、root/shadow
一致、共享 backing、copyout 分裂、嵌套共享、先父销毁、brk 清零/收缩/页数/边界，
commit 打穿与溢出拒绝，以及 FILE 无 DUPLICATE 时的晚期失败回滚。
每组核对 free+cache、commit，必要时核对 FILE 数；不以关闭 swap 掩盖泄漏。

`tests/linux_l2.S` 与 `tests/linux_l2_next.S` 是固定源码、可重复构建的静态 Linux 标记 ELF。
CPL3 路径实际 fork：父/子对 heap 和数据写不同值，检查通用寄存器/FS，GET_FS 触发 copyout，
失败 exec 仍执行旧镜像，成功 exec 在同子 PID 检查 argv、FS=0、CLOEXEC，exit42。
父 wait4 读 status=0x2a00、检查自身数据/FILE仍可用，brk 收缩再增长清零，最后 exit0。
自测比较总页/commit 基线；hosted 明确跳过 CPL3 执行，不假称跑过 fork 用户返回现场。

关键日志：`linux L2 mechanism`、`linux L2 fork`、`linux L2 COW fault`、
`linux L2 execve replace`、`linux L2 wait4 reap`、`rollback=ok`、
`linux L2 CPL3 verified`、`linux L2 selftest: PASS (0 failures)`。
关闭时为 `linux L2 skipped: persona disabled; native fork=NOTSUP`。
全量最终仍以 `selftest result: PASS (0 failures)` 为准。

L1 回归中的旧 brk ENOSYS 探针已改为未知保留号；mmap现在明确95。L0 固定黄金 ELF
及 L1 BusyBox 固定 ELF 未修改，三 applet 仍须通过原有输出/exit/ENOSYS检查。

### 在启用 Linux 的无 ISO 产品盘手工运行探针

探针只由 debug 自测写入，不自动改动产品 `/bin`。在已初始化且关闭的测试安装盘上：

```bash
make build/linux-l2.elf build/linux-l2-next.elf build/cow-put
python3 scripts/copy-to-disk.py /tmp/my-test.img build/linux-l2.elf /bin/linux-l2.elf
python3 scripts/copy-to-disk.py /tmp/my-test.img build/linux-l2-next.elf /bin/linux-l2-next.elf
```

镜像启动配置需显式含 `capmix.linux=1`；安装布局按 [INSTALL_DISK](INSTALL_DISK.md)，
不要在仍运行的 VM 上导入。启动后：

```text
run /bin/busybox-l1.elf echo l2-persist > /l2-file.txt
run /bin/linux-l2.elf
```

测试文件至少5字节；`/bin/hello.elf` 是既有原生镜像，用来验证跨 persona exec 拒绝。
预期子 exec 后 exit42，由父 wait4 消费，shell最终收到父 `exit=0 reap=ok`。
该探针使用默认控制台 stdio，不给它加 `> file`，避免上文说明的无 DUPLICATE stdio 边界。

## 修改文件与关键路径审查

- `proc/elf.c`、`include/capmix/elf.h`：引用、驻留副本预留、克隆、brk、销毁；
  `proc/uaccess.c`：copyout COW；`mm/commit.c`、`include/capmix/mm.h`：计数同步/溢出与声明。
- `proc/spawn.c`、`include/capmix/proc.h`：Linux fork/wait4、BSP亲和、子现场与生命周期；
  `proc/control.c`：spawn返回实际CPU（原生值不变，Linux BSP覆盖后如实报告）。
- `abi/linux.c`、`abi/linux_process.c`、`include/capmix/linux_process.h`：Linux分流、exec事务及现场布局断言。
- `arch/x86_64/syscall.S`、`usermode.c`：完整用户寄存器捕获/子返回/exec现场与CR3更新；
  `switch.S`、`fault.c`：可恢复用户COW #PF。它们属于 AGENTS.md 的人审关键路径；
  本轮保留 KPTI窗口/双CR3策略、buddy锁和定时器标定，不应把新增返回分支视为无需审查。
- `boot/linux_l2_test.c`、`boot/kernel.c`、`boot/linux_l1_test.c`、
  `tests/linux_l2{,_next}.S`、`tests/check_linux_l2.py`、`Makefile`：测试/生成/回归接线。
- 文档：本页、L0/L1历史说明、文档导航、内核总览、ELF并发与VBox手册。

## 实测矩阵（2026-10-15）

| 环境 | 结果与证据 |
| --- | --- |
| hosted，编译开/关 | 全量 `selftest result: PASS (0 failures)`；`/tmp/linux-l2-hosted-final.log`、`/tmp/linux-l2-hosted-off-final.log` |
| QEMU KVM SMP4 单节点 e1000，Linux开 | 全量PASS，包含真实CPL3 COW/fork/exec/wait和L0/L1；`/tmp/linux-l2-qemu-single-on-final.log` |
| QEMU KVM SMP4 双NUMA、e1000+virtio-net，Linux开 | 全量PASS，含buddy soak、phase0–9、TRACE/EPT/NUMA、文件、UDP/DHCP/TCP/RTO；`/tmp/linux-l2-qemu-on-final.log` |
| QEMU KVM SMP4 编译关/运行时关 | Linux拒绝、原生全量PASS；`/tmp/linux-l2-qemu-compiled-off-final.log`、`/tmp/linux-l2-qemu-runtime-off-final.log` |
| QEMU SMP4 AHCI安装盘，无ISO，Linux开 | 冷启动运行导入的L2探针，真实fork/COW/exec同PID/exit42/wait→父exit0；BusyBox、原生重定向/cat/nc/http-get/httpd、关机PASS；`/tmp/linux-l2-product-on.log`、`/tmp/linux-l2-product-cold.log` |
| 同盘关闭Linux后冷启动 | Linux探针拒绝，原生cat仍读到 `/l2-file.txt` 与 `/native-l2` 的持久内容，原生hello/httpd及ACPI关机PASS；`/tmp/linux-l2-product-off.log` |
| 独立宿主HTTP客户端 | 产品Linux开/关各两次HTTP200、25B正文、EOF验证PASS；`/tmp/linux-l2-httpd-client.log`、`/tmp/linux-l2-httpd-client-off.log` |
| VirtualBox7.2.20 UEFI/AHCI/e1000 NAT SMP4，无ISO | 产品COW写故障、fork→同PID exec→wait42→父exit0/reap、BusyBox cat、原生hello与ACPI关机PASS；`/tmp/linux-l2-vbox-product.log`。未运行VBox全量debug，未将VBox网络记为通过 |

本轮 kernel 编译开/关无新增 warning/error；L0/L1固定ELF哈希校验与L1 allocator测试通过，
`/tmp/linux-l2-fixture-verify.log`。默认最终ISO仍为产品/Linux关闭配置。
以上 `/tmp` 是本次宿主的证据路径，不随源码分发；重新测试应保存自己的日志并运行检查脚本。

审查重点是 syscall完整现场/对齐、#PF错误码与CR3恢复、exec提交后不再访问旧用户指针，
以及预留链 `writable refs - 1` 的释放不变量。通过上述有界测试不代表无限负载或通用Linux兼容。
