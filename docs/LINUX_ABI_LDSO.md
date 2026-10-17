# Linux ABI：最小真实用户态 ld.so

> 证据归档：见 [tests/evidence 索引与脱敏规则](../tests/evidence/README.md)。矩阵链接为原轮次留存日志的脱敏副本；本次仅归档，未重跑测试。命令中的 `/tmp` 是新运行的输出位置。

> 后续已增加有界 DSO initial-exec TLS，当前支持范围见 [LINUX_ABI_TLS](LINUX_ABI_TLS.md)。
> 本文环境矩阵保留首次真实 ld.so 交付快照；新增 TLS 及默认动态 W^X/RELRO 的当前范围见
> [TLS](LINUX_ABI_TLS.md) 与 [W^X/RELRO](LINUX_ABI_WX_RELRO.md)。

本轮实现的是**仓库自有、无 libc 依赖、有界的 x86-64 用户态动态链接器**。
它读取真实 PT_DYNAMIC/DT_NEEDED、从 VFS 加载 DSO、解析动态符号并执行 RELA，
随后跳到主程序入口并进入 C main。不是 musl/glibc ld.so，也不承诺任意Linux动态程序。
L4a解释器探针仍保留；`.so`后缀本身不是兼容性证明。

原生CapMix ABI与原生程序保持轨A；仅明确标记的Linux ELF进入轨B。
产品默认Linux关闭，解释器默认关闭。未修改KPTI、buddy锁、时钟、TCP/RTO、
L5匿名mmap/COW reserve commit核心语义、磁盘安装布局。

## 与 L4a 的区别

| 项目 | L4a | 本轮 |
| --- | --- | --- |
| PT_INTERP | `/lib/ld-capmix-l4a.so` | `/lib/ld-capmix.so.1` |
| 动态表 | PT_DYNAMIC拒绝 | 有界PT_DYNAMIC，用户态解析DT_* |
| 共享库 | 无 | 通过Linux open/read/close从VFS `/lib/`加载DT_NEEDED |
| 符号/重定位 | 无 | 实际符号查找、立即绑定、4类RELA |
| 验证 | PIC探针检查auxv再跳主程序 | 跨DSO函数调用、GOT/PLT、相对指针、绝对指针及全局数据校验 |
| TLS/生态 | 无 | 有界 DSO initial-exec TLS 见续作；不是完整glibc/musl动态生态 |

内核只校验和映射ET_EXEC主镜像、固定bias的ET_DYN解释器，构建既有auxv。
**内核不替代用户ld.so完成符号解析，也没有新增私有“动态链接syscall”。**
新路径仅在指定解释器及双开关满足时允许PT_DYNAMIC；原生/静态/L4a原有拒绝边界保留。

## 黄金用例与复现

| 安装路径 | 固定产物与源 |
| --- | --- |
| `/bin/linux-dynamic.elf` | `tests/fixtures/ldso/linux-dynamic.elf`；`user/linux/ldso/main.c`、`main_start.S` |
| `/lib/ld-capmix.so.1` | 同目录同名；`ldso.c`、`runtime.h`、`start.S` |
| `/lib/libcmgold.so.1` | 同目录同名；`gold.c` |

全部为本仓库原创源码，不打包第三方ld.so；构建依赖GCC/binutils与工具链`<elf.h>`的
ELF64 ABI声明，不链接宿主libc。固定工具链为Ubuntu GCC13.3.0、GNU binutils2.42。
脚本使用 `--hash-style=sysv`、`-z now`、4KiB最大页、无build-id、无stack-protector/red-zone、
general-regs-only，最后去符号并明确标记ELFOSABI_LINUX。SOURCE_DATE_EPOCH=0/TZ=UTC。
主程序为非PIE ET_EXEC，主/DSO用PIC代码避免COPY relocation；解释器为无自身重定位的PIC。

```bash
scripts/build-linux-ldso.sh /tmp/capmix-ldso-fresh
for n in linux-dynamic.elf ld-capmix.so.1 libcmgold.so.1; do
  cmp /tmp/capmix-ldso-fresh/$n tests/fixtures/ldso/$n
done
sha256sum -c tests/fixtures/ldso/SHA256SUMS
python3 scripts/check-ldso-fixtures.py tests/fixtures/ldso
```

