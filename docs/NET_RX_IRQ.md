# 网卡 RX：e1000 / virtio-net 硬件 IRQ 与空闲等待

## 本轮支持范围

实现 **e1000 与 legacy virtio-net 的 INTx RX IRQ**。已验证 QEMU 默认 PC/i440FX：
PCI firmware Interrupt Line 对应 ISA GSI，e1000 使用 IOAPIC vector **0x42**，
virtio-net 使用 **0x43**，
active-low/level、目标 BSP。键盘仍用 0x41，LAPIC timer 仍用 0x40。
两个独立 PCI 路由槽位，不支持共享 IRQ handler 链、MSI/MSI-X 或 ACPI `_PRT` 解释。
若两个设备的固件 Interrupt Line 相同，先注册者保留 IRQ，后注册者明确回退轮询；
产品初始化先 virtio-net，因此这种冲突时它保留 IRQ。不同 GSI 可各用独立路由，
停用某设备仅 mask 该设备路由，不影响另一个槽位。

路由仅在已知 i440FX host bridge `8086:1237` 上启用；未验证的 q35/VMware/其他
root complex、无有效 PCI Interrupt Line、无 IOAPIC 时明确打印 skip，保留原网卡
收发能力。不会把未知 firmware Interrupt Line 猜成可靠 GSI。
virtio-net 仍仅支持现有 legacy PCI transport，modern-only 设备沿用 skip。
无任何网卡则网络自测 skip。

## 真实收包链

1. 初始化 e1000 RX/TX ring 后注册 IDT/IOAPIC 路由，清除 PCI INTx-disable，
   IMS 打开 RXDMT0/RXO/RXT0；其他中断不启用。
2. 设备 DMA 把帧写进 RX descriptor ring，触发真实 INTx。
3. 新 NIC stub 保存通用寄存器、对齐 C 栈、沿用现有 entry CR3 保存/恢复宏。
   top half 读 ICR 确认设备原因，设置接口通知 bit，最后 LAPIC EOI。
4. top half **不解析包、不分配、不打印、不取得 driver/TCP/rq 锁、不直接调度**。
   IRQ 自然唤醒 CPU，通知由调度查询点的 deferred hook 处理，唤醒 BLOCKED RX worker。
5. worker 按每接口 16 帧预算读取 DMA ring，沿原 ARP/UDP/TCP/DHCP ingress 路径
   解析、过滤、入队。预算用尽保留通知继续 drain，避免只处理第一批。

内核只增加 native 网卡入口，用户 UDP/TCP syscall、capability ABI、TCP RTO/状态机、
DHCP 状态机不变。e1000 停止时先 mask IMS/ICR 和其 IOAPIC 路由，再走已有 DMA 回滚。

virtio-net 的对应路径：初始化仍抑制两队列中断；确认 IOAPIC 路由可用后，清 RX
`VIRTQ_AVAIL_F_NO_INTERRUPT`，保持 TX 中断抑制，清 PCI INTx-disable。IRQ top half
只读 legacy ISR（I/O BAR+19），统计真实中断、原子通知 RX worker、LAPIC EOI；配置
变化只设置 pending bit，link 状态在 worker 中更新。已有 used-ring DMA 校验和 drain
保持不变。TX 仍同步等 used-ring completion，**IRQ 模式下 TX/RX 轮询均不读取 ISR**，
避免读清 RX 事件。队列初始 arm 后另发软件通知，收尾早于 arm 的 completion；它不
增加硬件 IRQ 计数。设备失败时先 mask 自己的0x43路由，再 reset/禁 DMA，沿用原
释放或 quarantine 策略。没有新增队列、MSI-X、offload 或 TCP 算法。

ISR 的 read-to-clear 和 available-ring 中断抑制依据
[OASIS Virtio 1.2](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.pdf)
的 PCI ISR 与 split virtqueue 定义。TX 抑制是向设备提出的优化请求，不能据此承诺
设备绝不产生其他中断；handler 会安全确认实际 cause。

