# 可复现静态 BusyBox sh：非交互 hush 子集

> 证据归档：见 [tests/evidence 索引与脱敏规则](../tests/evidence/README.md)。矩阵链接为原轮次留存日志的脱敏副本；本次仅归档，未重跑测试。命令中的 `/tmp` 是新运行的输出位置。

本轮验收对象是固定的 **BusyBox 1.36.0 + musl 1.2.5 静态 hush 子集**，
不是通用 BusyBox、glibc、完整 POSIX shell 或任意 Linux 二进制兼容性承诺。
产品 Linux persona 默认关闭；仅明确标记的 Linux ELF 走翻译路径。原生 init/shell、
`/bin/*.elf`、未来 `.faz` / `/bin/cm/` 约定不变；关闭/卸载 Linux 不删除原生工具。

L1 的 `busybox-l1.elf` 及其**定制 musl 静态 BusyBox 子集 + 有界 BSS 分配**仍原样保留。
新 `busybox-sh.elf` 使用真实 musl malloc/brk/mmap，不覆盖 L1 fixture/分配器。
L5 mmap/COW commit、KPTI、buddy锁、时钟、TCP/RTO、安装布局核心未修改。

## 固定构建与许可

| 项 | 固定来源 |
| --- | --- |
| BusyBox | `third_party/linux-l1/busybox-1.36.0.tar.bz2`，GPL-2.0-only；归档内 LICENSE/COPYING/文件通知 |
| musl | `third_party/linux-l1/musl-1.2.5.tar.gz`，MIT及文件通知；归档内 COPYRIGHT |
| 源码哈希 | `third_party/linux-l1/SHA256SUMS`，构建时校验；正常内核构建不联网 |
| 配置 | `user/linux/busybox-sh.config`；hush、echo/cat/ls/true/false，关闭作业控制/交互模式/陷阱等 |
| musl补丁 | 复用 `user/linux/musl-integer-printf.patch`：整数输出，无浮点printf支持 |
| BusyBox补丁 | `user/linux/busybox-sh-signals.patch`：sigaction失败时返回默认动作，不读取未初始化old action；并不实现信号投递 |
| 标量构建 | musl选定单元沿用 `musl-scalar-units.txt`；BusyBox的 `CONFIG_EXTRA_CFLAGS` 强制无red-zone/stack-protector、general-regs-only |
| 固定 ELF | `tests/fixtures/busybox-sh.elf`，SHA-256见相邻 `busybox-sh.sha256`；已去符号，OSABI明确标为Linux |

上游BusyBox会编译未被子集链接的浮点dump.o，因此仅为该未链接对象加 `-msse2`。
脚本反汇编最终ELF并拒绝出现XMM/YMM/ZMM及fld/fst指令，不把编译未用对象误当成
运行时浮点能力。没有扩大内核FPU上下文策略。复现用Ubuntu GCC13.3.0、GNU binutils2.42，
SOURCE_DATE_EPOCH=0、TZ=UTC；不同编译器版本可能得到不同二进制。

```bash
scripts/build-busybox-sh.sh /tmp/capmix-sh-fresh
sha256sum -c tests/fixtures/busybox-sh.sha256
cmp /tmp/capmix-sh-fresh/busybox-sh.elf tests/fixtures/busybox-sh.elf
```

脚本检查源归档哈希、应用上述补丁、固定配置、构建并审计最终指令，不自动替换fixture。
分发此ELF时一并保留源码归档、配置、补丁、脚本及上游版权/许可证；本仓库已包含对应源码。
不需要外部下载服务。SHA固定的是本定制子集，不能拿宿主发行版的任意BusyBox替换。

## 验收覆盖

`boot/linux_sh_test.c`以真正Linux argv传入 `{"...busybox-sh.elf","sh","-c",script,NULL}`。
通过capability绑定捕获stdout，对字节内容、退出码、回收基线做断言：

