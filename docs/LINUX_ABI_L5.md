# Linux ABI L5：匿名 PRIVATE mmap、16MiB 窗口与驻留 COW commit

L5 仅作用于 `CM_ABI_LINUX`。产品默认 `capmix.linux=0`，原生程序继续使用
CapMix syscall/capability、posix_spawn 和原生 fork=NOTSUP；后续原生软件命名
`.cmx` 或放 `/bin/cm/`，不强制改名已有 `.elf`。关闭/离线卸载 persona 不删除
原生 `/bin`，Linux 数据保留/显式删除政策见 [LINUX_PACKAGE](LINUX_PACKAGE.md)。

本轮新增匿名映射与一个**未修改 musl 1.2.5 libc/分配器**的固定静态探针。
这不等于通用 musl、BusyBox sh 或 glibc 兼容。L1 仍是原固定 fixture：
**定制 musl 静态 BusyBox 子集 + 有界 BSS 分配**，64KiB 分配器没有被替换。
L4a 仍只是固定解释器交接，不是真 ld.so 重定位/符号解析。

## 地址空间与硬上限

| 对象 | 范围/上限 |
| --- | --- |
| Linux 用户 VA 校验窗口 | `[0x400000,0x1400000)`，跨度 **16MiB**；并非全区默认已映射 |
| Linux 每镜像映射 payload | **2048页 / 8MiB**，PT_LOAD、BSS、栈、brk、mmap 合计；fork 每子也受此限 |
| 匿名 mmap arena | `[0x800000,0x1400000)`，12MiB VA；实际仍受8MiB合计额度约束 |
| 静态 PT_LOAD / brk | 继续在旧低地址布局：PT_LOAD 不越过 `0x7fc000`；brk 从主镜像末尾增长到栈前 |
| 用户栈 | 四页 `[0x7fc000,0x800000)`，布局/argc/argv/envp/auxv保持不变 |
| L4a 固定解释器 | `[0x600000,0x700000)`，解释器模式下主镜像仍限其下；本轮不扩解释器分区 |
| 原生 CapMix | 仍128页 / 512KiB payload，VA校验 `[0x400000,0x800000)` |
| Linux 全局引用池 | 8192个物理 backing/reserve 槽，总计最多32MiB数据页；与物理RAM、commit、task槽共同限流 |
| 内核 copyin/out | 保持每次最多1MiB；可直接触达大映射，超1MiB I/O须分批，窗口扩容没有扩大单次copy许可 |

描述表首128项内嵌；之后每128项按需占一个buddy页，最多15个额外块。
页表/描述表/镜像元数据还消耗实际物理页，不算入8MiB payload额度；它们不是可延迟
承诺的资源，申请失败立即回滚。Linux 的总窗口扩大不意味着允许任意 ET_EXEC
链接地址、任意大小文件段、MAP_FIXED、无限栈或全部48位用户空间。

## syscall 语义

遵循 x86-64 Linux 参数寄存器顺序 `rdi,rsi,rdx,r10,r8,r9`，沿现有入口保存的
六参数现场取 fd/offset；未改变原生分发与入口汇编。

| 号 | 调用 | 首版语义 |
| --- | --- | --- |
| 9 | mmap(addr,len,prot,flags,fd,offset) | 只支持 `prot=PROT_READ\|PROT_WRITE=3`、`flags=MAP_ANONYMOUS\|MAP_PRIVATE=0x22`；fd忽略、offset须0。len向上取整页，逐页预分配并清零后返回地址 |
| 11 | munmap(addr,len) | addr页对齐、len非0并向上取整，范围须在arena内。支持部分释放和空洞，重复释放返回0；不会释放代码/栈/brk |
| 12 | brk(addr) | 既有有界增长/收缩语义不变，Linux共享2048页额度；失败返回旧break |
| 57 | fork() | 既有Linux-only fork，新增显式逐物理backing/reserve的commit所有权 |

非FIXED hint落在arena内且有连续空间时采用；否则从arena首地址first-fit，绝不覆盖
已有页。file mmap、MAP_SHARED、MAP_FIXED、其他flags或prot组合返回
`-EOPNOTSUPP=-95`（检查支持组合在先）。支持组合下零长度、长度大于arena或
非零offset返回 `-EINVAL=-22`；额度/连续VA/commit/物理页/引用槽不足返回
`-ENOMEM=-12`。非法munmap返回EINVAL。未实现的其他syscall仍 `-ENOSYS` 并计数。
不支持mprotect、PROT_NONE guard page、file mmap、MAP_SHARED、mremap、munmap
释放PT_LOAD/brk；没有新增按需缺页分配。NX状态不因本轮改变。