两个干净目录已逐字节复现；其他工具链版本可能有不同结果。脚本不自动替换fixture。
构建审计读取实际program header/dynamic/relocation，不只检查源码里是否有某个符号。
正常kernel构建校验三份哈希并执行ABI审计，不联网：

```text
99a67d763d6349669b02ce3550a3c5dc37c1d8effe1f098c4edc8934c0bc1b50  linux-dynamic.elf
e3ec302a77cd9f321c603803698c6c253a031523b43069b171f2fd674a0b2c9d  ld-capmix.so.1
a725679a780f6f74a131008b178940a28dd8996a1a1f881c65a9d892a3a622a4  libcmgold.so.1
```

黄金main实际调用共享库的 `gold_answer()`，检查42+7=49、导入全局值42、相对指针指向7、
绝对指针等于导入全局的真实地址；全部成立才打印并exit=0：

```text
ldso: loaded /lib/libcmgold.so.1
ldso: reloc RELATIVE=1 GLOB_DAT=4 JUMP_SLOT=1 R64=1
linux ldso: relocation verified
```

这些计数对应真实重定位：主程序GLOB_DAT×2、JUMP_SLOT×1、64×1；DSO RELATIVE×1、
GLOB_DAT×2。解释器本身无DT_NEEDED及RELA，审计失败即停止构建，不声称支持任意ld.so自举。

## 启动与 auxv

- 主程序须明确Linux标记的x86-64 ET_EXEC，PT_INTERP精确匹配新路径；不支持PIE主程序。
- 主PT_LOAD仍在 `[0x400000,0x600000)`，解释器固定bias `0x600000`、窗口至`0x700000`。
  没有ASLR/KASLR声明。解释器PT_DYNAMIC必须是文件中可读PT_LOAD的一部分；递归PT_INTERP和PT_TLS拒绝。
- PT_DYNAMIC只允许一个，filesz=memsz，非零、16字节对齐且≤4096字节，文件偏移与PT_LOAD一致。
- auxv沿用Linux语义：AT_BASE=解释器bias，AT_ENTRY=主入口，AT_PHDR/PHENT/PHNUM=主程序表；
  另有AT_PAGESZ=4096、AT_RANDOM=16字节及AT_NULL。不新增与这些含义冲突的私有字段。
- 原始argc/argv/envp/auxv栈交给解释器，链接后恢复原栈、RDX=0跳AT_ENTRY。
  黄金CRT调用C main后直接Linux exit；没有rtld finalizer、libc启动、构造器或退出析构。

## 实际支持的重定位与符号

采用ELF64 RELA，24字节条目、显式addend；全部立即绑定，不做懒绑定resolver。

| 类型 | 公式/范围 |
| --- | --- |
| R_X86_64_RELATIVE=8 | B+A；symbol index必须0 |
| R_X86_64_64=1 | S+A |
| R_X86_64_GLOB_DAT=6 | S；本轮要求addend=0 |
| R_X86_64_JUMP_SLOT=7 | S；本轮要求addend=0 |

B=当前对象bias，S=解析后的符号地址；64位结果按ELF64字段写入。每个target必须8字节对齐，
完整落在该对象声明的可写PT_LOAD中，禁止向代码/只读段写入。
不支持NONE、COPY、PC32、IRELATIVE/IFUNC、REL/RELR及其他架构类型。
TLS 续作额外支持有界 `R_X86_64_TPOFF64`，其余 TLS 重定位仍拒绝。
未知类型打印数值并拒绝，不通过跳过修复来假装链接成功。

动态表支持DT_HASH（SysV）、STRTAB/STRSZ、SYMTAB/SYMENT、RELA/RELASZ/RELAENT、
JMPREL/PLTRELSZ/PLTREL、NEEDED，以及本目标的SONAME、PLTGOT、DEBUG、BIND_NOW、
FLAGS/FLAGS_1(NOW)、RELACOUNT；解释器自描述可有SYMBOLIC。未知动态feature拒绝。
DEBUG不建立调试器link_map；PLTGOT不实现lazy resolver；RELACOUNT不代替逐条检查。