| 用例 | 验证 |
| --- | --- |
| basic | 变量赋值/展开、echo、if/true，准确输出25字节 |
| subshell | `(x=child; echo $x); echo $x`，子shell不改变父变量，13字节 |
| exec | 绝对路径调用同一个Linux BusyBox的echo，父随后继续执行；fork/exec/wait，18字节 |
| pipe | `echo pipe-ok \| /bin/busybox-sh.elf cat`，8字节 |
| stream | 两个外部cat经2048字节pipe传递6000字节，跨越缓冲容量，逐字节比较 |
| missing | 不存在的绝对路径返回127；stderr有明确No such file诊断，stdout为空 |
| exit | `exit 7`准确返回7 |

每个CPL3用例最多5秒；失败清理其直接子任务。正常路径结束后必须恢复页数+缓存、commit、
打开FILE数及pipe槽基线。selftest不把无限挂起当成功，也不只凭“屏幕上有echo”判定。
hosted验证pipe机制/fixture供应，实际hush执行只由freestanding或产品路径验证。

## Linux接口补充与错误

- getppid(110)：返回已记录的父PID；内核直接spawn时可能为0。
- wait4(61)：接受WNOHANG=1和WUNTRACED=2。当前无停止/继续信号事件，仍只返回真实退出状态；
  其他选项拒绝。是否非阻塞只由WNOHANG决定，不把WUNTRACED误作非阻塞。
- pipe(22)、pipe2(293)：读/写端独立capability，依各自READ/WRITE/DUPLICATE right门控；
  fork/dup共享同一endpoint的最后引用生命周期。类型加在枚举末尾，旧对象编号不变。
- Linux的stdio绑定仅在可信源对象本来授予DUPLICATE时保留此right，供fork及重定向使用。
  原生绑定仍按原规则衰减为READ或WRITE；非duplicable FILE的fork仍拒绝，并保留L2负例。

Pipe首版语义：

| 项目 | 行为 |
| --- | --- |
| 容量 | 全局最多8条、每条2048字节，实际一个buddy页含数据/元数据；受fd/handle/object总槽位共同限制 |
| atomic/读长度 | 写入≤512字节为一个原子块；更大写拆块，可部分完成。单次read最多512字节，允许短读 |
| 等待 | 默认yield/recheck直至可收发、EOF或kill；没有新建事件等待队列，也没有普通调用超时 |
| 非阻塞 | pipe2 O_NONBLOCK=0x800；支持fcntl GETFL/SETFL；空/满返回EAGAIN(-11)，已有进度返回部分字节数 |
| close | 最后writer关闭，读完存量后EOF=0；最后reader关闭，write返回EPIPE(-32)，**无SIGPIPE投递** |
| CLOEXEC | pipe2 O_CLOEXEC=0x80000；沿用现有fcntl及exec关闭机制 |
| 错误 | 错方向/无right EBADF(-9)，用户地址EFAULT(-14)，fd不足EMFILE(-24)，pipe全局上限ENFILE(-23)，物理不足ENOMEM(-12)，不支持flags EINVAL(-22) |
| 其他 | fstat返回Linux FIFO布局，seek为ESPIPE；readv/writev/dup/close复用fd桥，最后close/kill退出释放物理页 |

机制测试覆盖空读EAGAIN、方向right拒绝、dup共享、坏指针不消耗数据、EOF、EPIPE、
全局8槽耗尽及回收。不是完整Linux pipe产品语义；不支持poll/select/epoll或SIGPIPE。

## 诚实限制

- hush验收使用AP固定执行位置、单线程路径（见 [Linux SMP](LINUX_SMP.md)）；16MiB VA窗口、8MiB合计映射额度、8192个全局物理backing/reserve槽。
  栈仍4页，argv≤32、单参数≤256字节、总参数4KiB；受32个proc槽和当前64个fd等资源限制（[FD64](FD_TABLE.md)）。
- COW页/reserve仍驻留，不参与压缩/swap，no-overcommit纪律完全沿用L5。
- 13/14现有有限元数据支持，详见 [ENOSYS兼容范围](LINUX_ENOSYS.md)。
  不提供SIGCHLD处理器执行、信号投递、trap、Ctrl-C、作业控制或futex/线程。
  固定七组脚本现要求ENOSYS=0；自定义handler仍明确EOPNOTSUPP。
  单进程日志前8次限流，但按号汇总不再受限流影响。