修改在同一进程既有固定CPU/单线程纪律下完成；没有为多线程同地址空间设计锁。
Linux fork/exec继续固定BSP，原生SMP多ELF策略不变。

## no-overcommit 与 COW 所有权

L2已经预分配副本并对fork收费；L5将收费归属明确到**物理数据页**，不是在原先
费用之上再重复加一笔。每个backing记录和每个reserve记录各拥有1页全局commit。

- 新映射N页：先全局charge N，再逐页取得物理页和引用槽；每取得一页就从临时
  `pending` 转交到该引用槽。失败释放已建映射、空页表/描述块、已取得物理页和剩余pending。
- fork有W个逻辑可写页：先charge W，再为每页预分配一个副本物理页及引用记录槽。
  只读代码共享现有backing，不重复charge。所有准备完成才将父PTE置只读。
- 多引用可写backing始终有 `refs-1` 个reserve；嵌套fork继续增加一个reserve/页。
- 写分裂：取出一个reserve、复制内容、将它变成新的backing。**commit总数不变**，
  fault中不再申请物理页或引用记录。单引用页只恢复可写，不消耗reserve。
- 任意次序unmap/退出：共享可写组减少一份引用就释放一份reserve并uncharge；最后
  引用释放backing并uncharge。父先退出、子先退出、嵌套子先写均对称。
- fork/map在commit不足或中途物理分配失败时完整回滚，已有内容/父PTE不被错误替换。

全局commit接口仍供其他子系统使用；即使swap扩展了commit_limit，Linux ELF backing
和reserve仍必须实际驻留。**这些页不接入anon压缩器或swap**，不会用磁盘信用冒充可用
COW副本。物理不足仍失败。页表与元数据通过实际分配约束；不是可换出的payload。

## 固定测试与源码来源

1. `tests/linux_l5.S`，链接到 `build/linux-mmap.elf`：手写Linux号，1MiB逐页清零/写读、
   fork真实写fault、wait4、父内容校验、munmap；输出
   `linux L5 user mmap: 1MiB touch + COW fork + unmap ok`，exit=0。
2. `user/linux/mmap_probe.c` → `third_party/linux-l5/musl-mmap.elf`：原版musl1.2.5
   CRT及mallocng，malloc2MiB+mmap1MiB逐页校验、free/munmap/write/exit_group。
   无libc源码补丁、无BSS分配器覆盖、无用户TLS变量。
3. `boot/linux_l5_test.c`：扩容后总页额度触达、VA最高一页、部分unmap/空洞重用、
   file/shared/越界拒绝、嵌套COW、reserve/backing/commit增减、commit拒绝，以及
   人为持有buddy页导致中途映射/fork失败；清理后页数、引用池及commit回到基线。

musl上游归档：`third_party/linux-l1/musl-1.2.5.tar.gz`，由该目录SHA256SUMS钉住，
来源/许可证见该目录README及musl归档COPYRIGHT（MIT）。新增二进制哈希：
`third_party/linux-l5/SHA256SUMS`。构建用Ubuntu GCC13.3.0/binutils，禁red-zone、
stack protector、tree vectorize，**仍是固定探针，不承诺浮点/SIMD上下文的任意libc路径**。

```bash
scripts/build-musl-l5.sh /tmp/capmix-musl-l5-fresh
sha256sum -c third_party/linux-l5/SHA256SUMS
# 与固定fixture比较；脚本不会自动替换仓库二进制
cmp /tmp/capmix-musl-l5-fresh/musl-mmap.elf third_party/linux-l5/musl-mmap.elf
```

此探针的mallocng元数据通过既有brk获得；mallocng会尝试一次MAP_FIXED/PROT_NONE
保护页，其返回EOPNOTSUPP且该上游路径不依赖其成功。故**没有真正guard page保护**。
依赖mprotect/PROT_NONE、小块分配其他路径、完整TLS/futex/信号、任意printf浮点、
动态链接的musl程序仍可能失败；本轮不宣称“任意未修改musl可运行”。
L1定制BSS分配器继续留作兼容基线；新程序可逐步使用brk/mmap，未全局切换旧fixture。

## 运行与验收

```bash
make test
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
# 宿主另开 scripts/tcp-echo.py、scripts/udp-echo.py，HTTP8000服务供原生回归
# 按仓库QEMU命令运行smp4（无网卡也允许skip网络），保存serial.log
python3 tests/check_linux_l5.py serial.log                 # L4a gate开启
python3 tests/check_linux_l5.py --interp-off serial.log    # 若使用普通selftest配置
make iso LIMINE_CONFIG=boot/limine-linux-off-selftest.conf # 运行时关闭矩阵
make iso LINUX_ABI=0 LIMINE_CONFIG=boot/limine-linux-off-selftest.conf
# 关闭日志检查加 --disabled；hosted检查加 --hosted
```