符号在主程序→按广度优先加载的DSO中有界线性查找，第一份定义胜出；仅default visibility的
GLOBAL/WEAK、FUNC/OBJECT/NOTYPE。未定义weak可为0；未定义strong打印名字并拒绝。
定义必须落在对象加载范围，函数在可执行PT_LOAD；不支持ABS/COMMON特殊section、
符号版本、GNU hash、GNU unique、复杂弱符号/可见性规则或IFUNC调用。

## 文件路径、内存与 TLS 边界

- DT_NEEDED只接受1–63字节basename，字符限字母数字、点、下划线、连字符，不接受`.`、`..`、
  slash或任意绝对路径；统一拼接 `/lib/<name>`。没有RPATH/RUNPATH、缓存、LD_LIBRARY_PATH、
  LD_PRELOAD或系统ld-linux路径别名支持。
- 最多主程序+3个DSO；按请求basename去重、广度优先加载，循环依赖不重复加载。
  黄金只验一份DSO；没有宣称任意依赖森林及构造器排序。
- 每个DSO文件≤512KiB、PT_LOAD相对跨度≤1MiB、≤16个PHDR、每表≤2048个符号/RELA、
  string table≤64KiB。要求相对起始页0，PT_LOAD按页不重叠，p_align≤4096且合法，禁止声明W+X段。
- open/lseek/read取得文件快照缓冲，再用**既有匿名 PRIVATE RW mmap**取得驻留空间，复制PT_LOAD，
  未初始化数据由eager清零保障；临时文件buffer用完munmap，文件立即close。
  这不是文件mmap、MAP_SHARED、按需pager或共享物理库缓存；每进程各有私有副本。
- mmap语义/commit收费不变：Linux16MiB VA窗口、8MiB总映射额度；库span中的空洞、临时buffer也消耗
  真实映射额度与commit。COW页/reserve仍驻留，不接压缩/swap。物理、commit、对象额度不足明确失败。
- **当前真实动态路径默认 W^X/最小 GNU_RELRO**：库以 RW+NX 装载，重定位后按段切换，
  保护主程序/库/解释器约定 RELRO 页。仅动态镜像开放有界 mprotect；无 RELRO 文件可兼容。
  需要新版解释器协议 note，旧加载器在内核中拒绝，详见 [W^X/RELRO](LINUX_ABI_WX_RELRO.md)。
- **TLS续作**：旧黄金三份文件仍无PT_TLS；主/解释器继续拒绝PT_TLS。
  新增两个 DSO initial-exec TLS 黄金库，loader 分配最小 TCB 并支持 TPOFF64；
  无 DTV、TLSDESC、dlopen/pthread 或完整 glibc TCB，精确范围见 [LINUX_ABI_TLS](LINUX_ABI_TLS.md)。
- 已支持AP固定执行位置；本篇动态黄金仍按单线程验收（见 [Linux SMP](LINUX_SMP.md) 和 [线程子集](LINUX_THREADS.md)）；无dlopen/dlsym/dlclose、审计、完整signals/futex、C++运行时、完整libc启动或32位。

## 错误、回收与自测

内核拒绝坏PT_DYNAMIC、TLS、缺解释器，并沿用加载失败的页表/页/commit回滚。
用户ld.so遇缺库、表越界、未知feature/relocation、未解析strong符号、非法target等，输出
`ldso: reject ...`，关闭已打开fd，munmap临时buffer/已分配DSO，exit_group(126)。
loader不把unsupported格式转换成虚假的syscall成功；未新增syscall，目标路径ENOSYS=0。
若用户main自己的内容检查失败，返回71/72；正常返回0。

`boot/linux_ldso_test.c`检查：

1. 正确黄金执行及恢复后的再执行，逐字节比较输出，exit=0。
2. 缺库、IRELATIVE=37、向R-only段写relocation、缺符号、伪造空PT_TLS库均exit=126，比较准确诊断。
3. 内核坏PT_DYNAMIC、主PT_TLS、缺解释器拒绝，回收对称。
4. 每次执行后页数+cache、commit、打开FILE计数回到基线。负例不会遗留库映射。
5. gate-off拒绝新动态主程序；旧L0–L5/hush及原生链由host checker递归断言。

