# 可选 Linux 包：安装、启用与卸载

首版沿用现有**宿主离线安装/管理 RAW 镜像、下次启动生效**的工作流；没有 guest 内包管理器、
在线切换或内核热卸。包是内核已有 Linux persona + 内置的固定测试程序配置，不是 Linux 发行版、
容器、独立下载的 glibc 或通用 BusyBox。原生 CapMix ABI/程序保持独立；新原生软件仍建议 `.faz` 或 `/bin/cm/`。

默认产品 Linux 关闭。此工具启用稳定的 L0–L3 路径，明确写入 `capmix.linux_interp=0`，
不自动开启实验性 L4a。L1 仍是**定制 musl 静态 BusyBox 子集 + 有界 BSS 分配**。

## 安装时选择

```sh
make kernel LINUX_ABI=1
make install-disk DISK_IMAGE=/tmp/capmix-system.img INSTALL_LINUX=1
# 或：
python3 scripts/install-disk.py /tmp/capmix-system.img --linux
```

不传 `INSTALL_LINUX=1` / `--linux` 时保持原默认配置；已有显式自定义配置仍按原参数处理。
安装器继续拒绝未知非空盘、已有安装盘、设备节点、symlink/hardlink；不改变安装分区布局。
`--linux` 检查所选内核的编译支持标记，生成产品 cmdline，再使用原安装器写新盘。
首次从盘启动补入缺失 Linux 测试 ELF 与 `/data/linux`，并进入原生用户 init/shell。
已有同名程序、数据不覆盖。原生 `/bin` 工具照常安装。

包内入口沿用 `/bin/linux-hello.elf`、`/bin/busybox-l1.elf`、`/bin/linux-net.elf`。
`/data/linux` 是约定的数据目录，**不是 chroot/容器边界**；Linux 文件 syscall 仍使用同一 VFS 根。
软件若把数据写在其他位置，卸载时不会替它猜测、移动或删除。

## 装后管理

先正常 `poweroff`，关闭 VM；只操作离线 RAW 安装镜像：

```sh
make build/linux-package
python3 scripts/linux_package.py status /tmp/capmix-system.img
python3 scripts/linux_package.py enable /tmp/capmix-system.img
# install 是 enable 的同义词
```

下一次启动 persona 才启用；`status` 显示的是**磁盘上的配置**，不是活跃 VM 状态。
`enable` 会准备已有 COW 根的 `/data/linux`，空白未首启的根不格式化，交给首次产品启动处理。
启用所需内核必须包含本版 `CapMix.LinuxPersona.v1:compiled=1` 标记；编译关闭或缺少标记的旧内核拒绝。
旧内核需要先更新 ESP 中内核或用新内核重新准备新安装盘；工具不自动替换内核、不重新安装已有盘。
支持标记用于配置一致性，不是数字签名/verified boot。

启动后核对 `linux persona: compiled=1 enabled=1`、`linux package ready`，然后：

```text
run /bin/linux-hello.elf
echo example-data > /data/linux/note.txt
run /bin/cat.elf /data/linux/note.txt
poweroff
```

## 卸载默认保留数据

```sh
python3 scripts/linux_package.py uninstall /tmp/capmix-system.img
# disable 是相同操作
```

下次启动 Linux persona 关闭，Linux ELF 被拒绝。`/data/linux`、目录外文件、Linux 测试 ELF、
原生 `/bin` 全部保留；以后 enable 可以继续使用原数据。
这里的“卸载”是关闭可选兼容运行环境，不是从内核二进制中剥离代码，也不删除原生软件。
`LINUX_ABI=0` 是另一个编译级禁用方式；编译关闭内核下 enable 会拒绝，原生路径仍可用。

## 显式删除 Linux 数据

```sh
python3 scripts/linux_package.py uninstall /tmp/capmix-system.img --purge-data
```

该选项明确授权删除**当前 live COW 根的 `/data/linux` 及全部子目录/文件**，包括目录本身。
不删除 `/data`、`/data/outside`、`/bin` 或任何目录外文件；没有用户自定义删除根参数。
只允许与 disable/uninstall 使用，enable/status 搭配它会拒绝。
重新 enable 会创建空数据目录；被删除的数据不会自动恢复。

这不是安全擦除：已有 COW snapshot、旧 generation、磁盘备份可能仍包含旧内容，本轮不销毁快照或备份。
首版清理遍历限制为深度32、最多512个对象、路径小于128字节；超限或异常则整次 staging 不发布。

宿主依赖 Linux/POSIX、Python3（fcntl）、mtools 与构建环境；Windows宿主可在关机后将RAW副本交给构建机处理。

## 发布与拒绝策略

