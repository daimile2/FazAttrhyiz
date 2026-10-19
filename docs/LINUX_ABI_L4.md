# Linux ABI L4a：PT_INTERP 的最小用户态交接

> 证据归档：见 [tests/evidence 索引与脱敏规则](../tests/evidence/README.md)。矩阵链接为原轮次留存日志的脱敏副本；本次仅归档，未重跑测试。命令中的 `/tmp` 是新运行的输出位置。

后续：[最小真实ld.so](LINUX_ABI_LDSO.md)使用独立路径实现真实共享库/符号/RELA；本篇L4a探针仍保留，不能将两者混为完整glibc动态生态。

后续更新：[L5](LINUX_ABI_L5.md)已新增Linux匿名PRIVATE mmap/munmap、16MiB VA窗口及8MiB映射额度，并细化COW物理页commit归属；下文首版容量/全部mmap拒绝为阶段快照，原生限制不变。

本轮是动态链接启动协议的第一步：**内核映射主程序和解释器，进入解释器，再由解释器跳转主程序**。
这是仓库固定的无 libc 探针；没有实现符号解析、重定位、共享库加载或真正可用的 musl/glibc ld.so。
`.so` 后缀是解释器试验产物名称，不代表共享库生态已支持。

Linux persona 产品默认仍关闭；原生 CapMix syscall、程序、安装布局与数据策略不变。
新增原生软件继续建议 `.faz` 或 `/bin/cm/`，本轮不改旧 `.elf` 文件名。
L1 依旧是**定制 musl 静态 BusyBox 子集 + 有界 BSS 分配**；L2/L3 能力保留。

## 接口意义与允许的镜像

| 项目 | 本轮定义 |
| --- | --- |
| 主程序 | 明确 Linux 标记的 x86-64 ET_EXEC；可有一个 PT_INTERP，不能有 PT_DYNAMIC/PT_TLS |
| Linux 识别 | 沿用 ELFOSABI_LINUX 或 GNU/Linux ABI note；仅有 SYSV 或 PT_INTERP 不足以切换 persona |
| PT_INTERP | 文件内完整、NUL 终止、长度有界，精确路径 `/lib/ld-capmix-l4a.so`；重复/其他路径拒绝 |
| 解释器 | Linux ET_DYN；受限 PT_LOAD、无递归 PT_INTERP、无 PT_DYNAMIC/PT_TLS；自己必须是无需重定位的 PIC |
| `entry` | 镜像实际初始 RIP，解释器存在时为 load bias + interpreter e_entry |
| `main_entry` | 主程序 e_entry；静态路径与 entry 相等 |
| AT_ENTRY | 主程序入口，不是解释器入口 |
| AT_BASE | 解释器 load bias；只在本轮解释器路径追加，静态栈布局保持原样 |
| AT_PHDR/PHENT/PHNUM | 主程序的用户态 PHDR 地址、56字节表项与数量，不替换成解释器的表 |
| 栈 | 同一份 argc/argv/envp/auxv，16字节对齐；保留 AT_PAGESZ、AT_RANDOM、AT_NULL |
| brk | 从主程序最高 PT_LOAD 页末开始；解释器路径最多到0x600000，不能覆盖解释器 |

内核不解析 PT_DYNAMIC，不执行用户态重定位，也不模拟符号解析成功。
解释器代码会检查 CPL=3 和 auxv、打印标记，以原栈跳转 AT_ENTRY；设置 RDX=0 表示本探针没有 rtld finalizer。
主程序打印另一标记并 exit=0。没有引入 CapMix 私有的“Linux 动态 syscall”。