关键日志：`linux ldso interp mapped`、`ldso: reloc ...`、`linux ldso: relocation verified`、
`CPL3 exit=0 ... ENOSYS=0 rollback/reclaim=ok`、负例exit=126，以及
`linux ldso selftest: PASS (0 failures)`。最终还必须有全量 `selftest result: PASS (0 failures)`。
hosted只验证内核结构/回滚/gate，真实跨库执行由freestanding和产品路径验证。

## 启用、安装与回归

默认 `make iso` 仍为原生产品路径且Linux关闭。新路径需要编译 `LINUX_ABI=1`，并同时：

```text
capmix.linux=1 capmix.linux_interp=1
```

只开Linux不打开解释器时，静态L0–L5/hush可运行，新动态主程序拒绝。
`LINUX_ABI=0`时即便配置含开关也不会启用persona。开关不删除原生/bin，不做内核热卸。
仅双开时bootstrap补缺失的三份新产物；不覆盖用户已有同名文件。

```bash
make iso LIMINE_CONFIG=boot/limine-linux-ldso.conf
# 可选：新空白盘安装，用磁盘专用配置含installed=1；不要追加--linux，它会把interp重置0
python3 scripts/install-disk.py /tmp/new-ldso.img --config boot/limine-linux-ldso-disk.conf
# 从该盘启动，不挂ISO；产品shell输入：
```

```text
run /bin/linux-dynamic.elf
run /bin/busybox-sh.elf sh /data/linux/sh-smoke.sh
```

离线Linux包enable/install仍默认 `linux_interp=0`，没有擅自扩大稳定包的范围。
只关闭interp时已经安装的ELF/库留盘，新动态执行被拒；Linux卸载的数据策略见
[LINUX_PACKAGE](LINUX_PACKAGE.md)。

```bash
make test
CAPMIX_CMDLINE='capmix.mode=selftest capmix.linux=1 capmix.linux_interp=1' ./capmix-hosted
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
# 按仓库QEMU smp4串口路径运行；网络回归开宿主TCP9998/UDP9999/HTTP8000
python3 tests/check_linux_ldso.py serial.log --wire
# 普通selftest关interp：--interp-off；hosted：--hosted；Linux关：--disabled
make iso LIMINE_CONFIG=boot/limine-linux-off-selftest.conf
make test LINUX_ABI=0
make iso LINUX_ABI=0 LIMINE_CONFIG=boot/limine-linux-off-selftest.conf
```

## 环境矩阵与修改文件

2026-10-16，QEMU8.2.2/KVM、CPU host、512MiB、SMP4；VBox7.2.20。
下列是实际运行范围；hosted不执行CPL3，VBox本轮不声称全量debug/网络通过。

