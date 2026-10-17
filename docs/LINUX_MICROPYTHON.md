# 静态解释器黄金用例：MicroPython 最小 Linux-user port

这是**定制 MicroPython 1.24.1 静态标量展示子集**，不是通用 MicroPython、CPython、
任意 musl/glibc 或 Linux 发行版兼容。产品默认 Linux persona 关闭；原生 ABI、
`/bin` 工具、hush/ldso/TLS 黄金均保留。原生新软件 `.cmx` 或 `/bin/cm/` 约定不变。
不新增内核热卸或改变 `/data/linux` 保留/显式删除政策。

## 固定来源、配方与补丁

| 项目 | 固定内容 |
| --- | --- |
| 解释器 | MicroPython **v1.24.1**；源码原包在 `third_party/micropython/`，MIT 许可证及源码内 notices 保留 |
| 上游源码 SHA-256 | `a291173de1d680786a66dd2381805b6d134defaecea670a560dc7e7d8de540ad` |
| libc | 已有 musl **1.2.5** 原包；SHA-256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4` |
| ELF | `tests/fixtures/micropython.elf`，**144496 bytes**，静态 x86-64 ET_EXEC，Linux OSABI 显式 marker，无 PT_INTERP/PT_DYNAMIC |
| ELF SHA-256 | `206ad3d7e1957e106541d2f53f1d3a9e22f3b3f89cd7ec0ed5fcbfca110180cf` |
| 构建基准 | GCC **13.3.0**、GNU binutils **2.42**、Python **3.12**，固定 SOURCE_DATE_EPOCH=0 / TZ=UTC / LC_ALL=C / PYTHONHASHSEED=0 |

源码来源：
[MicroPython v1.24.1 原包](https://codeload.github.com/micropython/micropython/tar.gz/refs/tags/v1.24.1)、
[musl 1.2.5](https://musl.libc.org/releases/musl-1.2.5.tar.gz)。
正常 make kernel/iso 只校验并内嵌固定 fixture，不联网、不临时选择宿主 libc。

使用新增的 `user/linux/micropython/` 专用 Linux-user port，**不修改上游 VM、编译器、
parser、GC 源码**。GC 使用上游 `shared/runtime/gchelper_generic.c` 的 x86-64
callee-saved 寄存器与栈扫描，不采用 minimal 示例里仅扫描栈的简化 GC。

musl 在独立目录构建，复用现有 `musl-integer-printf.patch` 和
`musl-scalar-units.txt`，移除浮点 printf、把选定 C 对象重编译为标量；这是定制 libc，
不是“未修改的 musl”。未改 hush/L1 的配方、fixture 或源码归档。
解释器 C 编译使用 `-mgeneral-regs-only -mno-red-zone -fno-tree-vectorize`；
对**最终链接代码**审计 SIMD/x87，不能只凭关闭 float 的宏假定所有 libc 路径安全。

```sh
scripts/build-micropython.sh /tmp/capmix-mp-build1
scripts/build-micropython.sh /tmp/capmix-mp-build2
cmp /tmp/capmix-mp-build1/micropython.elf /tmp/capmix-mp-build2/micropython.elf
cmp tests/fixtures/micropython.elf /tmp/capmix-mp-build1/micropython.elf
make micropython-verify
```

两个独立干净目录重建必须逐字节一致。固定 MICROPY_GIT_TAG=v1.24.1、
MICROPY_GIT_HASH=<no hash>，避免构建目录位于其他Git仓库内时继承其版本标签。配方不会覆盖仓库 fixture；工具链版本不同
可能改变哈希，不能跳过比较或自动改写黄金哈希。完整源码约7.6MiB、fixture约142KiB。

## 语言及资源边界

| 支持 | 限制/关闭 |
| --- | --- |
| Python 源码解析→字节码→执行；print、整数算术、assert、简单循环/函数/列表 | 不承诺完整 Python 语法、标准库或第三方包 |
| 整数 MPZ，可在固定 GC 堆内计算 `2**80` | 禁用浮点、复数；浮点字面量明确报错，不修改内核 FPU/SIMD 策略 |
| builtins、sys、sys.exit；测试退出码0/7 | 不提供 os/socket/ssl、外部 `.py` import、扩展模块、pip、线程、JIT/native/viper、REPL |
| 静态 BSS 中 **256KiB GC 堆**；自动回收可达对象 | 无动态扩堆；不是 L1 的 malloc 替换器，不修改 L5 mmap/commit |
| **8KiB** 源码缓冲，`-c` 或一个脚本文件路径 | guest `-c` 仍受 ELF 单字符串255字节限制；长脚本用文件；不接受脚本附加参数、不提供 sys.argv；文件含NUL拒绝 |
| 既有16KiB用户栈中设置 **8KiB stack-check budget**，其中1024字节检查余量 | 不是扩栈；可用递归深度依编译布局变化，超限报 RuntimeError |
| 当前静态镜像加载 **106页**，含BSS及4页栈 | 仍受 Linux16MiB VA窗口、8MiB payload、驻留/no-overcommit 限制；不提供无限GC/深递归 |

异常 traceback 使用 `<stdin>` 作为源码标签，即使来源是脚本文件。该标签不承诺
完整 Python 调试信息。语法/运行时/内存/递归异常返回1；命令行错误64，源码过大或
含NUL为65，文件打开失败66，I/O失败74，未处理的内部 fatal 为70。
`sys.exit`只承诺本轮测试的简单整数/None语义，不承诺各种异常对象或超大退出值兼容。

ELF 参数仍为32个 argv/32个 env、每串含NUL至多256字节、合计4096字节；未扩容 argv。
首版单线程运行，不安装用户信号处理器、不启用语言线程，不新增 pthread/futex/信号语义。
已有 Linux SMP/受控迁移能力独立保留；本轮不承诺解释器线程或跨CPU负载均衡。

## guest 使用与安装

仅需 `capmix.linux=1`，**不需要** `capmix.linux_interp=1`。
用已有 Linux 产品配置或安装时 `INSTALL_LINUX=1` 启用。

```sh
make iso LIMINE_CONFIG=boot/limine-linux.conf
# 使用新的一次性磁盘；安装器继续拒绝非空未知盘
make install-disk DISK_IMAGE=/tmp/new-capmix-mp.img INSTALL_LINUX=1
```

启用后产品 init 通过既有 `install_missing` 仅补缺文件：

- `/bin/micropython.elf`
- `/data/linux/micropython-demo.py`

shell：

```text
run /bin/micropython.elf /data/linux/micropython-demo.py
```

输出 `micropython golden`、`42`，然后 shell 报 `exit=0 reap=ok`。
可继续原生重定向：

```text
run /bin/micropython.elf /data/linux/micropython-demo.py > /mp-result.txt
run /bin/cat.elf /mp-result.txt
```

关闭 persona 不删除原生程序，也不自动删除已安装的 Linux ELF 或 `/data/linux`；
Linux ELF 将按既有默认拒绝策略不能启动。安装布局、AHCI/virtio-blk、shell 核心未改。

## 自测、ENOSYS 与口径

`boot/linux_micropython_test.c` 捕获 stdout 到 FILE capability：
成功用例逐字节比较，负例断言退出码和异常诊断；逐次检查页+cache、commit、fd回收。
超时明确 panic，拒绝释放仍可能运行的用户现场。

用例包括 inline 打印/42、VFS脚本文件、sys.exit(7)、4000轮列表分配/自动GC/MPZ大整数、
语法错误、堆超限、递归超限、缺文件、浮点/外部模块禁用、超长文件、含NUL文件。
真正执行来自 guest CPL3；hosted 只验证 ELF/persona/BSS/记账回收，执行明确 skipped。
宿主额外用固定 Linux ELF 运行同类断言，不能代替 guest 证据。

```sh
make test
make kernel
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
# 全量含 hush、ldso/TLS 及新增解释器；无网卡可跳过 wire 测试
 timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -nic none -cdrom capmix.iso -serial stdio -display none -no-reboot \
  > /tmp/capmix-mp.log 2>&1