## 空闲与 fallback

- worker 没有通知时使用现有 block-prepare/context swap 进入 **BLOCKED**，不再
  持续 runnable/yield 扫 DMA ring。准备等待与 deferred wake/stop 使用同一 IRQ-save 锁，
  防止 SMP 丢失唤醒；硬 IRQ 只原子设置 bit，不碰这个锁。
- `cm_sched_runnable()` 在 rq 锁外调用一个 trusted poll hook，处理通知或到期唤醒。
  新接口是单个内部 hook，不是用户 syscall，不是新的通用 hrtimer 框架。
- 最多每 **10ms** 定时唤醒 worker，推进既有 `cm_tcp_poll()`；不改 TCP 算法或时钟标定。
- IRQ 已路由的 e1000/virtio-net 主要按通知 drain；另每 **100ms** 作一次 descriptor 健康检查，
  兜底漏通知/排队中的旧帧。这不是完全禁用轮询。
- IRQ 未启用的 virtio-net/e1000 在 worker 运行时轮询，空闲时约 **10ms** 一次；
  有其他网卡事件时可能更频繁。参数是轮询/唤醒目标，非实时延迟承诺。
- stop 主动唤醒 sleeper，再执行原有有界 join；不会留下永久 BLOCKED 的 worker。

仅网络 worker 从空转改成等待；现有 AP scheduler pause 循环、用户 shell 的 PS/2
轮询没有重构，**不宣称整机宿主 CPU 占用已降到零**。IRQ 合并通知 bit 不是包队列，
包仍在有限硬件 RX ring 中；不承诺过载无丢包。

## 日志与 selftest

```text
irq rx route: PCI INTx GSI=11 ... vector=0x42 ... level/low
e1000 irq rx enabled: INTx line=11 ...
net bg RX started: ... idle=BLOCKED timer=10ms watchdog=100ms
irq rx verified: enet0 slept=1 hardware_delta=N irq_udp_queued=N payload=1730 queue-before-recv=1
net irq rx stats: hw=N vnet_hw=N frames=N wakes=N sleeps=N ...
```

P1b 的测试在发包前确认 worker 已 BLOCKED；真实网卡 UDP echo 验证
1/257/1472B（总 1730B）。两种网卡的 IRQ 模式均额外断言硬件中断计数增加、**在 IRQ 通知批次
中入队的 UDP 计数增加**，并保留原 queue-before-recv 和 payload 比较。
因此不是只读一个 IRQ flag 就假称已收到用户数据。

`frames` 统计 IRQ 通知批次 drain 的所有帧（含 ARP），`irq_udp_queued` 专门统计
该批次通过 ingress 入队的 UDP；一次 IRQ 可以对应多帧。`wakes` 是通知唤醒次数，
`sleeps` 是 worker 成功准备 BLOCKED 的次数，均允许合并、不是一包一 IRQ。

## 验收命令

宿主分别开服务：

```bash
python3 scripts/tcp-echo.py
python3 scripts/udp-echo.py
```

在另一个终端：

```bash
make test
make kernel 2>&1 | grep -iE 'warn|error'
make iso LIMINE_CONFIG=boot/limine-tcp-rto-selftest.conf
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -cdrom capmix.iso -serial stdio -display none -no-reboot
```

应包含上述 IRQ 验证，以及 DHCP lease/fallback、TCP RTO 恢复、用户 TCP ELF、
`selftest result: PASS (0 failures)`。无服务时使用普通 `boot/limine-selftest.conf`。
`-net none` 验证无设备 skip；`-machine q35` 验证未验证路由 skip+10ms fallback；
默认 i440FX 改成 virtio-net-pci 则验证硬件 IRQ；q35 则验证 fallback。默认 `make iso` 仍进入产品用户 shell。

产品可依次 run `/bin/tcpwire.elf &`、`/bin/filetest.elf`、`/bin/udpwire.elf`、poweroff，
核对各进程 exit=0；PS/2 输入和 FB+serial 输出保留。无需修改磁盘安装布局。