- 工具只接受当前固定分区布局、单链接普通 RAW 文件；拒绝设备、链接、未知布局及文件身份变化。
- 获取 flock 和 POSIX 锁，拒绝被 QEMU 等占用的镜像。不得绕过锁让其他程序并发改写。
- 在同目录完整 staging 副本中修改 ESP 配置和 COW；COW helper 复用原 VFS/COW 实现，**只 mount，不 mkfs**。
- 验证配置读回、数据 remount，再 fsync、检查原镜像身份并原子替换；发布前失败保留原镜像。
- `status` 也在 staging 上挂载读取，始终不发布修改；不把恢复/修复动作写回原盘。
- 配置只接受单个Limine启动项，kernel_path为 `boot():/boot/capmix.elf`，且恰有一个 installed product cmdline；多启动项、selftest 配置或超出255字节的 cmdline 拒绝。
- 更新使用完整副本，需要额外约一份镜像大小的空间；不支持 VDI/VMDK 原地修改。
  VBox 请在 VM 关闭时导出/使用 RAW 的离线副本，并按原测试手册转换、重新挂盘。

L4a、TCP/RTO、KPTI、buddy、调度和文件系统磁盘格式不因包配置改变。

## 验证入口

```sh
python3 tests/test_linux_package.py
make build/test-linux-package-fixture
```

fixture 只用于可丢弃的已初始化测试盘：seed 创建 Linux 子目录和目录外哨兵，retained/purged
检查默认保留或仅清理前缀。测试盘必须离线，禁止对实际用户数据盘运行 seed。
完整环境结果与本轮修改文件见下文。

## 本轮修改文件

- `scripts/linux_package.py`：离线 enable/install、disable/uninstall、status；staging/锁/配置验证。
- `scripts/install-disk.py`、`Makefile`：`--linux`、`INSTALL_LINUX=1` 和 helper/fixture 构建。
- `tools/linux-package.c`：复用现有 COW/VFS 的受限数据目录准备/清理，remount验证。
- `abi/linux.c`：只增加编译支持标记；persona、syscall、TCP语义不变。
- `proc/init.c`：Linux已启用时准备 `/data/linux`，已有数据保留。
- `tests/test_linux_package.py`、`tests/linux_package_fixture.c`：拒绝策略与实际COW前缀/保留数据检查。
- 本文、`docs/README.md`、`docs/INSTALL_DISK.md`、VBox手册：操作边界与实测记录。

本轮保留了之前L0回收修复和L4a改动，未回滚它们，也没有修改fs/cowfs、KPTI、buddy或TCP/RTO核心。

## 实测矩阵（2026-10-16）

| 环境/场景 | 结果 | 日志 |
| --- | --- | --- |
| hosted Linux开/编译关 | 全量 `PASS (0 failures)` | `/tmp/linux-package-hosted-final.log`、`/tmp/linux-package-hosted-off.log` |
| QEMU KVM SMP4 Linux开，无网卡 | 全量 `PASS (0 failures)`，L0回收严格相等 | `/tmp/linux-package-selftest-no-nic-final.log` |
| QEMU KVM SMP4 双NUMA节点 | 全量 `PASS (0 failures)` | `/tmp/linux-package-selftest-dual-no-nic-final.log` |
| QEMU KVM SMP4 编译关闭 | 全量 `PASS (0 failures)` | `/tmp/linux-package-selftest-compiled-off-no-nic-final.log` |
| `--linux`新盘，无ISO AHCI首启 | Linux ELF exit=0，数据目录写入/读回，原生文件持久写入 | `/tmp/linux-package-first.log` |
| 默认卸载 + QEMU冷启动 | Linux拒绝；Linux数据与原生文件保留，原生hello exit=0 | `/tmp/linux-package-disabled.log`、`/tmp/linux-package-disable-retain.log` |
| 清理子目录 + 再启用 + QEMU冷启动 | remount显示删除4对象、目录外哨兵保留；空目录重建，Linux hello/定制BusyBox echo exit=0 | `/tmp/linux-package-purge.log`、`/tmp/linux-package-reenabled.log` |
| compiled-off内核拒绝enable/安装 | 启用前后镜像哈希不变；安装不创建新镜像 | `/tmp/linux-package-unsupported.log` |
| 深目录超限/部分staging清理失败 | 拒绝发布，原镜像SHA-256完全不变 | `/tmp/linux-package-deep-refusal.log` |
| VirtualBox 7.2.20 UEFI/AHCI、4CPU、无ISO | 开启后Linux ELF运行和数据写入；关机导出RAW卸载+清理，再冷启动Linux被拒、目录外文件/原生hello/关机正常 | `/tmp/linux-package-vbox-enabled.log`、`/tmp/linux-package-vbox-disabled.log`、`/tmp/linux-package-vbox-uninstall.log` |

VBox本轮验证产品流程，未跑全量debug。没有声称本轮重新完成真实NIC TCP回归。
5项Python单测通过，覆盖配置幂等、歧义/未知镜像/链接拒绝、清理选项约束及支持检查；
另实际验证status不改原镜像、持锁镜像拒绝。kernel/helper构建无新增warning/error，ISO成功。
清理的物理写入仅发生在自建测试盘或其staging/RAW副本，未处理实际用户数据盘。
