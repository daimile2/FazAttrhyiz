# Linux ABI L1：文件映射与定制静态 BusyBox 子集

> 证据归档：见 [tests/evidence 索引与脱敏规则](../tests/evidence/README.md)。矩阵链接为原轮次留存日志的脱敏副本；本次仅归档，未重跑测试。命令中的 `/tmp` 是新运行的输出位置。

后续更新：[L5](LINUX_ABI_L5.md)已新增Linux匿名PRIVATE mmap/munmap、16MiB VA窗口及8MiB映射额度，并细化COW物理页commit归属；下文首版容量/全部mmap拒绝为阶段快照，原生限制不变。

后续 COW fork、有界 brk 与替换式 execve 见 [L2](LINUX_ABI_L2.md)。本文保留本阶段快照；L2 默认关闭且不改变原生 ABI。

L1 是 **「定制 musl 静态 BusyBox 子集 + 有界 BSS 分配」**。支持固定构建的
BusyBox 1.36.0 的 `echo`、`cat`、`ls`，不代表通用 BusyBox、任意 musl 或 glibc
静态程序兼容。本轮没有实现 mmap/brk、fork、execve 替换、socket、futex、动态链接。
原生网络程序继续走 CapMix ABI。

## 双轨、默认关闭和数据策略

轨 A `CM_ABI_CAPMIX` 保留原生 syscall、capability、fdio 与 `/bin` 工具。
轨 B `CM_ABI_LINUX` 仅用于 ELF 明确带 Linux OSABI 或 GNU ABI note 的静态
x86-64 ET_EXEC，syscall 进入独立翻译表，绝不退回同号原生调用。
识别、FS_BASE 与 auxv 沿用 [L0](LINUX_ABI_L0.md)，不改 KPTI、入口切换策略。
有 PT_INTERP、PT_DYNAMIC、PT_TLS 或 ET_DYN 的镜像仍明确拒绝。

产品默认**运行时关闭**，编译默认包含翻译代码：

```bash
make kernel iso                         # 默认产品，无 capmix.linux=1
make iso LIMINE_CONFIG=boot/limine-linux.conf
make iso LIMINE_CONFIG=boot/limine-linux-selftest.conf
make iso LIMINE_CONFIG=boot/limine-linux-off-selftest.conf
make kernel iso LINUX_ABI=0              # 编译关闭，cmdline 无法重新打开
```

`capmix.linux=0` 优先关闭；selftest 默认开启，但编译关闭始终有效。
启用的产品路径只在文件缺失时 provision `/bin/linux-hello.elf` 和
`/bin/busybox-l1.elf`，不覆盖现有同名文件。关闭时拒绝 Linux 镜像的运行，
**不删除、格式化、改名任何原生或 Linux 文件**；不提供 `.o` 热卸载。
升级已有盘上的旧同名 Linux 文件需由用户显式处理，不借开关执行更新。
新原生软件建议 `.cmx` 或 `/bin/cm/`；旧 `.elf` 工具保留，后缀不参与 ABI 判断。

## 文件 syscall 与权限

