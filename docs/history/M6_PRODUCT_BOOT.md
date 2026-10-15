# M6：产品启动与 VMware UEFI

> 历史阶段记录：保留当时实现与验收边界；后续功能可能已扩展。当前状态见
> [内核总览](../KERNEL_OVERVIEW.zh-CN.md)，操作测试见 [VirtualBox手册](../VIRTUALBOX_TESTING.zh-CN.md)。

默认 `make iso` 使用 `boot/limine.conf`，直接挂根、启动 PID1、进入
`capmix>`，不执行 phase 测试。完整回归必须选择：

```bash
make iso LIMINE_CONFIG=boot/limine-selftest.conf
# 或在 Limine cmdline 指定 capmix.mode=selftest
make test                         # hosted 始终是 selftest
make iso                          # 恢复产品 ISO
```

启动横幅例：`init: CapMix 0.8.0-phase8 mode=product root=disk nic=vnet0/up`。
版本使用现有 CAPMIX_VERSION；root 为实际 disk/ram，nic 为实际
vnet0/up、vnet0/down 或 unavailable。up 仅表示驱动报告的链路状态，
不承诺外网、TCP 或生产网络服务；lo 不计作物理/虚拟网卡。

shell 支持 M5 的命令，`run /bin/hello.elf` 启动 CapMix ABI 的静态用户 ELF。
PID1/shell 本身仍是内核 C-entry 任务。`exit` 同步根并进入控制台 idle；
`poweroff` 和 `shutdown` 无参数，先同步根，失败则留在 shell。
同步成功后尝试 ACPI S5；不可用、启用超时或请求未关机时打印
`poweroff: safe-hlt`，停止 BSP（产品模式 AP 调度已禁用）。这不会关闭
宿主机；降级时请从虚拟机管理器关闭 VM。

## VMware：进入 shell

1. 新建 x86-64 VM，选其他 64-bit 客户机，配置 512 MiB 或更多内存、2～4 vCPU。
   关机状态下选择 UEFI/EFI，关闭 Secure Boot；本项目没有 Secure Boot 签名启动链。
   Workstation 的固件选项通常在 Settings → Options → Advanced；vSphere 在
   VM Options → Boot Options，具体菜单随版本变化。
2. CD/DVD 挂载默认产品 `capmix.iso`，勾选开机连接，从虚拟光驱启动。
   保留普通虚拟显示器以获取 GOP；不需要 GPU 加速。
3. 存储/网卡可选。**M7 增加了 SATA/AHCI 支持（见 [M7](M7_AHCI.md)）；virtio 仍仅支持
   legacy/transitional PCI。M8 增加了特定 e1000/e1000e 型号支持（见 [M8](M8_E1000.md)）；
   尚未实现 SCSI/NVMe 或 vmxnet3 驱动。** VMware 配置若不能提供
   受支持的 virtio 或 AHCI 设备，直接使用 RAM 根、网卡 unavailable 进入 shell；
   需明确选择受支持的控制器；其他 VMware 虚拟盘不会自动得到持久根。QEMU 的 raw 盘也不能
   直接当 VMware 可用的 virtio 盘承诺支持。
4. 在 VM 控制台捕获键盘，需固件/平台暴露 i8042/PS2；仅 USB 输入不受支持。
   看到 `shell ready` 和 `capmix>` 后，输入 `help`、`ls /bin`、
   `run /bin/hello.elf`。可配置虚拟 COM1 输出到文件保留日志。
5. 输入 `poweroff` 关闭 VM；不支持 ACPI 时会安全 halt，再从 VMware 关闭。
   VMware 本身未在本次环境实测，不声称 VMware PASS。

VMware 固件参考：[Broadcom UEFI 说明](https://community.broadcom.com/vmware-cloud-foundation/viewdocument/using-efiuefi-firmware-in-a-vmware?CommunityKey=e2de134c-2676-4a0a-9418-0dfb3a8e611e&tab=librarydocuments)。

## ACPI 首版边界与验收

自包含原始实现，依据 [ACPI 6.6 S5](https://uefi.org/specs/ACPI/6.6/16_Waking_and_Sleeping.html)。
校验 RSDP、RSDT/XSDT、FADT、DSDT 校验和与长度；支持根 Name 或根
Scope 内的静态 `_S5_` 整数 Package，以及 16-bit SystemIO PM1 控制寄存器。
需要时通过 FADT 的 SMI enable 有界启用 ACPI（最多一秒及迭代上限）。
不执行 `_PTS`/`_GTS`，不是完整 AML 电源管理；动态 `_S5`、SSDT 提供的
对象、MMIO GAS、hardware-reduced ACPI 等明确降级。不会扫描缓冲区或
方法体中的 `_S5` 字节来猜寄存器值，也不使用硬编码 QEMU 关机端口。

验收日志：`acpi poweroff ready` 或 `acpi poweroff skipped`、`init:`、
`shell ready`；关机时 `poweroff: root sync=0` 与 `ACPI S5 request` 或
`safe-hlt`。debug/hosted 必须有 `selftest result: PASS (0 failures)`。
关机解析 selftest 覆盖根 Scope、截断、元素计数、Buffer/Method 伪对象，
不会在回归测试中真的关机。
