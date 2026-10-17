# Linux ABI：最小 DSO initial-exec TLS

> 证据归档：见 [tests/evidence 索引与脱敏规则](../tests/evidence/README.md)。矩阵链接为原轮次留存日志的脱敏副本；本次仅归档，未重跑测试。命令中的 `/tmp` 是新运行的输出位置。

> 后续真实动态路径已默认启用 [W^X/最小 RELRO](LINUX_ABI_WX_RELRO.md)。
> 以下环境矩阵保留 TLS 首轮验证记录。

本实现扩展仓库自有 `/lib/ld-capmix.so.1`，支持启动阶段加载的 DSO 使用
x86-64 Variant II initial-exec TLS。不是 musl/glibc ld.so，不承诺任意 Linux
动态程序。原生 ABI 不变，原生新软件仍建议 `.cmx` 或 `/bin/cm/`。

## 开关与运行

产品默认 `capmix.linux=0`，解释器默认关闭。TLS 没有额外的全局开关，只能在
`capmix.linux=1 capmix.linux_interp=1` 的 Linux persona 中通过真实解释器使用。
`make ... LINUX_ABI=0` 可编译关闭 Linux；关闭后保留原生程序与持久数据，
不涉及内核热卸。稳定 Linux 包启用流程仍默认关闭解释器，实验配置需显式选择。

```sh
make iso LIMINE_CONFIG=boot/limine-linux-ldso.conf
# 进入产品 shell 后：
run /bin/linux-tls.elf
# 两个独立进程也可运行：
run /bin/linux-tls.elf &
run /bin/linux-tls.elf
```

双开时产品入口仅补充缺失文件：`/bin/linux-tls.elf`、
`/lib/libcm_tls_a.so.1`、`/lib/libcm_tls_b.so.1` 和解释器。
不会覆盖磁盘已有解释器。旧实验盘若已有不支持 TLS 的 ld.so，须显式离线更新该文件，
或用新测试盘；不要以为换内核就会覆盖持久文件。安装布局不变。

## 精确支持范围

- 主程序仍为标记 Linux 的 ET_EXEC，无 `PT_TLS`；通过 DSO 导出函数访问 TLS。
- 最多主程序 + 3 个 DSO；其中最多 3 个 TLS 模块，全部在进入主程序前加载。
- 每模块最多一个非空 `PT_TLS`，`p_memsz <= 65536`，`p_filesz <= p_memsz`。
  对齐为 0/1 或不超过 4096 的 2 次幂；模板 VA 必须满足声明对齐。
  初始化数据必须与可读 PT_LOAD 的文件偏移对应。重复、越界、超限均拒绝。
- 动态 TLS 符号为默认可见 GLOBAL/WEAK `STT_TLS`，定义必须在模块 TLS 范围内。
  未解析 TLS 符号拒绝，不提供未定义 weak TLS 的特殊零地址语义。
- 新增 `R_X86_64_TPOFF64`（18），仅零 addend；GOT 写入定义模块的 TP 相对偏移。
  库允许 `DT_FLAGS` 的 `DF_STATIC_TLS`。既有 RELATIVE/GLOB_DAT/JUMP_SLOT/64 保留。
- 库用 `-ftls-model=initial-exec` 编译；解释器自身无 PT_TLS、外部依赖或自重定位。
- 不支持主程序 local-exec TLS、general/local-dynamic、DTPMOD64/DTPOFF64、
  TLSDESC、`__tls_get_addr`、DTV、`dlopen`、pthread 或完整 glibc TCB。
  不支持的重定位输出类型并 exit 126；这不等于内核为所有应用实现了 TLS。

ABI 常量与结构来自构建工具链的 `<elf.h>`（GNU binutils 2.42 配套定义），
按 x86-64 ELF TLS Variant II 使用 TP 负偏移；未发明寄存器或更改 syscall ABI。
仅有 PLT RELA 的对象可以没有 DT_RELAENT；若提供则仍校验条目大小。

## 内存、TCB 与 FS

模块按依赖遍历顺序排在 TP 前，逐个向负方向分配并对齐，不相互重叠。
TP 按 4096 对齐：`FS:0` 为 TP 自身，其余 TCB 保留区域为零。
这是一份面向该黄金 crt 的最小约定，FS:8 **不是可用 DTV**。

总分配为 `round_up(所有模块含对齐间隙,4096) + 4096`，上限 200704 字节
（49 页）；最后一页放最小 TCB。复制 `.tdata`，匿名映射的零初始化供应 `.tbss`。
分配经既有 Linux PRIVATE anonymous mmap，立即分配并计入 commit；仍受 L5
8MiB 映射容量、16MiB VA 窗口约束，没有扩大窗口或改变 no-overcommit。