python3 tests/check_linux_micropython.py /tmp/capmix-mp.log
python3 scripts/linux-enosys.py --parse-only --scope micropython \
  --log /tmp/capmix-mp.log
```

专属直方图使用前后差值，scope=micropython；本轮固定用例要求 **ENOSYS=0**。
无需为展示用例增加 syscall；已有 arch_prctl、set_tid_address、write、exit_group，
以及文件模式的 open/read/close 即可。全局 ENOSYS 仍可能包含其他负探针，不能把
全局计数混作解释器需求。没有为了兼容而伪造线程/信号成功。

关键日志：`micropython fixture`、`CPL=3 output verified exit=0/7`、
`micropython reclaim ... pages/commit/fds=ok`、
`linux ENOSYS histogram end: scope=micropython total=0`、`micropython selftest: PASS`。

## 环境矩阵

2026-10-17 实际运行；QEMU8.2.2/KVM、512MiB、4vCPU；VBox7.2.20、UEFI、AHCI光驱、512MiB、4vCPU、无网卡。
下列全量日志均有 `selftest result: PASS (0 failures)`；产品日志另用宿主断言，不冒充全量自测。

| 配置 | 结果与仓库证据 |
| --- | --- |
| hosted Linux/interp 开 | 全量 PASS；[日志](../tests/evidence/logs/mp-hosted-on.log)、[断言](../tests/evidence/logs/mp-hosted-assertions.log)；解释器执行明确 skip |
| hosted 默认 selftest、interp 关 | 全量 PASS；[日志](../tests/evidence/logs/mp-hosted-final.log) |
| hosted Linux 运行关/编译关 | 全量 PASS；[运行关](../tests/evidence/logs/mp-hosted-off.log)、[编译关](../tests/evidence/logs/mp-hosted-compiled-off.log) |
| QEMU 单节点 e1000 usernet，Linux/interp 开 | 全量 PASS；[日志](../tests/evidence/logs/mp-on-final.log)、[递归断言](../tests/evidence/logs/mp-on-assertions.log)，含实际UDP/TCP路径 |
| QEMU 双NUMA节点、无网卡，Linux/interp 开 | 全量 PASS；[日志](../tests/evidence/logs/mp-dual-final.log)、[断言](../tests/evidence/logs/mp-dual-assertions.log) |
| QEMU Linux 开/interp 关 | 全量 PASS；[日志](../tests/evidence/logs/mp-static-final.log)、[断言](../tests/evidence/logs/mp-static-assertions.log)；静态解释器不依赖动态加载器 |
| QEMU Linux 运行关/编译关 | 全量 PASS；[运行关](../tests/evidence/logs/mp-off-final.log)、[编译关](../tests/evidence/logs/mp-compiled-off-final.log) |
| VBox UEFI SMP4，Linux/interp 开 | 全量 PASS；[日志](../tests/evidence/logs/mp-vbox.log)、[断言](../tests/evidence/logs/mp-vbox-assertions.log) |
| 无ISO产品安装盘、Linux开/interp关 | 两次启动 PASS；[首次](../tests/evidence/logs/mp-product-on-first.log)、[冷启动](../tests/evidence/logs/mp-product-on-second.log)、[宿主断言](../tests/evidence/logs/mp-product-on-assertions.log)。解释器脚本/退出/重定向冷读，L0/hush及原生hello/cat/nc/http-get/httpd/kill/poweroff均断言 |
| 无ISO产品安装盘、默认Linux关 | 两次启动 PASS；[首次](../tests/evidence/logs/mp-product-off-first.log)、[冷启动](../tests/evidence/logs/mp-product-off-second.log)、[宿主断言](../tests/evidence/logs/mp-product-off-assertions.log)。原生重定向/文件/网络/关机回归 |

全量递归断言包含L0–L5、hush、ldso/TLS/W^X、SMP overlap、线程、受控迁移以及既有内核自测。
本轮未另测 virtio-net wire；VBox产品盘没有另跑，只报告上述VBox全量自测。

三个干净目录重建一致（含仓库内部目录，最终配方固定版本信息）：
[比较与哈希](../tests/evidence/logs/mp-repro-assertions.log)、
[fixture/指令/宿主正负例检查](../tests/evidence/logs/mp-verify.log)、
[构建与产物标识摘要](../tests/evidence/logs/mp-build-summary.log)。
第三方全量构建原始日志的SHA和字节数保留在摘要，未把约4.7MiB编译逐行输出全部收入仓库。
上游musl getcwd编译警告及Python版本生成器弃用提示未屏蔽，摘要保留；
固定用例不调用getcwd。它们属于单独第三方重建，不是kernel/iso新增警告。
[内核编译](../tests/evidence/logs/mp-kernel-final.log)无 warning/error；hosted仍有已有架构未使用函数/变量警告。

解释器专属ENOSYS：
[单节点0](../tests/evidence/logs/mp-enosys.log)、
[双节点0](../tests/evidence/logs/mp-dual-enosys.log)。没有新增syscall或把大项伪装为成功。

