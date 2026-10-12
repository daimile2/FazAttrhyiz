# M7：AHCI / SATA 最小存储

新增原始自包含驱动，按 [Intel AHCI 1.3.1](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf)
实现 PCI class 01/06/01 枚举、ABAR 不可缓存映射、BIOS/OS ownership
handoff、SATA signature 检测与 IDENTIFY。首版选择第一个受支持的 AHCI
控制器上的第一个活动 SATA 数据盘：512B 逻辑扇区，LBA48，FLUSH CACHE
EXT，容量至少 8 MiB。ATAPI 光驱不会当数据盘。无设备/不支持则 skip。

一个 slot、一个 PRDT、4 KiB DMA bounce、同步轮询；无 NCQ、热插拔、
分区解析、安装器、多盘调度或用户态 AHCI runtime。NVMe 留后续。
初始化时 quiesce 所有 implemented ports，再启用 bus mastering。错误/
超时后停止端口并禁用控制器 DMA；确认 engine 停止才释放 DMA 页，否则
隔离保留并拒绝再次初始化。TSC 校准前使用迭代上限；校准后也有时间
上限（普通等待最多 1 秒/1000 万次，ownership 最多 2 秒/2000 万次）。

## 根和安全边界

默认选择 virtio-blk → AHCI → RAM。既有 virtio 驱动保持不变。每个后端
有独立 offline 状态，旧 `cm_blk_select(0/1)` 含义保持 RAM/virtio；
新 backend API 用于保存/恢复 AHCI。AHCI 根的横幅为 `root=disk`，
详细日志为 `product root=ahci COW`，shell 命令集不变。

COW 使用整盘前 8 MiB 的既有布局，仅当该区域完全为空才自动格式化；
非空未知/损坏卷拒绝挂载，不自动覆盖。没有受支持的驱动时用 RAM 根；
已选真实盘但挂载失败时保留现有产品拒绝启动策略。
请使用专用新盘，不能把有分区/系统数据的 SATA 盘当本项目安装盘。

产品启动不会执行原始扇区测试。debug 才测试最后一个扇区（位于 8 MiB
卷之外）：备份 → 写已知 512B → FLUSH → 读回比较 → 恢复 → FLUSH →
再次比较。恰好 8 MiB 没有多余扇区则跳过该测试；COW 持久测试仍可运行。
真实设备故障导致无法恢复时报告 FAIL，不伪称恢复成功。完整 debug
pipeline 使用真实根 → RAM scratch → 原后端恢复，避免 phase3/4 的
破坏性/离线测试影响真实根。`virtio_requests/virtio_flushes` 只统计
virtio 驱动，不代表 AHCI 请求数。

## QEMU

仅第一次创建**不存在的专用盘**；后续启动不要重新创建/截断：

```bash
qemu-img create -f raw /tmp/capmix-sata.img 64M
make kernel
make iso LIMINE_CONFIG=boot/limine-selftest.conf
# QEMU -device ahci 是 ich9-ahci 的别名；这里使用明确名称。
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso \
  -drive id=disk,file=/tmp/capmix-sata.img,if=none,format=raw \
  -device ich9-ahci,id=ahci \
  -device ide-hd,drive=disk,bus=ahci.0 \
  -serial stdio -display none -no-reboot
# 再跑同一命令，应见 m3 persist previous boot verified。

make iso                         # 默认产品 shell
# 复用以上盘参数；改为 -display gtk 后输入：
# ls /bin
# run /bin/hello.elf
# poweroff
```

UEFI 可使用 OVMF code + 独立复制的 vars pflash，保持 Secure Boot 关闭；
不要同时启动两个 QEMU 写同一盘。无 AHCI 不加上述盘/设备参数即可验证
`ahci skipped` / `ahci selftest skipped` 和完整 debug PASS。hosted 始终
skip 硬件，仍执行其余回归。添加 virtio-blk 时它优先作为根。

日志关键字：`ahci PCI`、`ahci SATA disk`、`ahci ready`、
`ahci read/write selftest: PASS bytes=512 restore=verified`、
`m3 persist ... remount ... verified`、`m3 mount restored: '/' ahci`、
`selftest result: PASS (0 failures)`；产品 `init: ... root=disk`、
`product root=ahci COW`、`shell ready`、`hello from elf`。

## VMware SATA

沿用 [M6 UEFI 启动步骤](M6_PRODUCT_BOOT.md)：选择 x86-64 UEFI、关闭 Secure
Boot，挂产品 ISO。新建**专用空虚拟硬盘**，控制器类型选择 SATA/AHCI，
不要选择 NVMe、SCSI/PVSCSI 或 IDE；让 ISO 作为启动介质，数据盘作为
独立 SATA 磁盘。容量至少 8 MiB（建议 64 MiB 或更大），保持 512B 逻辑
扇区。虚拟光驱可为 SATA ATAPI，本驱动会跳过它。

VMDK 是宿主文件格式，客户机通过 AHCI 访问扇区；不能直接把 raw 文件
路径冒充 VMDK。成功应见 `ahci ready` 和 `root=disk`；无受支持的 SATA
控制器/盘时回退 RAM。SATA 支持不等于新增 VMware 网卡支持，网卡仍可能
unavailable。本轮用 QEMU BIOS/UEFI 验证，不声称 VMware SATA 实测 PASS。