重定位完成后调用 Linux `arch_prctl(ARCH_SET_FS)`，再恢复原始栈跳主入口。
FS 的硬件保存/恢复完全复用 L0 上下文机制，没有更改 KPTI、切换汇编或 buddy。
GET_FS 继续返回实际 FS base。两个进程可在同一 BSP 交错执行并拥有独立 TLS。
现有 fork 继承 FS 虚址、匿名页沿现有 COW 机制处理；本轮未专门验收 TLS+fork，
不新增该组合兼容承诺，也未增加线程或 COW 页换出支持。exec 仍重置 FS。

失败时关闭库文件、释放文件缓冲、TLS 映射及库映射，exit 126；进程退出后既有
镜像销毁回收匿名页。若失败发生在 FS 安装后，先清 FS。无新增全局 TLS 池。
动态路径已续作 W^X/最小 RELRO（见链接），原生/静态兼容例外保留；仍不是安全隔离完整的动态生态。

## 可复现黄金文件

源码 `user/linux/ldso/tls_main.c`、`tls_lib.c`，复用 `main_start.S`。
库源码编译两次得到独立符号/模块：初值 17/29，64 字节对齐，同时含 `.tbss`。
主程序验证 FS:0 与 GET_FS 一致、两个模块初值和零值，然后写入依赖 PID 的不同值，
在 TSC 有限循环中持续校验，成功输出 `linux tls: payload verified` 并 exit 0。
没有 libc、TLS bootstrap 运行库或隐式宿主库依赖。

固定工具链 Ubuntu GCC 13.3.0、GNU binutils 2.42，SOURCE_DATE_EPOCH=0、TZ=UTC。
原有三份黄金文件与新三份均在 `tests/fixtures/ldso/`，SHA-256 以同目录
`SHA256SUMS` 为唯一清单，构建时核验。换工具链可能改变二进制，不自动重钉哈希。

```sh
scripts/build-linux-ldso.sh /tmp/cm-tls-rebuild
for n in linux-dynamic.elf ld-capmix.so.1 libcmgold.so.1 linux-tls.elf libcm_tls_a.so.1 libcm_tls_b.so.1; do
  cmp /tmp/cm-tls-rebuild/$n tests/fixtures/ldso/$n
done
sha256sum -c tests/fixtures/ldso/SHA256SUMS
python3 scripts/check-tls-fixtures.py tests/fixtures/ldso
```

审计确保两个 IE 库各两个 TPOFF64、无 DTPMOD/TLSDESC/SIMD，主程序无 PT_TLS；
原有审计继续保证解释器无 TLS/自重定位，旧黄金重定位数量不变。

## 自测与日志

启用配置：`make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf`。
运行前宿主可启动 `scripts/tcp-echo.py`（9998）、`scripts/udp-echo.py`（9999）及
8000 端口 HTTP 服务，`/hello.txt` 内容为 `linux L3 host body\n`，以覆盖网络回归。

```sh
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -serial stdio -display none -no-reboot 2>&1 | tee /tmp/capmix-tls.log
python3 tests/check_linux_tls.py --wire /tmp/capmix-tls.log
```

- 正例：`tls-golden` 捕获并逐字比较 stdout，要求 exit=0、ENOSYS=0。
- 隔离：两个进程同时创建，检查执行区间重叠、各自发生抢占、均 exit=0，
  调度器 FS=0；每个进程反复检查自己 PID 派生的 TLS 值。
- 负例：模板 filesz>memsz、memsz>64KiB、对齐>4KiB、重定位改成
  DTPMOD64（16）；要求对应诊断、exit=126、页/commit/打开文件数回到基线。
- `ldso: tls init modules=2 TPOFF64=4`
- `linux tls: payload verified`
- `linux tls isolation verified: same-cpu overlap preempts=... scheduler_FS=0`
- `linux ldso tls-...: CPL3 exit=126 ... rollback/reclaim=ok`
- `linux tls selftest: PASS (0 failures)` 和全量 `selftest result: PASS (0 failures)`。

hosted 只检查既有加载/门控与 FS 模型，不执行硬件 TLS，明确打印 skipped；
不能把 hosted PASS 当作执行 TLS 指令的证据。

## 环境矩阵

2026-10-16，Ubuntu 宿主；以下仅记录真实完成的测试。