号表固定参照 Linux **v6.6 x86-64**：
[syscall_64.tbl](https://github.com/torvalds/linux/blob/v6.6/arch/x86/entry/syscalls/syscall_64.tbl)。
Linux 参数按现有入口取 RDI/RSI/RDX/R10 四参数；本子集没有需要第五/第六参数的调用。
Linux 返回成功值或负 Linux errno，不向用户泄露 CapMix 错误号。

| Linux 号 | 调用 | 首版语义 |
| --- | --- | --- |
| 0 / 1 | read / write | FILE cap 的读/写权限；普通控制台 stdin 为 EOF，stdout/stderr 输出原始字节 |
| 2 / 257 | open / openat | 绝对路径，或目录 fd 的相对路径；支持读写、CREAT/EXCL/TRUNC/APPEND、DIRECTORY/CLOEXEC |
| 3 / 8 | close / lseek | 关闭 fd；SEEK_SET/CUR/END，拒绝负结果/溢出；控制台 ESPIPE |
| 4 / 6 / 5 | stat / lstat / fstat | 显式转换成 Linux stat；无符号链接，stat 与 lstat 同义 |
| 262 | newfstatat | 目录相对路径，AT_SYMLINK_NOFOLLOW、AT_EMPTY_PATH；其他标志 EINVAL |
| 217 | getdents64 | 8 字节对齐记录，包含 . / ..，独立于 CapMix 原生 dirent 布局 |
| 32 / 33 | dup / dup2 | FILE 必须有 DUPLICATE right；保留原 rights，重复 fd 共享 FILE 偏移 |
| 72 | fcntl | DUPFD / DUPFD_CLOEXEC、GETFD/SETFD、GETFL；无锁/SETFL 等全集 |
| 19 / 20 | readv / writev | 最多 16 个 iovec，总长度最多 1 MiB，先检查全部用户区间 |
| 39 / 79 / 63 | getpid / getcwd / uname | proc pid；固定 cwd=/；如实报告 sysname=CapMix |
| 102/104/107/108 | uid/gid/euid/egid | 固定返回 0，仅身份兼容值，不能获得新的 capability |
| 16 | ioctl | 有效 fd 返回 ENOTTY，无效 fd EBADF；不假装实现终端 ioctl |
| 218 | set_tid_address | 单线程启动兼容，仅返回 pid；不实现 clear_child_tid 通知或 futex |
| 231 / 60 | exit_group / exit | 单线程退出、关闭 FILE/cap 并正常回收镜像 |
| 158 | arch_prctl | L0 SET_FS / GET_FS；无 TLS 分配器或线程支持 |

所有路径与数据通过 copyin/copyout/ELF 权限验证；禁止写只读镜像页和访问内核地址。
单次 I/O 最多 **1 MiB**，超限或不可访问区间返回 EFAULT；不把超限请求偷偷拆成
绕过限制的调用。路径含 NUL 最多 128 字节；组件沿用 COWFS 上限。
每进程 **32 个 fd（含 stdio）**，满则 EMFILE。openat 不引入任意内核对象访问：
必须从本进程目录 FILE cap 取路径；dup 不增加 rights。

cwd 固定 `/`，没有 chdir；`.`/`..` 做词法规范化，无 symlink。
NOFOLLOW 被接受是因为当前命名空间没有 symlink，不能据此宣称实现 symlink 安全语义。
创建 mode/umask 不实施完整 POSIX ACL；stat 的目录/文件 mode 为 0755/0644，
uid/gid/时间戳为 0，底层权限仍以 capability 为准。控制台为模拟字符设备 stat。
CLOEXEC 可设置/读取，但本轮没有替换式 execve，因而没有跨 exec 的演示。
目录 cookie 是有界索引；并发目录改名/变更下不保证 Linux 全部细节。

未实现调用统一 `-ENOSYS` 并增加每进程计数，退出日志包含 `ENOSYS=N`。
L0 的未知调用探针已从现已实现的 nr=2 改为保留的 0x40000000；固定 write/exit
黄金 ELF 字节和哈希不变。`linux_calls` 是翻译层调用诊断数，writev 内部转 write
也计数，不能当作精确硬件 SYSCALL 次数。

## Linux 布局来源与编译期校验

`include/capmix/linux_uapi.h` 使用显式 32/64 位成员，不直接复用宿主 libc struct。
固定定义来源：

- [v6.6 arch/x86/include/uapi/asm/stat.h](https://github.com/torvalds/linux/blob/v6.6/arch/x86/include/uapi/asm/stat.h)：stat 为 144B，mode=24、size=48、blocks=64、atime=72 等。
- [v6.6 include/linux/dirent.h](https://github.com/torvalds/linux/blob/v6.6/include/linux/dirent.h)：dirent64 的 off=8、reclen=16、type=18、name=19。
- [v6.6 include/uapi/linux/uio.h](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/uio.h) 与 [utsname.h](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/utsname.h)：iovec=16B、utsname=390B。

头文件用 `_Static_assert` 校验大小和上述成员偏移；结构转换零初始化 padding，
用户不会读到内核栈残留。getdents64 记录逐条对齐，不把 flexible array 的 sizeof 当记录长度。

## 固定程序、分配上限与构建

文件：`tests/fixtures/busybox-l1.elf`，SHA256：

```
1d7b09819dabade79f19a3f5001d4fb0c89e40e055a45c76de6b897fb4850fbf
```

对应完整源码归档在 `third_party/linux-l1/`，不依赖宿主随手编译的 libc：

| 组件 | 版本 | 上游归档 SHA256 |
| --- | --- | --- |
| BusyBox | 1.36.0 | 542750c8af7cb2630e201780b4f99f3dcceeb06f505b479ec68241c1e6af61a5 |
| musl | 1.2.5 | a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4 |

来源、GPL/MIT 许可说明见 [源码目录](../third_party/linux-l1/README.md)，许可正文保留
在完整归档中。定制内容包括配置、BSS 分配器和 musl integer-only printf 补丁，
均随仓库提供。正常 kernel/ISO 构建只检查固定 ELF 哈希并嵌入，不需要联网构建 BusyBox。

```bash
sha256sum -c third_party/linux-l1/SHA256SUMS
sha256sum -c tests/fixtures/busybox-l1.sha256
make linux-l1-verify  # 附带 allocator 耗尽/保留旧数据/合并/溢出/清零测试
scripts/build-busybox-l1.sh /tmp/capmix-l1-fresh-build
cmp tests/fixtures/busybox-l1.elf /tmp/capmix-l1-fresh-build/busybox-l1.elf
```

目录必须全新；需要 gcc/binutils/make/patch/python3/tar/bzip2。验证工具链为 GCC 13.3.0；
不同编译器可能产出不同字节，不应自动覆盖固定 fixture/hash。
脚本固定时间戳、静态链接、max-page-size=4096，显式写 EI_OSABI=3，保留 musl 的
crt、stdio、FS 启动代码。只启用 echo/cat/ls，不含 shell、fork 或外部命令调度。

`user/linux/bss_alloc.c` 每进程预留 **65536B BSS arena**，16B 对齐，头部消耗包含在内，
first-fit/split/coalesce；malloc/calloc/realloc/free 及 musl 内部入口均走该 arena。
超限返回 NULL/ENOMEM，不借 brk/mmap 扩容。它是单线程、有限目录规模的测试配置，
不能支撑任意大目录、任意 applet 或线程。程序映射仍受现有 ELF 128 页总上限约束。

为适应当前用户上下文，不启用内核 SIMD：选定 libc C 单元与 BusyBox 用
`-mgeneral-regs-only`，printf 补丁移除浮点格式，浮点格式不支持；浮点扫描/数学库的
未使用代码可能仍在二进制中，不能调用它们。**没有承诺一般用户 FPU/SIMD 上下文支持**。
无 PT_TLS；musl 使用内置单线程 TCB，经 SET_FS 建立 FS。AT_RANDOM 等仍由 L0 提供。

## 使用、回归与验收日志

启用 Linux 的产品 shell（可安装到盘后无 ISO 使用）：

```
run /bin/linux-hello.elf
run /bin/busybox-l1.elf echo linux-l1
run /bin/busybox-l1.elf cat /some-file
run /bin/busybox-l1.elf ls /
run /bin/busybox-l1.elf echo persisted > /l1-saved
run /bin/busybox-l1.elf cat /l1-saved
```

只演示这三个 applet；不宣称 BusyBox shell、ls 全选项、管道或任意 applet 兼容。
原生 shell 重定向负责 FILE stdio 绑定，Linux write 不绕过这些 rights。

```bash
make test
make kernel 2>&1 | grep -iE 'warn|error'
make iso LIMINE_CONFIG=boot/limine-linux-selftest.conf
# 需要原有UDP/TCP回归服务时先运行 scripts/{udp,tcp}-echo.py
# 完整QEMU参数可用现有串口路径，增加 -netdev user,id=n0 -device e1000,netdev=n0
python3 tests/check_linux_l1.py /tmp/linux-l1-qemu-on-final.log
python3 tests/check_linux_l1.py --disabled /tmp/linux-l1-qemu-compiled-off-final.log
```

`boot/linux_l1_test.c` 逐字节核对 echo22B/cat21B/ls11B 和 exit=0、ENOSYS=0；
还验证 EFAULT、flags/目录错误、只读 cap、dup 共享偏移、console alias、openat、stat144、
dirent64 小缓冲回滚、结束 cookie、mmap/brk ENOSYS、fd/页回收。hosted 验证桥接结构，
明确跳过实际 CPL3 applet；实际执行由 QEMU 验证。

关键字：`linux L1 fd verified`、`linux L1 busybox echo/cat/ls: CPL=3`、
`verified exit=0 ENOSYS=0 BSS-heap=65536`、`linux L1 selftest: PASS`。
全量仍以 `selftest result: PASS (0 failures)` 为准；关闭日志为
`linux L1 skipped: persona disabled; native files retained`。

环境矩阵与本轮实测结果见下方记录。L2–L4 的地址空间扩展、fork/exec、网络转译、
动态加载均是后续工作，没有因本轮文件调用映射而获得。

## 本轮实测矩阵（2026-10-15）

| 环境 | 结果与证据 |
| --- | --- |
| hosted，编译开 / 关 | 全量 `PASS (0 failures)`；[linux-l1-hosted-final.log](../tests/evidence/logs/linux-l1-hosted-final.log)、[linux-l1-hosted-off-final.log](../tests/evidence/logs/linux-l1-hosted-off-final.log) |
| QEMU KVM SMP4 单节点，Linux 开，e1000 | 全量 PASS，L0 与 BusyBox 三 applet 字节/退出断言；[linux-l1-qemu-single-on-final.log](../tests/evidence/logs/linux-l1-qemu-single-on-final.log) |
| QEMU KVM SMP4 双 NUMA、e1000+virtio-net，Linux 开 | 全量 PASS；TRACE、EPT、IRQ、sign、NUMA/UDP/TCP/RTO 同跑；[linux-l1-qemu-on-final.log](../tests/evidence/logs/linux-l1-qemu-on-final.log) |
| QEMU KVM SMP4，编译关 / 运行时关 | Linux 拒绝、原生全量 PASS；[linux-l1-qemu-compiled-off-final.log](../tests/evidence/logs/linux-l1-qemu-compiled-off-final.log)、[linux-l1-qemu-runtime-off-final.log](../tests/evidence/logs/linux-l1-qemu-runtime-off-final.log) |
| QEMU SMP4 AHCI 无 ISO 产品盘，开→关冷启动 | BusyBox echo/cat/ls、重定向、原生 hello/cat/nc/http-get、httpd 两次响应、ACPI关机 PASS；关闭后 Linux 拒绝，原生 cat 仍读到两份持久文件；[linux-l1-product-on.log](../tests/evidence/logs/linux-l1-product-on.log)、[linux-l1-product-off.log](../tests/evidence/logs/linux-l1-product-off.log) |
| 宿主 httpd 独立客户端 | Linux开/关产品路径均两次 HTTP200、25B、EOF 校验 PASS；[linux-l1-httpd-client.log](../tests/evidence/logs/linux-l1-httpd-client.log)、[linux-l1-httpd-client-off.log](../tests/evidence/logs/linux-l1-httpd-client-off.log) |
| VirtualBox 7.2.20 UEFI/AHCI/e1000 NAT SMP4，无 ISO | L0、BusyBox echo/cat/ls、重定向、原生 hello/cat、ACPI关机 PASS；冷启动再读回 `/vbox-l1`。产品文件测试，不是 VBox 全量 debug。[linux-l1-vbox-firstboot.log](../tests/evidence/logs/linux-l1-vbox-firstboot.log)、[linux-l1-vbox-coldboot.log](../tests/evidence/logs/linux-l1-vbox-coldboot.log) |
| VirtualBox 网络 | 本轮新建 VM 启动 e1000 `link=down`，因此网络未验，不记 PASS；未为此修改网卡核心。网络原生回归证据来自上述 QEMU 产品路径 |
| 离线重建 / BSS 上限 | 第二个全新目录产物与 fixture 逐字节相同；[linux-l1-repro-verify.log](../tests/evidence/logs/linux-l1-repro-verify.log)。allocator 耗尽、失败保留旧块、合并、溢出与清零测试 PASS；[linux-l1-verify.log](../tests/evidence/logs/linux-l1-verify.log) |

以上矩阵证据已随仓库脱敏归档；重新验收应另存自己的串口日志。
文件 I/O 断言由 guest selftest 实际检查内容；不是仅凭出现路径名判定通过。

## 修改文件

- 翻译与布局：`abi/linux.c`、`abi/linux_file.c`、`include/capmix/{abi,linux_uapi,proc}.h`。
- FILE 桥接与创建：`proc/{syscall,spawn,init}.c`、`fs/vfs.c`、`include/capmix/fs.h`。
- 回归接线：`boot/{kernel,linux_test,linux_l1_test}.c`、`tests/linux_probe.S`、
  `tests/{check_linux_l1.py,linux_bss_alloc.c}`。
- 固定程序与可复现来源：`tests/fixtures/busybox-l1.{elf,sha256}`、
  `user/linux/{busybox.config,bss_alloc.c,musl-integer-printf.patch,musl-scalar-units.txt}`、
  `third_party/linux-l1/`、`scripts/build-busybox-l1.sh`、`Makefile`、`.gitignore`。
- 文档：本文、`LINUX_ABI_L0.md`、`README.md`、`KERNEL_OVERVIEW.zh-CN.md`、
  `EXTERNAL_ELF.md` 与 VirtualBox 手册。
