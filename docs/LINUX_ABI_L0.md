# Linux ABI L0：可关闭的静态 persona，与原生 ABI 并存

后续更新：[L5](LINUX_ABI_L5.md)已新增Linux匿名PRIVATE mmap/munmap、16MiB VA窗口及8MiB映射额度，并细化COW物理页commit归属；下文首版容量/全部mmap拒绝为阶段快照，原生限制不变。

后续 COW fork、有界 brk 与替换式 execve 见 [L2](LINUX_ABI_L2.md)。本文保留本阶段快照；L2 默认关闭且不改变原生 ABI。

后续文件映射与定制 BusyBox 子集见 [L1](LINUX_ABI_L1.md)。本文保留 L0 验收快照；
L1 已实现 open(nr=2)，未知调用探针改用保留号 0x40000000，黄金 write/exit ELF 不变。

本轮只实现一个明确标记的静态 x86-64 Linux ET_EXEC 进程的 write、exit 和
arch_prctl。它不是 Linux 内核、不是容器，也不意味着任意 `gcc -static` 的 glibc
程序已经可用。内核仍为 capability 模型，原生 syscall 号、现有 `/bin/*.elf` 工具、
驱动和网络栈继续保留。

## 双轨与镜像识别

| 轨 | 进程 ABI | syscall 路径 |
| --- | --- | --- |
| A：CapMix | `CM_ABI_CAPMIX` | 原生 capability/fdio/control 分发；旧工具保持原行为 |
| B：Linux L0 | `CM_ABI_LINUX` | 独立 `abi/linux.c` 翻译表；不回落到同号原生调用 |

persona记录于ELF image和proc，在进程发布之前确定，用户程序不能通过一个syscall改变它。
识别是ELF内容证据，不依赖文件扩展名、路径、反汇编猜测或“第一次syscall是什么”：

- `EI_OSABI=3`（Linux）或者 `PT_NOTE` 内的 `GNU\0 / NT_GNU_ABI_TAG=1`、
  16字节descriptor的OS字段=0（Linux）标记为Linux。
- 历史 `EI_OSABI=0`、没有Linux note的镜像继续为原生；**无标记的SYSV Linux ELF
  也不会被自动猜成Linux**，必须使用明确Linux标记后才可运行。
- 支持显式原生note：name=`CapMix\0`，namesz=7，type=1，4字节descriptor=0。
  与Linux标记冲突时拒绝；其他不支持的OSABI/已知GNU OS标记拒绝。
- NOTE逐项校验长度、4字节对齐和文件范围，每个PT_NOTE最多4096B，PHDR最多16个。
- 有PT_INTERP的ET_EXEC/ET_DYN都明确拒绝并记录日志；PT_DYNAMIC/PT_TLS仍拒绝，
  ET_DYN/PIE仍不支持。不尝试加载ld.so，不把动态Linux镜像当原生运行。

新CapMix原生软件建议采用 `.cmx` 后缀或 `/bin/cm/` 路径；这只是发布命名约定，
不用于ABI识别。本轮不重命名、删除或迁移已有 `.elf` 工具。

## 构建与开关

编译默认 `LINUX_ABI=1`（包含翻译代码），但**产品默认运行时关闭**：

```bash
make kernel iso
# 明确启用Linux的产品ISO
make iso LIMINE_CONFIG=boot/limine-linux.conf
# 明确启用、带网络echo/RTO回归的debug ISO
make iso LIMINE_CONFIG=boot/limine-linux-selftest.conf
# 编译移除Linux syscall处理表；即使cmdline要求启用也不能打开
make kernel iso LINUX_ABI=0
```

对应宏为 `CM_LINUX_ABI`。Makefile的配置stamp保证切换0/1时重新构建两个kernel，
不要求手动clean。默认产品 `boot/limine.conf` 不含启用参数；
`capmix.linux=1`开启，`capmix.linux=0`优先关闭。debug/selftest默认开启用于回归，
hosted是selftest环境；编译关闭时始终关闭。
`boot/limine-linux-off-selftest.conf`用于编译包含但运行时关闭的完整回归。

关闭后只拒绝Linux镜像加载，不删除根上的任何原生文件，也不删除已经安装的Linux文件。
产品只有启用时才provision缺失的 `/bin/linux-hello.elf`；不覆盖同名文件。
不实现 `.o` 热卸载。编译关闭仍保留ELF识别/拒绝及结构测试，FS隔离作为线程上下文机制保留。

## 固定黄金 ELF 与 guest 用法

仓库固定文件：`tests/fixtures/linux-l0.elf`，SHA-256清单为同目录 `linux-l0.sha256`。
来源为 `tests/linux_hello.S` 和 `tests/linux_hello.ld`；手写 `_start` 仅调用 Linux
write(1) 和 exit(60)，不用glibc，也不使用CapMix syscall号。
构建直接校验并嵌入该固定字节。当前工具链可独立重建核对：

