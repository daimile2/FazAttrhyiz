# CapMix 磁盘安装与无 ISO 冷启动

## 首版安装方式与范围

本版为**宿主命令行安装工具 + 首次从盘启动完成 COW provisioning**。不需要产品 ISO 提供根，也没有 guest 内安装命令、分区安装 GUI 或真实宿主块设备写入功能。

1. `make install-disk` 在新路径或全零的普通镜像文件中写入 MBR、Limine BIOS stages、FAT32 ESP、UEFI fallback loader、内核与产品配置。
2. 首次**不带 ISO**启动这个盘。内核识别专用卷，把卷内 raw/swap/COW 地址转换为物理 LBA；空白卷按既有 M3 策略格式化，bootstrap 将缺失系统 ELF 写入 COW 并 fsync。
3. init ELF 正式挂载磁盘根，启动用户 shell。正常 poweroff sync 后退出 QEMU。
4. 第二次启动同一个盘，仍不带 ISO。挂载已存在的 COW；用户 init/shell 及其他文件从磁盘读取，不再格式化。

安装工具输出 `install boot staged` 表示引导已准备，并不声称 COW 已初始化。首次启动的 `disk install: system ELF provisioning synced; root=disk ... ready`、CPL=3 init/shell 日志才证明根系统文件连通。

## 固定布局

默认镜像 128 MiB；至少 128 MiB，512B 对齐，容量受 MBR 32-bit LBA 约束。不是扩容 COW 文件系统。

| 物理范围 | 用途 |
|---|---|
| LBA0 | MBR；disk ID `0x43504d58`，2 个固定分区，签名 55aa |
| LBA1..2047 | Limine BIOS stage2 空间；不是 raw selftest/swap |
| LBA2048..18431 | 8 MiB CapMix 卷，MBR type 0xda |
| LBA18432..149503 | 64 MiB FAT32 ESP，type 0xef、active，label CAPMIXBOOT |
| 其后 | 首版未使用，不扩大文件/页/卷容量 |

8 MiB 卷内部保持原布局：logical raw `[0,64)`、swap `[64,8256)`、COW `[8256,16384)`。安装盘 COW 物理起点为 2048+8256=10304；文件系统仍为 1016 个 4 KiB block。RAM 和旧未分区 M3 数据盘仍使用 base=0。

ESP 中包括 `/EFI/BOOT/BOOTX64.EFI`、`/boot/limine/limine-bios.sys`、`/boot/limine/limine.conf`、`/boot/capmix.elf`。Limine 从 ESP 读内核；运行期 VFS `/` 是独立 COW 卷，不是 FAT/ISO。配置源为 `boot/limine-disk.conf`，包含 `capmix.installed=1`。

内核严格校验 disk ID、signature、分区类型/状态/固定起点/长度、额外分区和设备容量。带标识但非法的布局禁用该后端 root I/O，不退回物理 LBA0。installed 模式选中的后端若没有有效安装布局，拒绝以 RAM/raw 根代替。没有设备的普通 ISO 路径仍 skip 并走 RAM 回退。

物理 AHCI capacity 通过只读 getter 提供；只在块层做 LBA offset，未改 COW 磁盘格式或 KPTI/buddy/timer/并发入口。后端优先级仍为 virtio-blk > AHCI。本版测试/安装时只连接一个支持的系统盘，尚不按 Limine boot device UUID 选择多盘根。

## 安全边界与依赖

安装工具没有 force/重装/迁移选项：

- 新路径使用临时完整镜像并原子发布，拒绝覆盖同时出现的目标。
- 已有镜像必须为单链接普通文件、全盘每字节为零；整盘扫描，尾部非零也拒绝。
- 拒绝设备节点、目录、symlink/hardlink、容量不足和被锁定镜像。已打开文件使用 flock + POSIX 排他锁；不要绕过锁并发修改目标。
- 写入发生在 staging 镜像，Limine 完成后再次验证分区表/标识未变、整个 CapMix 卷仍为零，才发布；发布前复验既有目标身份和全零状态。
- 已有 M3 数据盘、已安装盘都拒绝改写。其格式迁移/升级不是本轮功能。
- 内核保持 M3：有效 COW 正常挂载；保留卷窗口非空且无有效 superblock 时拒绝自动格式化。未知盘不因安装功能而清空。

宿主依赖：gcc/make/Python3、dosfstools (`mkfs.fat`)、mtools (`mmd/mcopy/mdir`)、Limine 发布二进制与对应 host installer 源。Makefile 自动编译现有 `third_party/limine/limine`；不替换仓库中的第三方发布文件。单独运行 Python 工具前可先 `make -C third_party/limine`。