| 环境/配置 | 实测结果 | 归档日志 |
| --- | --- | --- |
| 两个干净构建目录 | 三份产物逐字节一致，SHA256SUMS及重定位审计通过 | [ldso-build1.log](../tests/evidence/logs/ldso-build1.log)、[ldso-build2.log](../tests/evidence/logs/ldso-build2.log) |
| hosted Linux开/interp关、双开、编译Linux关 | 全量PASS (0 failures)，分别验证gate与内核格式/回滚 | [ldso-hosted.log](../tests/evidence/logs/ldso-hosted.log)、[ldso-hosted-on.log](../tests/evidence/logs/ldso-hosted-on.log)、[ldso-hosted-off.log](../tests/evidence/logs/ldso-hosted-off.log) |
| QEMU双开、e1000单节点 | 全量PASS (0 failures)，黄金及所有负例、L0–L5/hush回归链通过 | [ldso-on-e1000-matrix.log](../tests/evidence/logs/ldso-on-e1000-matrix.log) |
| QEMU双开、双NUMA、virtio-net优先+e1000 | 全量PASS (0 failures) | [ldso-on-dual-vnet-matrix.log](../tests/evidence/logs/ldso-on-dual-vnet-matrix.log) |
| QEMU双开、无网卡 | 全量PASS (0 failures)，网络按既有skip策略 | [ldso-on-no-nic-matrix.log](../tests/evidence/logs/ldso-on-no-nic-matrix.log) |
| QEMU Linux开/interp关、e1000 | 全量PASS (0 failures)，新动态拒绝，静态及网络链通过 | [ldso-interp-off-matrix.log](../tests/evidence/logs/ldso-interp-off-matrix.log) |
| QEMU Linux运行时关、e1000 | 全量PASS (0 failures) | [ldso-linux-off-matrix.log](../tests/evidence/logs/ldso-linux-off-matrix.log) |
| QEMU编译LINUX_ABI=0、双节点两网卡 | 全量PASS (0 failures) | [ldso-compiled-off-matrix.log](../tests/evidence/logs/ldso-compiled-off-matrix.log) |
| 无ISO产品盘，virtio-blk+e1000 | 动态黄金exit=0、hush脚本、原生重定向/nc/http-get/httpd两次200、关机通过 | [ldso-product-installboot.log](../tests/evidence/logs/ldso-product-installboot.log)、[ldso-httpd-client.log](../tests/evidence/logs/ldso-httpd-client.log) |
| 同盘冷启动，磁盘专用配置 | 动态黄金再exit=0，原生cat读回持久数据、hush及关机通过 | [ldso-product-second.log](../tests/evidence/logs/ldso-product-second.log) |
| 离线稳定包enable将interp置0的盘副本 | 动态拒绝shell error=-7；L0、hush、原生hello/cat、数据保留和关机通过 | [ldso-product-interp-off.log](../tests/evidence/logs/ldso-product-interp-off.log) |
| 默认QEMU产品ISO | Linux enabled=0，原生用户shell/hello/ACPI关机通过 | [ldso-default-product.log](../tests/evidence/logs/ldso-default-product.log) |
| VBox EFI64/PIIX3/AHCI/SMP4，无ISO | 动态输出四类实际重定位计数、校验成功exit=0；hush、L0、原生cat、ACPI关机通过 | [ldso-vbox-first.log](../tests/evidence/logs/ldso-vbox-first.log) |

初版实验ISO配置没有installed=1，离线包工具正确拒绝修改；本轮没有放宽检查。
现提供独立 `limine-linux-ldso-disk.conf` 保留installed标志，并在离线盘副本上重测通过。
VBox第二次冷启动也再次运行动态黄金exit=0、读回持久数据并关机；日志 [ldso-vbox-second.log](../tests/evidence/logs/ldso-vbox-second.log)。
VBox网卡显示down，本轮未验证其网络。QEMU原生网络回归使用TCP9998/UDP9999/HTTP8000
测试端和httpd转发18081→8080；全量日志由 `tests/check_linux_ldso.py` 检查。
内核/ISO无新增warning/error；hosted保留原有arch硬件桩unused警告。

修改文件：

- `proc/elf.c`、`include/capmix/elf.h`：新解释器路径、rtld标记与PT_DYNAMIC结构校验，旧静态/L4a边界保留。
- `user/linux/ldso/{ldso.c,runtime.h,start.S,main.c,main_start.S,gold.c}`：真实用户态链接器及黄金源码。
- `scripts/build-linux-ldso.sh`、`scripts/check-ldso-fixtures.py`、`tests/fixtures/ldso/*`：复现、ELF审计、三份固定产物/哈希/说明。
- `boot/linux_ldso_test.c`、`tests/check_linux_ldso.py`、`boot/kernel.c`、`include/capmix/abi.h`：正负例、回收与回归链。
- `proc/init.c`、`Makefile`、`.gitignore`、`boot/limine-linux-ldso*.conf`：仅双开时供应文件，实验ISO/磁盘配置及构建。
- 本文、总览/导航、L4a和VBox手册：功能边界、用法与实际验收矩阵。

没有修改Linux syscall分发、mmap/brk/COW commit实现、KPTI策略、buddy锁、时钟、
TCP/RTO或安装布局核心。此版本没有“任意Linux动态程序可用”的承诺。

线程续作：已有 [有界clone/futex](LINUX_THREADS.md)。本文单线程/不支持线程的说明为此前范围；
新子集仍不等于通用pthread，线程组内fork/exec/映射与fd表变更暂拒绝，未创建线程的原路径保留。