## 原 e1000 实测记录

- `/tmp/rxirq-hosted.log`：全量 hosted PASS；硬件路径明确 skip。
- `/tmp/rxirq-e1000-final.log`：4 CPU，`slept=1 hardware_delta=4 irq_udp_queued=3`，TCP RTO/用户 ELF 及全量 PASS。
- `/tmp/rxirq-dual-nic.log`：双 NUMA、virtio-net+e1000 并存，IRQ 和 fallback 两路径，全部网络测试与全量 PASS。
- `/tmp/rxirq-q35.log`：未验证的 root complex 路由 skip，降频 fallback，TCP/UDP/DHCP 和全量 PASS。
- `/tmp/rxirq-nonic.log`：无 NIC skip，全量 PASS。
- `/tmp/rxirq-product-final.log`：用户 shell 运行 TCP 后台 ELF、文件和 UDP ELF 均 exit=0，PS/2 输入及 ACPI 关机正常。

本轮修改 IRQ 入口/IOAPIC 文件、e1000/runtime、少量 scheduler deferred hook、
对应头文件、P1b 断言及本文档。新增 IRQ stub/IDT 路由属于 AGENTS.md 的人审关键路径；
沿用既有 entry ABI，没有改 KPTI 策略、TCP 状态机、buddy/时钟或安装布局。

## virtio-net IRQ 续作验收

将上面 QEMU 的 `-device e1000,netdev=n0` 换为
`-device virtio-net-pci,netdev=n0`，其余宿主 echo 服务、debug 配置不变。
应出现：

```text
virtio-net irq rx enabled: INTx line=11 vector=0x43 ...
irq rx verified: vnet0 slept=1 hardware_delta=N irq_udp_queued=N payload=1730 queue-before-recv=1
net irq rx stats: ... vnet_hw=N ...
selftest result: PASS (0 failures)
```

`hw` 仍指 e1000 硬件计数，新增 `vnet_hw` 独立记录 virtio-net；`frames/wakes` 是
worker 的合并统计。硬件计数只在真实 ISR cause 非零时增加，不把 arm 软件通知、
watchdog 或预算续跑当硬件中断。

产品 nc 验证：`make install-disk DISK_IMAGE=/path/new.img` 创建新盘，无 ISO 启动，
使用 virtio-net 与宿主 TCP echo，在 shell 输入 `run -i /bin/nc.elf 10.0.2.2 9998`，
输入文字后检查 tx/rx，再 `/quit`、`poweroff`。详见 [EXAMPLES.md](EXAMPLES.md)。
已有盘的内核升级与 shell 更新不在本次范围，不自动覆盖文件或安装布局。

IRQ stub/IDT 路由仍属 AGENTS.md 人审关键路径；新 stub 沿用 e1000 的寄存器保存、
栈对齐、entry CR3 宏，未更改 KPTI 策略、时钟、buddy、TCP/RTO 或 DHCP 核心。

本轮实测（4 CPU）：
- `/tmp/vnetirq-hosted.log`：hosted 全量 PASS。
- `/tmp/vnetirq-single.log`：virtio-net，硬件 IRQ delta=4、IRQ UDP入队=3、payload=1730，全量 PASS。
- `/tmp/vnetirq-numa.log`：双节点双网卡，共享 GSI 明确回退 e1000，virtio IRQ 与全量 PASS。
- `/tmp/vnetirq-e1000.log`：e1000 单独使用仍保留硬件 IRQ，全量 PASS。
- `/tmp/vnetirq-fallback.log`：q35 virtio-net 明确 skip IRQ，UDP/TCP/DHCP fallback 全量 PASS。
- `/tmp/vnetirq-product.log`：无 ISO virtio 磁盘启动，DHCP ACK、nc tx/rx各16B、exit=0/reap=ok，cat与poweroff成功。