产品启用：`make iso LIMINE_CONFIG=boot/limine-linux.conf` 或离线Linux包工具；默认
`make iso`仍关闭。启用后bootstrap仅补缺失文件：
`run /bin/linux-mmap.elf`、`run /bin/musl-mmap.elf`；关闭不删它们也不影响原生工具。
安装盘由更新后的内核供应这两个ELF，不要求挂ISO；重开/卸载政策不变。

关键日志：`linux L5 mmap: window=16MiB quota=8MiB`、`linux L5 COW commit:`、
`partial allocation failure: ... rollback=ok`、`baseline restored`、
`CPL3 ... verified: exit=0 ENOSYS=0`、`linux L5 selftest: PASS (0 failures)`。
最终总验收还必须有 `selftest result: PASS (0 failures)`；局部PASS不能代替全量。

## 环境矩阵

2026-10-16，x86-64宿主，QEMU8.2.2/KVM、CPU host、512MiB、SMP4。
日志保留在开发机 `/tmp/`；矩阵对应实际运行，非跨环境保证。

| 环境/配置 | 结果 | 证据 |
| --- | --- | --- |
| hosted Linux开启/编译关闭 | 全量PASS (0 failures) | `/tmp/l5-hosted-final.log`、`/tmp/l5-hosted-off.log` |
| QEMU Linux开启、单节点e1000 | 全量PASS (0 failures)，L0–L5及原生网络链校验通过 | `/tmp/l5-on-e1000-matrix.log` |
| QEMU Linux开启、双节点、virtio-net优先+e1000 | 全量PASS (0 failures) | `/tmp/l5-on-dual-vnet-matrix.log` |
| QEMU Linux开启、无网卡 | 全量PASS (0 failures)，网络按既有skip策略 | `/tmp/l5-on-no-nic-matrix.log` |
| QEMU 运行时Linux关闭、e1000 | 全量PASS (0 failures) | `/tmp/l5-linux-off-matrix.log` |
| QEMU 编译LINUX_ABI=0、双节点、两网卡 | 全量PASS (0 failures) | `/tmp/l5-compiled-off-matrix.log` |
| QEMU 默认产品ISO，Linux默认关 | 用户shell、原生hello、ACPI关机通过；不跑全量selftest | `/tmp/l5-default-product.log` |
| QEMU 无ISO产品盘两次启动、virtio-blk+e1000 | 两个L5 ELF、L0、原生cat/重定向持久化、nc/http-get/httpd、poweroff通过 | `/tmp/l5-product-installboot.log`、`/tmp/l5-product-first.log`、`/tmp/l5-httpd-client.log` |
| VBox7.2.20 EFI64/PIIX3/AHCI/SMP4/512MiB，无ISO | 产品路径两个L5 ELF与L0 exit=0，原生文件/重定向写读、第二次冷启动持久读回、ACPI关机通过 | `/tmp/l5-vbox-first.log`、`/tmp/l5-vbox-second.log` |

VBox本轮仅产品验收，不宣称全量debug PASS；其e1000显示down，未验证VBox网络。
QEMU开启配置同时启用L4a实验gate，L0–L4日志断言一并通过；普通hosted不打开L4a，
验证其拒绝/保留静态的gate-off路径。构建内核无新增warning/error；hosted仍有原有
arch硬件桩unused警告。本轮没有修改KPTI、buddy锁、时钟、TCP/RTO、IRQ或安装布局核心。

## 修改文件

- `include/capmix/elf.h`、`proc/elf.c`、`proc/elf_args.c`：Linux分块描述表、窗口/额度、
  anonymous map/unmap、COW reserve引用槽和物理页commit所有权、失败页表回收。
- `abi/linux.c`：Linux9/11号到私有映射接口，原生分发不变。
- `include/capmix/abi.h`、`boot/kernel.c`、`boot/linux_l5_test.c`：测试入口与机制/ring3验收；
  `boot/linux_l2_test.c`、`boot/linux_l4_test.c`：旧128页负例适配新Linux额度/固定解释器分区。
- `tests/linux_l5.S`、`tests/check_linux_l5.py`、`user/linux/mmap_probe.c`、
  `scripts/build-musl-l5.sh`、`third_party/linux-l5/*`：固定黄金ELF、原版musl探针/来源许可/哈希与日志断言。
- `proc/init.c`、`Makefile`、`.gitignore`：嵌入/构建/校验fixture，仅Linux启用时补文件。
- 本文、文档导航/总览、L0/L1/L2/L4阶段快照提示及VBox手册：当前边界和实测矩阵。