- cwd仍固定`/`，没有新增chdir、完整access/权限检查、symlink、/proc/self/exe、文件mmap或动态链接。
  本轮不承诺cd、任意脚本、shell交互会话、通用glibc或真实ld.so。
- Linux execve只接Linux persona ELF。不要从hush直接exec原生CapMix `.elf`；使用
  `/bin/busybox-sh.elf cat/echo/ls` 等Linux子命令。原生shell及工具依旧走轨A。
- 原生shell仍按空白拆argv，不新增引号解析。因此产品中不要把含空格的 `sh -c '...'`
  原样粘到原生shell；用下面的脚本文件路径。内核selftest则直接传入完整argv验证`sh -c`。

## 产品与全量测试

```bash
make iso LIMINE_CONFIG=boot/limine-linux.conf  # 产品Linux开启；默认make iso仍关闭
# 或 scripts/install-disk.py /tmp/new-empty.img --linux，再从盘无ISO启动
```

仅Linux开启时bootstrap补缺失的 `/bin/busybox-sh.elf` 与 `/data/linux/sh-smoke.sh`；
后者源于 `user/linux/sh-smoke.sh`，包含子shell/外部exec/pipe、原生示例文件读、Linux重定向写读。
不覆盖用户已有脚本；已有安装盘若保存旧脚本，须自行更新该文件。

在原生产品shell输入：

```text
run /bin/busybox-sh.elf sh /data/linux/sh-smoke.sh
run /bin/cat.elf /data/linux/sh-output.txt
```

预期看到 `sh-product-start`、child/parent、`sh-exec-ok`、`sh-pipe-ok`、
`sh-redirect-ok`、`sh-product-ok`，shell报告exit=0 reap=ok。
关机冷启动后原生cat仍读到 `sh-redirect-ok`。Linux关闭不删除这些文件，运行LinuxELF会被拒绝；
仍可用原生cat读取其数据。卸载策略见 [LINUX_PACKAGE](LINUX_PACKAGE.md)，不做内核热卸。

```bash
make test
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
# 开宿主TCP9998/UDP9999/HTTP8000测试端，再按仓库QEMU smp4路径运行
python3 tests/check_linux_sh.py serial.log --wire
# 普通Linux自测配置未开L4a时加 --interp-off；hosted加 --hosted
make iso LIMINE_CONFIG=boot/limine-linux-off-selftest.conf
make test LINUX_ABI=0
make iso LINUX_ABI=0 LIMINE_CONFIG=boot/limine-linux-off-selftest.conf
# 关闭矩阵检查加 --disabled
```

宿主checker递归检查L0–L5旧断言，再核对所有sh用例字节/状态日志、错误127及6000字节流。
局部PASS不能代替 `selftest result: PASS (0 failures)`。

## 环境与验收记录

2026-10-16，QEMU8.2.2/KVM、CPU host、512MiB、SMP4；VBox7.2.20。