```bash
sha256sum -c tests/fixtures/linux-l0.sha256
make linux-golden-verify
```

重建使用固定名称的中间目标文件、无build-id、无libc；不同binutils版本如导致ELF
元数据差异，应先审查差异，不静默替换黄金文件。`scripts/mark-linux.py`仅标记本仓库
已校验的静态x86-64测试镜像；不是任意Linux软件的兼容性认证器。

启用Linux的产品shell：

```text
run /bin/linux-hello.elf
run /bin/linux-hello.elf > /linux-note.txt
cat < /linux-note.txt
run /bin/hello-sdk.elf native-still-works
```

预期输出 `hello from Linux ABI L0` 加换行，write返回24，exit=0、reap=ok。
重定向通过已有FILE对象与权限路径，保留完整24字节；默认终端输出是无诊断前缀的
原始文本，串口仍沿用终端CRLF转换。stdout与stderr继承现有诊断输出权限，未绑定fd0不可写。
未实现Linux open，因此本轮Linux程序只能write已有stdout/stderr或父进程显式委托的FILE fd。

新安装盘无ISO开启Linux：

```bash
make kernel
python3 scripts/install-disk.py /tmp/capmix-linux.img --config boot/limine-linux.conf
```

遵守原安装器的全零/新盘约束，拒绝破坏未知非空盘；布局不变。已有旧内核安装盘需要
先按现有升级/重建流程更新内核，单复制Linux ELF不足以获得新ABI。

## Linux syscall / errno

Linux x86-64寄存器约定沿用现有entry：RAX为号，RDI/RSI/RDX/R10为前4参数。
L0所实现的调用至多3参数，不新增6参数入口或修改syscall汇编/KPTI策略。

| Linux号 | 调用 | L0行为 |
| --- | --- | --- |
| 1 | write(fd, buf, count) | 验证用户范围，FILE路径检查capability WRITE；保留字节，不借用原生诊断截断逻辑 |
| 60 | exit(status) | status取低8位，使用现有退出、cap/网络清理与off-CPU回收 |
| 158 | arch_prctl(code, addr) | SET_FS=0x1002；GET_FS=0x1003 |
| 其他 | 未实现 | 返回Linux `-ENOSYS=-38`，累计本进程计数，不回落原生同号接口 |

未知号不逐次刷屏；退出记录 `linux exit: pid=... write_bytes=... ENOSYS=...`。
测试特别检查Linux号2不会误入CapMix WRITE。
非法fd为EBADF=-9，非法用户缓冲区/GET_FS目标为EFAULT=-14，
SET_FS内核/非低半区地址为EPERM=-1，未知arch_prctl操作为EINVAL=-22。
FILE层cap拒绝映射EACCES=-13，其他内核错误转换为Linux负errno，不直接复用CapMix错误数字。
单次用户I/O范围上限仍1MiB，越界/超上限按EFAULT拒绝；这是L0边界，不是Linux完整write实现。

FS基址允许0及规范低半区地址，不要求SET_FS时已经映射；使用不存在映射是程序自身错误。
GET_FS通过copyout写8字节。上下文切换在中断关闭区域保存/恢复 `MSR_FS_BASE=0xc0000100`，
包括线程与每CPU scheduler，新线程/复用槽清零；未改switch.S寄存器偏移。
hosted只维护slot模型，绝不改宿主libc/pthread的FS寄存器。
这提供FS寄存器隔离，不提供PT_TLS、动态TLS、set_thread_area或pthread运行时。

## Linux 初始栈 / auxv

RSP为16字节对齐，布局为：argc、argv指针及NULL、envp指针及NULL、auxv键值对，
后接字符串和独立16字节随机区。没有main寄存器参数，手写_start从RSP取数据。

| 项 | 值 |
| --- | --- |
| AT_PAGESZ=6 | 4096 |
| AT_PHDR=3 | 可读PT_LOAD中实际程序头表的用户虚址 |
| AT_PHENT=4 | 56 |
| AT_PHNUM=5 | 经验证的PHDR数量 |
| AT_ENTRY=9 | ELF入口 |
| AT_RANDOM=25 | 用户栈中可读的16字节地址 |
| AT_NULL=0 | 值0，终止auxv |

Linux镜像要求PHDR被一个可读PT_LOAD完整覆盖；首段通常使用链接脚本的FILEHDR PHDRS。
不满足则在发布进程前拒绝，不伪造AT_PHDR地址。地址窗口仍为0x400000..0x800000，
上方16KiB为栈，最多128个image页；不是Linux通常的任意用户虚拟地址布局。
原生镜像仍保持原argc/argv/envp与AT_NULL栈布局，不额外塞入Linux auxv。