语义核对以固定 Linux v6.6 的
[create_elf_tables/load_elf_binary](https://github.com/torvalds/linux/blob/v6.6/fs/binfmt_elf.c)
为参考：AT_BASE 是解释器基址，AT_ENTRY/PHDR 描述主镜像；实际启动转向解释器。
仅参考接口，没有复制 Linux GPL 加载器代码。

## 地址、容量与回收

- 保留用户窗口 `[0x400000,0x800000)`、最多128个用户页（512KiB），包含4页栈。
- 解释器开启时主程序的 PT_LOAD 必须在 `[0x400000,0x600000)` 内。
- 解释器固定 bias `0x600000`，各 PT_LOAD 的相对地址/大小限定在1MiB窗口内，最终不越过0x700000。
  **固定地址不是 ASLR/KASLR**；没有随机化展示。
- 栈仍在 `[0x7fc000,0x800000)`。主程序、解释器、栈共享128页容量与同一镜像的 commit 记账。
  当前固定探针为主程序2页 + 解释器1页 + 栈4页，共7页；页表、内核栈等另按原内核分配路径管理。
- VFS 锁覆盖两个镜像的读取；检查文件边界、页重叠、入口必须位于有文件内容的可执行 PT_LOAD、对齐与权限标志。
  沿用已有 RO/W 权限处理；没有新增完整 NX/RELRO 实现。
- 所有映射由同一个 `cm_elf_image_t` 拥有。半成品失败统一 destroy；exit/reap 回收完整镜像。
- fork 保留两种入口/AT_BASE 元数据，沿用驻留 COW 与预留副本；不把 COW 页送入压缩/swap。
- execve 仍先准备新镜像与 argv，成功后才切换；缺解释器返回 ENOENT，旧镜像继续运行。
  沿用现有 errno 转换：加载器资源/格式边界不等于完整 Linux exec 错误全集。

白名单只是实验格式边界，不是签名认证；没有赋予解释器额外权限。
替换同路径文件仍是在进程原有权限下运行用户代码。

## 开关和构建

L4a 需要两个条件同时成立：Linux persona 已启用，且显式 `capmix.linux_interp=1`。
`capmix.linux_interp=0` 优先拒绝；只有 selftest 模式不会自动开启 L4a。
`LINUX_ABI=0` 同时禁止 Linux 静态和解释器路径，不删除任何文件。

```sh
make kernel
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
CAPMIX_CMDLINE='capmix.mode=selftest capmix.linux_interp=1' make test
```

`CAPMIX_CMDLINE` 仅用于 hosted 测试入口快照；不设置时保留默认 hosted selftest。
可以设置 `capmix.linux=0` 测运行时关闭；freestanding 仍从 Limine 读取 cmdline。

```sh
make test                                    # Linux静态测试开启，L4a默认关
CAPMIX_CMDLINE='capmix.mode=selftest capmix.linux=0 capmix.linux_interp=1' make test
CAPMIX_CMDLINE='capmix.mode=selftest capmix.linux_interp=1' make test LINUX_ABI=0
make iso                                     # 默认产品；Linux/L4a均默认关
```

三个固定源码产物：

- `tests/linux_l4_main.S` + `.ld` → `build/linux-l4a.elf`，含 PT_INTERP 的主程序。
- `tests/linux_l4_interp.S` + `.ld` → `build/linux-l4a-interp.so`，只有一个 RX PT_LOAD 的 PIC。
  链接到零后由 `scripts/mark-linux-interp.py` 验证无 REL/RELA 并标记为 Linux ET_DYN；没有宿主 libc。
- `tests/linux_l4_exec.S` → `build/linux-l4a-exec.elf`，调用 Linux execve；失败时证明旧镜像仍能打印和退出。

`readelf -h -l -r build/linux-l4a-interp.so` 可核对 ET_DYN、LOAD 和无重定位。
测试/产品运行的是这些固定构建产物，不下载或冒充宿主 ld-linux/libc。

## 产品路径与日志

产品配置显式增加 `capmix.linux=1 capmix.linux_interp=1` 后，首次补入
`/lib/ld-capmix-l4a.so` 与 `/bin/linux-l4a.elf`；已有文件不覆盖，不覆盖原生工具。
解释器路径关闭时仍拒绝 PT_INTERP，L0–L3 的静态程序照常运行。

```text
run /bin/linux-l4a.elf
```

预期顺序：

```text
linux L4a interp mapped: base=600000 ...
linux L4a image ready: main_entry=... initial_rip=... AT_BASE=600000 handoff-only
linux L4a interp entered: CPL3 auxv verified
linux L4a main entered: hello via interpreter
shell child done: ... exit=0 reap=ok
```

自测还需看到 `malformed/overlap/capacity/commit/missing rollback=ok`、
`CPL3 /bin/linux-l4a-exec.elf verified`、`exec-failure-old-image ... exit=42`，
并以最终 `selftest result: PASS (0 failures)` 为准。

检查保存的日志：`python3 tests/check_linux_l4.py LOG`。
hosted 加 `--hosted`，Linux关闭加 `--disabled`，仅L4a关闭加 `--interp-off`，
带宿主TCP echo验证加 `--wire`。脚本串联 L0–L3 检查，并核对两次交接顺序与回收日志。

## 后续里程碑，不属于本轮已实现

1. 受限动态段与固定 relocation 集：先验证 RELATIVE，再定义符号解析/DT_NEEDED 与依赖搜索边界。
2. 有界映射、munmap/mprotect、文件映射和错误回滚，补足选定真实 ld.so 的内存需求。
3. 固定版本的极小 musl 动态程序：TCB/PT_TLS、启动 syscall 与有限 libc 功能逐项验收。
4. 再研究 glibc：符号版本、IFUNC、TLS、线程/futex、信号、vDSO 等是多个独立里程碑。

不承诺通用 glibc、任意动态 BusyBox、LD_PRELOAD、dlopen、完整共享库搜索路径、TLS或线程。

## 本轮实测矩阵（2026-10-15）

| 环境/配置 | 结果 | 日志 |
| --- | --- | --- |
| hosted L4a开启 | 全量 PASS (0 failures)，映射/auxv/COW/负例回滚；硬件执行明确跳过 | [l4-hosted-on-final.log](../tests/evidence/logs/l4-hosted-on-final.log) |
| hosted 仅L4a关 | 全量 PASS，PT_INTERP拒绝、L0–L3保留 | [l4-hosted-interp-off.log](../tests/evidence/logs/l4-hosted-interp-off.log) |
| hosted Linux运行时关 | 全量 PASS，原生保留 | [l4-hosted-linux-off.log](../tests/evidence/logs/l4-hosted-linux-off.log) |
| hosted LINUX_ABI=0 | 全量 PASS | [l4-hosted-off.log](../tests/evidence/logs/l4-hosted-off.log) |
| QEMU KVM host SMP4 无网卡，L4a开 | 全量 PASS，spawn/exec真实交接、exec失败旧镜像存活 | [l4-on-no-nic-final.log](../tests/evidence/logs/l4-on-no-nic-final.log) |
| QEMU KVM SMP4 e1000，L4a开 | 全量 PASS，L0–L3含TCP wire回归 | [l4-on-e1000-final.log](../tests/evidence/logs/l4-on-e1000-final.log) |
| QEMU KVM SMP4 双NUMA节点+virtio-net/e1000 | 全量 PASS，真实交接及NUMA、EPT、TRACE等原有测试 | [l4-on-dual-vnet-final.log](../tests/evidence/logs/l4-on-dual-vnet-final.log) |
| QEMU 仅L4a关+e1000 | 全量 PASS，静态Linux TCP路径保留 | [l4-interp-off-final.log](../tests/evidence/logs/l4-interp-off-final.log) |
| QEMU Linux运行时关，无网卡 | 全量 PASS | [l4-linux-off-final.log](../tests/evidence/logs/l4-linux-off-final.log) |
| QEMU LINUX_ABI=0，双节点双网卡 | 全量 PASS | [l4-compiled-off-final.log](../tests/evidence/logs/l4-compiled-off-final.log) |
| QEMU 无ISO AHCI安装盘产品shell | run L4a及L0 exit=0；原生重定向/cat、nc、http-get、httpd两次响应、ACPI关机正常 | [l4-product.log](../tests/evidence/logs/l4-product.log)、[l4-native-httpd-client.log](../tests/evidence/logs/l4-native-httpd-client.log) |
| VirtualBox 7.2.20 UEFI/AHCI/e1000、4CPU、无ISO冷启动 | 产品L4a交接exit=0/reap；L0静态程序、持久文件读回和ACPI关机正常；未跑全量debug | [l4-vbox-product.log](../tests/evidence/logs/l4-vbox-product.log) |

开启解释器时的旧L0畸形PT_INTERP负例按解析结果断言EINVAL；关闭时仍断言NOTSUP。
没有把任意错误都视为通过。L0严格页回收检查仍保留，矩阵中每份全量日志都核对before=after。
kernel开/关均无warning/error，ISO构建成功；最终默认ISO恢复product配置。

## 本轮修改文件

- `proc/elf.c`、`include/capmix/elf.h`：解释器加载、两个入口、统一容量/回收与brk边界。
- `proc/elf_args.c`：AT_ENTRY取主入口、解释器路径追加AT_BASE；静态auxv布局保留。
- `abi/linux.c`、`include/capmix/abi.h`：独立实验开关及测试声明。
- `proc/init.c`：显式开启时补入固定主程序与解释器，不覆盖已有文件。
- `boot/hosted.c`、`boot/limine.c`：hosted测试cmdline快照，freestanding仍走Limine。
- `boot/linux_l4_test.c`、`boot/kernel.c`：新自测与调用点。
- `boot/linux_test.c`、`tests/check_linux_l0.py`：旧动态拒绝负例适配新显式开关，保留回收断言。
- `tests/linux_l4_main.S`、`tests/linux_l4_main.ld`、`tests/linux_l4_interp.S`、`tests/linux_l4_interp.ld`、
  `tests/linux_l4_exec.S`、`scripts/mark-linux-interp.py`、`Makefile`：固定探针构建与嵌入。
- `tests/check_linux_l4.py`、`boot/limine-linux-l4-selftest.conf`：日志断言与显式开启配置。
- 本文、`docs/README.md`、`docs/KERNEL_OVERVIEW.zh-CN.md`及VBox手册：能力边界与验收记录。

此前L0回收修复保持原样，其变更记录见[L0_RECLAIM](L0_RECLAIM.md)。

本次固定产物 SHA-256：

```text
f19a379153208090b6aec52702b9e53c262014691772a092176fc863b682bfa9  linux-l4a.elf
ef848ad2d2bdeb6b60eea7a9fbe9f06d7a39f295e3413e38c73ad88c568dedae  linux-l4a-interp.so
c541af2f2d6f1f832f4f30375c5399fd6d2f5d335856e2b8c1268bca55d83210  linux-l4a-exec.elf
```