| 环境 | 实际结果 | 归档日志 |
| --- | --- | --- |
| 两个干净构建目录 | cmp逐字节一致，SHA-256 `83576b81f0972e75676b1e595d6e0bd403ce5ef71bbd08fbeefdf283a528e050` | [sh-repro.log](../tests/evidence/logs/sh-repro.log)、[sh-repro2.log](../tests/evidence/logs/sh-repro2.log) |
| hosted Linux开/编译关 | 全量PASS (0 failures)；机制测试通过，实际ELF执行不在hosted中模拟 | [sh-hosted-final.log](../tests/evidence/logs/sh-hosted-final.log)、[sh-hosted-off.log](../tests/evidence/logs/sh-hosted-off.log) |
| QEMU Linux开、e1000单节点 | 全量PASS (0 failures)，sh各用例及L0–L5/原生链checker通过 | [sh-on-e1000-matrix.log](../tests/evidence/logs/sh-on-e1000-matrix.log) |
| QEMU Linux开、双NUMA节点，两网卡/virtio优先 | 全量PASS (0 failures) | [sh-on-dual-vnet-matrix.log](../tests/evidence/logs/sh-on-dual-vnet-matrix.log) |
| QEMU Linux开、无网卡 | 全量PASS (0 failures)，网络按既有skip规则 | [sh-on-no-nic-matrix.log](../tests/evidence/logs/sh-on-no-nic-matrix.log) |
| QEMU 运行时Linux关闭、e1000 | 全量PASS (0 failures)，原生保留 | [sh-linux-off-matrix.log](../tests/evidence/logs/sh-linux-off-matrix.log) |
| QEMU LINUX_ABI=0、双节点两网卡 | 全量PASS (0 failures) | [sh-compiled-off-matrix.log](../tests/evidence/logs/sh-compiled-off-matrix.log) |
| 无ISO QEMU产品盘 | sh-smoke脚本、原生cat/重定向、nc、http-get、httpd两次200响应、poweroff通过 | [sh-product-installboot.log](../tests/evidence/logs/sh-product-installboot.log)、[sh-httpd-client.log](../tests/evidence/logs/sh-httpd-client.log) |
| QEMU同盘冷启动 | 读回sh-redirect-ok，脚本再次exit=0并关机 | [sh-product-second.log](../tests/evidence/logs/sh-product-second.log) |
| 同盘副本离线禁用Linux后启动 | 原生cat仍读回数据、hello exit=0；Linux sh被拒绝，shell error=-7；可关机 | [sh-product-off.log](../tests/evidence/logs/sh-product-off.log) |
| QEMU默认产品ISO | Linux enabled=0、原生shell/hello/ACPI关机通过 | [sh-default-product.log](../tests/evidence/logs/sh-default-product.log) |
| VBox EFI64/PIIX3/AHCI/SMP4，无ISO | sh-smoke含子shell/exec/pipe/重定向、原生cat、L0 ELF exit=0及ACPI关机通过 | [sh-vbox-first.log](../tests/evidence/logs/sh-vbox-first.log) |

VBox第二次冷启动也由原生cat读回sh-redirect-ok，脚本再次exit=0并关机；日志 [sh-vbox-second.log](../tests/evidence/logs/sh-vbox-second.log)。
VBox本轮为产品路径验收，不声称全量debug PASS；网卡状态down，没有宣称VBox网络通过。
QEMU网络回归使用本机TCP9998/UDP9999/HTTP8000测试端及httpd端口转发18081→8080。
所有QEMU全量日志均经 `tests/check_linux_sh.py` 检查；Linux开启的两网卡场景加--wire。
内核/ISO构建无新增warning/error；hosted保留原有arch硬件桩unused警告。

## 修改文件

- `abi/linux_pipe.c`、`include/capmix/linux_pipe.h`、`include/capmix/cap.h`、`cap/object.c`：有界pipe、endpoint capability及名称。
- `abi/linux.c`、`abi/linux_file.c`、`proc/syscall.c`：pipe号表/read/write/dup/stat/close桥、getppid、有限ENOSYS号日志。
- `proc/spawn.c`：仅Linux stdio在源已授予时保留DUPLICATE；wait4支持WUNTRACED但不伪造停止事件；fork失败日志。
- `boot/linux_sh_test.c`、`boot/kernel.c`、`include/capmix/abi.h`、`tests/check_linux_sh.py`：机制、实际CPL3、输出/状态/回收验收及旧链回归。
- `scripts/build-busybox-sh.sh`、`user/linux/busybox-sh.config`、`user/linux/busybox-sh-signals.patch`、`tests/fixtures/busybox-sh.*`：源码配方、定制补丁、固定ELF/哈希和许可说明。
- `user/linux/sh-smoke.sh`、`proc/init.c`、`Makefile`、`.gitignore`：构建/嵌入及仅Linux开启时补产品脚本与ELF。
- 本文、总览/导航、L5文档和VBox手册：边界、复现、产品用法和矩阵。

线程续作：已有 [有界clone/futex](LINUX_THREADS.md)。本文单线程/不支持线程的说明为此前范围；
新子集仍不等于通用pthread，线程组内fork/exec/映射变更仍拒绝；fd仅放宽[FILE/console dup/close白名单](LINUX_THREADS.md#受控fd白名单与仍冻结范围)，未创建线程的原路径保留。