AT_RANDOM优先CPUID确认后的RDRAND，两份64位成功结果；失败或无能力时用
TSC/原子序号/地址的SHA-256摘要填16字节并明确日志：**该fallback不承诺密码学熵**。
没有新增真KASLR，也不把此随机区当作已完成安全随机数基础设施。

## 验证与后续边界

`boot/linux_test.c`覆盖：OSABI/note/冲突/动态拒绝、禁用拒绝、PHDR/auxv、
Linux错误码及未知号隔离、黄金ELF执行和回收。另一份固定汇编源码
`tests/linux_probe.S`运行两个BSP同CPU进程，选择不同FS基址，在抢占期间反复读FS，
校验执行时间重叠、preempts>0、退出后scheduler_FS=0；每进程恰2次ENOSYS。
hosted验证结构与模型，明确跳过硬件ring3/抢占证明。

```bash
python3 tests/check_linux_l0.py /tmp/linux-l0-qemu-on-final.log
python3 tests/check_linux_l0.py --disabled /tmp/linux-l0-qemu-compiled-off-final.log
python3 tests/check_linux_l0.py --disabled /tmp/linux-l0-qemu-runtime-off-final.log
```

| 环境（2026-10-15） | 结果/日志 |
| --- | --- |
| hosted，编译开/关 | 全量PASS；`/tmp/linux-l0-hosted-final.log`、`/tmp/linux-l0-hosted-off-final.log` |
| QEMU KVM SMP4双NUMA/双NIC，Linux开 | 黄金/FS探针与全量PASS；`/tmp/linux-l0-qemu-on-final.log` |
| QEMU KVM SMP4，编译关 | Linux拒绝、原生全量PASS；`/tmp/linux-l0-qemu-compiled-off-final.log` |
| QEMU KVM SMP4，运行时关 | Linux拒绝、原生全量PASS；`/tmp/linux-l0-qemu-runtime-off-final.log` |
| QEMU SMP4 AHCI无ISO安装盘开→关 | PASS：Linux输出/重定向与关闭拒绝、文件保留，原生hello/cat/httpd/nc/http-get、冷启动读回及关机；`/tmp/linux-l0-product-on-final.log`、`/tmp/linux-l0-product-off-final.log` |
| VirtualBox7.2.20 UEFI/AHCI/e1000 NAT SMP4，无ISO | 产品PASS：Linux黄金与重定向、原生hello/httpd，宿主两次HTTP200校验；非VBox全量debug；`/tmp/linux-l0-vbox-product.log`、`/tmp/linux-l0-vbox-httpd-client.log` |

后续层级只是范围划分，不是已实现的兼容承诺：L1可补Linux fd/文件调用；
L2可补用户内存、brk/mmap和libc所需状态；L3可补socket等网络表面；
L4才讨论PT_INTERP/ld.so/动态链接与更完整运行时。本轮不做Linux fork、exit_group、
mmap/brk、socket、futex/线程、信号、32位或namespaces，也不把原生现有网络API算作Linux socket支持。

## 回归中发现并修复的唤醒交接竞态

早期产品回归曾出现timer iretq的#GP，随后RX worker的park路径出现两个CPU使用同一
栈地址的#PF；原始证据保留于 `/tmp/linux-l0-product-on.log`、
`/tmp/linux-l0-product-off.log`，这两份不能记为PASS。
检查发现wake可在blocker完成上下文保存之前发布RUNNABLE，原选队列只检查state，
另一CPU可提前进入同一continuation。`sched/eevdf.c`仅为普通/aging候选补充已有
跨CPU持有检查，等待原CPU交接结束；没有修改EEVDF权重/预算算法。
新增 `sched early-wake claim verified` 确定性测试，验证持有时拒选、交接后可选。
修复后重跑三组完整debug及产品开/关回归，上表均引用修复后的final日志；
开启态还连续执行6次AP ELF文件重定向并正常关机。

## 修改文件

`abi/linux.c`、`include/capmix/abi.h`：persona开关、翻译表、随机区与内部桥接声明；
`include/capmix/config.h`、`include/capmix/{elf,proc}.h`：开关和image/proc元数据；
`proc/{elf,elf_args,spawn,syscall,init}.c`：识别、auxv、分流、退出统计和产品provisioning；
`arch/x86_64/ctx.c`：FS保存恢复；`sched/eevdf.c`、`include/capmix/sched.h`：最小唤醒交接保护；
`boot/{kernel,linux_test}.c`：回归接线与测试；`boot/limine-linux*.conf`：开关配置；
`tests/linux_{hello,probe}.S`、`tests/linux_hello.ld`、`tests/fixtures/linux-l0.{elf,sha256}`、
`scripts/mark-linux.py`、`tests/check_linux_l0.py`：固定黄金文件、探针和核验；
`Makefile`、`.gitignore`及文档索引/EXTERNAL_ELF/本文。
未修改KPTI策略、buddy锁、时钟标定、TCP RTO/主动连接语义或安装布局。