ISO 仍是可选调试/安装辅助介质。此次补齐 `-efi-boot-part --efi-boot-image`，确保构建 host Limine 工具后 `make iso` 的 bios-install 不再因旧 hybrid 分区布局失败；参考 [Limine 官方 hybrid ISO 用法](https://github.com/Limine-Bootloader/Limine/blob/v12.x/USAGE.md)。

## AHCI / BIOS：两次无 ISO 启动

```
make kernel
make install-disk DISK_IMAGE=/tmp/capmix-system-ahci.img
```

不存在时新建 128 MiB；如需先创建容量更大的全零镜像，可 `qemu-img create -f raw /tmp/capmix-system-ahci.img 256M`，再执行同一 install 命令。安装必须在 QEMU 未运行时进行。

```
timeout 60 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -drive id=disk,file=/tmp/capmix-system-ahci.img,if=none,format=raw \
  -device ich9-ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
  -net none -serial stdio -display gtk -no-reboot
```

没有 `-cdrom`。首轮通过 PS/2 输入：

```
run /bin/filetest.elf
poweroff
```

再执行完全相同的 QEMU 命令，输入：

```
run /bin/filetest.elf
run /bin/argtest.elf disk coldboot
poweroff
```

第二轮应有 `root=disk backend=ahci`、`shell ELF ready: CPL=3`、`p1a previous boot payload verified`、child exit=0/reap=ok，且没有 `blank disk volume — formatting`。

## virtio / UEFI：两次无 ISO 启动

```
make install-disk DISK_IMAGE=/tmp/capmix-system-virtio.img
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/capmix-system-vars.fd
```

使用匹配的 CODE/VARS 对，关闭 Secure Boot；可按系统实际 OVMF 路径调整。

```
timeout 60 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file=/tmp/capmix-system-vars.fd \
  -drive file=/tmp/capmix-system-virtio.img,if=virtio,format=raw \
  -net none -serial stdio -display gtk -no-reboot
```

仍无 `-cdrom`。第一轮 filetest + poweroff 后，结束进程并重新运行同一命令（不是 guest reset）；第二轮再 filetest、`run ... &`、poweroff，核对 previous-boot 内容及 root=virtio-blk。UEFI firmware 在 ESP 使用 fallback `EFI/BOOT/BOOTX64.EFI`。

VMware 可用 `qemu-img convert -f raw -O vmdk /tmp/capmix-system-ahci.img /tmp/capmix-system.vmdk` 后接作 SATA/AHCI 系统盘，选 UEFI、关闭 Secure Boot、卸下 ISO。请保留原镜像备份；没有验证所有 VMware 固件/控制器组合，NVMe 首版不支持。MBR/FAT32 是本版布局，不是 GPT 安装器。

## 回归与限制

```
make test
python3 tests/test_install_disk.py
make kernel 2>&1 | grep -iE 'warn|error'
make iso LIMINE_CONFIG=boot/limine-selftest.conf
```

debug 带安装盘时显式用 `-boot order=d -cdrom capmix.iso`，否则 BIOS 可能先启动安装盘中的产品配置。M3 将破坏性 selftest 切到 scratch RAM，最终恢复带 offset 的真实根；复制测试盘后再跑，不要把日常系统盘用于压力/崩溃测试。

`io/disk_layout.c` selftest 覆盖 legacy、offset、容量、布局损坏拒绝。宿主 6 项测试覆盖尾部非零、symlink/hardlink、容量、锁、工具失败不发布。无网卡时协议测试按原策略 skip；网络驱动、TCP/UDP 和用户 shell 核心没有改动。

此次 no-NIC debug 暴露旧 overlap 自测的基线问题：之前的 IPC backgroundd 仍在队列，AP 会在测试期间释放它的 4-page 栈，导致 free pages 比基线多 4。仅在并发自测采样前让之前任务自然完成并确认队列为空，保留原精确页数/commit 检查；未改并发入口或 buddy。

不支持真实设备直接安装、已有数据盘迁移、GPT/分区编辑器、双系统管理、动态根 UUID 选择、原子内核/ESP 升级、Secure Boot/verified boot、加密根或完整断电安装恢复。系统文件沿用“缺失才 provision”；内核在 ESP 中的升级和 COW ELF 更新需后续受控工具，不应重跑此 blank-only 安装器。

## 可选 Linux 包配置

`make install-disk DISK_IMAGE=/tmp/new.img INSTALL_LINUX=1` 或宿主安装器的 `--linux`
可在首次安装时请求 Linux persona。未选择时默认仍关闭；L4a不随此选项开启。
装后离线启用/卸载、`/data/linux`默认保留及显式清理见 [LINUX_PACKAGE](LINUX_PACKAGE.md)。
不增加guest安装命令，不重新格式化已安装盘，不热卸内核。