| 环境 | 结果 | 日志 |
| --- | --- | --- |
| 两个干净构建目录，GCC13.3/binutils2.42 | 六份产物逐字节一致，哈希/ELF 审计通过 | [tls-fixture-build.log](../tests/evidence/logs/tls-fixture-build.log)、[tls-reproduce.log](../tests/evidence/logs/tls-reproduce.log) |
| hosted：Linux关、Linux开/interp关、双开 | 三份全量 `PASS (0 failures)`；硬件 TLS 明确跳过 | [tls-hosted-disabled.log](../tests/evidence/logs/tls-hosted-disabled.log)、[tls-hosted-interp-off.log](../tests/evidence/logs/tls-hosted-interp-off.log)、[tls-hosted-on.log](../tests/evidence/logs/tls-hosted-on.log) |
| KVM SMP4，双开，e1000 | 全量 `PASS (0 failures)`，TLS 正负例、同 CPU overlap、实际 UDP/TCP 回归通过 | [tls-on-e1000-matrix.log](../tests/evidence/logs/tls-on-e1000-matrix.log) |
| KVM SMP4，双 NUMA，virtio-net+e1000 | 全量 `PASS (0 failures)`，TLS 隔离及网络链通过 | [tls-on-dual-vnet-matrix.log](../tests/evidence/logs/tls-on-dual-vnet-matrix.log) |
| KVM SMP4，双开，无网卡 | 全量 `PASS (0 failures)`，网络按既有策略 skip | [tls-on-no-nic-matrix.log](../tests/evidence/logs/tls-on-no-nic-matrix.log) |
| KVM SMP4，Linux开/interp关，e1000 | 全量 `PASS (0 failures)`，动态拒绝，静态/原生保留 | [tls-interp-off-matrix.log](../tests/evidence/logs/tls-interp-off-matrix.log) |
| KVM SMP4，Linux运行时关，e1000 | 全量 `PASS (0 failures)` | [tls-linux-off-matrix.log](../tests/evidence/logs/tls-linux-off-matrix.log) |
| KVM SMP4，编译 Linux关，双节点/两网卡 | 全量 `PASS (0 failures)` | [tls-compiled-off-matrix.log](../tests/evidence/logs/tls-compiled-off-matrix.log) |
| 无 ISO 产品盘，virtio-blk/e1000 | TLS 前台/后台、旧动态、hush、L0/L5、文件重定向、nc/http-get/httpd 两次响应、关机通过 | [tls-final-product-first.log](../tests/evidence/logs/tls-final-product-first.log)、[tls-final-httpd-client.log](../tests/evidence/logs/tls-final-httpd-client.log) |
| 默认产品 ISO，Linux 关闭 | 用户 shell、原生 hello exit=0、ACPI 关机通过 | [tls-default-product.log](../tests/evidence/logs/tls-default-product.log) |
| 同产品盘冷启动，无 ISO | TLS/旧动态 exit=0、持久文件 cat、hush、关机通过 | [tls-final-product-second.log](../tests/evidence/logs/tls-final-product-second.log) |
| VBox 7.2.20，EFI64/PIIX3/AHCI/SMP4，第一次无 ISO 启动 | TLS 前台/后台、旧动态、L0、hush、原生重定向/读回、关机通过 | [tls-vbox-first.log](../tests/evidence/logs/tls-vbox-first.log) |
| VBox 同盘第二次冷启动 | TLS/旧动态/L0/hush exit=0、读回持久文件、关机通过 | [tls-vbox-second.log](../tests/evidence/logs/tls-vbox-second.log) |

六组 QEMU 日志均通过 `tests/check_linux_tls.py`，递归检查 L0–L5、hush、旧 ld.so
及全量结果。启用网卡的 Linux 配置使用 `--wire` 强制要求实际网络回归。
本轮 VBox 产品中 e1000 未达到可用链路，未宣称 VBox 网络或全量 debug 通过。
VBox 验证使用单独的 `CapMix-minimal-TLS-20261016` VM 与 `/tmp/tls-product.vdi`，
没有修改已有虚拟机或用户数据盘。

## 修改文件

`user/linux/ldso/{ldso.c,runtime.h,tls_main.c,tls_lib.c}`；
`scripts/{build-linux-ldso.sh,check-tls-fixtures.py}`；
`tests/fixtures/ldso/` 的新黄金文件、解释器及哈希/说明；
`.gitignore`、`Makefile`、`proc/init.c`、`boot/linux_ldso_test.c`、`tests/check_linux_tls.py`；
本说明与文档导航/总览/ld.so 文档的 TLS 范围更新。
