# VBox ICH9：e1000 INTx RX 与原生 HTTP A/B

2026-10-19，默认产品Linux仍关闭；未改TCP/RTO、调度、定时器、KPTI或buddy。
本轮只增加受限路由探测、强制轮询参数、RX worker首次IRQ收包日志，以及基准的原生HTTP模式/延迟统计。

## 能力探测与回退

i440FX使用既有PCI Interrupt Line路由。新增VBox ICH9仅接受：

- e1000是Intel 82540EM（100e）、位于root bus 0、Interrupt Pin在1..4。
- host bridge为缺省隐藏（ffffffff）或29e0:8086，LPC在00:1f.0为27b9:8086或2918:8086。
- root bus存在VirtualBox VMMDev（cafe:80ee）。不只根据“ICH9”名字或vendor ID猜GSI。
- MADT发现的IOAPIC覆盖目标GSI，现有路由函数接受该GSI/向量且未被另一个NIC占用。

依据[VirtualBox官方DevPciIch9.cpp](https://github.com/VirtualBox/virtualbox/blob/main/src/VBox/Devices/Bus/DevPciIch9.cpp)
的 `ich9pciSlot2ApicIrq` 与 `ich9pciApicSetIrq`：root GSI = 16 + ((slot + pin - 1) & 7)。
该文件还明确禁用host bridge注册；[官方DevLpc.cpp](https://github.com/VirtualBox/virtualbox/blob/main/src/VBox/Devices/PC/DevLpc.cpp)
的ICHVersion默认7，解释了ICH9机器仍显示27b9的现象。源码读取日期2026-10-19；未引入第三方驱动代码。
实测NIC 00:03.0/INTA：firmware line=11，但实际APIC GSI=19，不能把11直接作为APIC路由。

路由沿用现有active-low/level INTx，vector 0x42、投递到初始化BSP；ISR仍仅ack+notify。
RX worker使用原传输队列与锁，第一次实际收到IRQ通知帧时输出：

```text
e1000 INTx probe: VBox/ICH9 root-slot=3 pin=1 firmware_line=11 verified_gsi=19 LPC=27b98086
e1000 irq rx enabled: INTx line=11 gsi=19 ...
net irq rx active: if=enet0 hardware=1 notified-frame=1
```

未知平台、非root总线、未知LPC/VMMDev/model/pin、无IOAPIC或路由冲突均不强行开IRQ，
日志明确 `unverified-or-unavailable-route fallback=poll-10ms`。
boot参数 `capmix.e1000_irq=0` 强制保留原10ms轮询（设备IMC与PCI INTx保持屏蔽），用于回退及同内核A/B。
没有新增完整ACPI _PRT解释器、桥后递归swizzle、MSI/MSI-X或共享INTx fanout。

IRQ模式正常不再每10ms主动drain硬件RX，而由IRQ通知启动drain；既有100ms watchdog仍保留。
**worker的10ms定时唤醒/RTO推进仍保留**，不是宣称CPU完全无周期唤醒。
未确认或丢失通知时watchdog仍可收包；本轮未实现通用运行时IRQ线路重绑定。

## 可复现的同内核 A/B

`perf-baseline.py`新增：`--chipset ich9`（VBox）、`--e1000-irq auto|poll`、`--http-only`。
http-only不安装/启用Linux，hush吞吐明确标为跳过；原HTTP+hush默认模式仍显式安装Linux，不改变内核默认开关。
配置JSON记录kernel SHA-256、CPU、模式、日期和请求参数。
在VBox ICH9 auto测量中，必须看到路由成功和实际IRQ RX帧，否则基准失败，不能把fallback数字标成IRQ数字。

顺序运行，禁止同时启动VBox和KVM，目录须不存在：

```sh
python3 scripts/perf-baseline.py --backend vbox --chipset ich9 --http-only \
  --e1000-irq auto --cpus 1,4 --lab /tmp/capmix-intx-auto --port 18089
python3 scripts/perf-baseline.py --backend vbox --chipset ich9 --http-only \
  --e1000-irq poll --cpus 1,4 --lab /tmp/capmix-intx-poll --port 18089
python3 tests/check_intx_perf.py /tmp/capmix-intx-poll /tmp/capmix-intx-auto
```

两组均为同内核，只改变boot强制轮询参数：
`35582b843048e4b65987200759dc7bfa4f9c31aec9839efcc4d2f3cdd032fa2d`。
前/后表示轮询/IRQ机制对照，不是声称运行时间顺序；本轮先跑IRQ、再跑轮询，未做随机交替统计实验。
安装盘新建，无ISO运行；UEFI、ICH9、AHCI、82540EM NAT转发127.0.0.1:18089→guest:8080、512MiB、1/4 CPU，
VirtualBox 7.2.20。每CPU先预热8次，再3批×20次请求，串行新连接，HTTP/1.0状态/25字节正文/EOF逐次核对。
测量前固定等待6秒载波；测量中不重试、丢弃或跳过失败请求。

http_rps = 每批20 / 墙钟秒。延迟从宿主connect前到完整响应EOF及正文验证后，含连接与关闭；不是内核纯RX延迟。
每批p50为中位数，p95为排序后的nearest-rank（ceil(0.95*N)），表格再取三批相应统计量的中位数。
原始每请求延迟数组在results JSON，检验器复算分位数及速率。少量有界样本，不是长期生产吞吐保证。

| CPU | poll req/s | IRQ req/s | 差值 | poll→IRQ p50 ms | poll→IRQ p95 ms |
|---|---:|---:|---:|---:|---:|
| 1 | 7.301 | 8.068 | +10.51% | 134.068 → 123.768 | 146.743 → 128.814 |
| 4 | 7.517 | 8.537 | +13.56% | 133.043 → 116.942 | 136.082 → 120.229 |

[对照复算](../tests/evidence/logs/ich9-comparison.log)、[IRQ原始JSON](../tests/evidence/logs/ich9-irq-results.log)、
[poll原始JSON](../tests/evidence/logs/ich9-poll-results.log)、[IRQ数字](../tests/evidence/logs/ich9-irq-numbers.log)、
[poll数字](../tests/evidence/logs/ich9-poll-numbers.log)。不是把旧PIIX3/Linux开启的历史数字当本轮ICH9/Linux关闭的baseline。
脚本不要求“必须变快”，只强制配置/载荷/IRQ证据正确。

## 本轮验收与限制

| 环境（2026-10-19） | 实际结果 | 证据 |
|---|---|---|
| VBox ICH9 UEFI 1/4 CPU，Linux关，IRQ vs poll，无ISO盘 | 两组性能载荷/EOF/退出/回收断言PASS，IRQ均有实际硬件通知帧 | [IRQ检查](../tests/evidence/logs/ich9-irq-check.log)、[poll检查](../tests/evidence/logs/ich9-poll-check.log)、[IRQ四核串口](../tests/evidence/logs/ich9-irq-smp4.log)、[poll四核串口](../tests/evidence/logs/ich9-poll-smp4.log) |
| VBox ICH9 UEFI SMP4，Linux关，e1000 NAT，debug | 全量0 failures，硬件IRQ delta=4、1730B UDP排队验证、TCP/RTO/用户网络ELF通过 | [串口](../tests/evidence/logs/ich9-vbox-debug.log)、[全链检查](../tests/evidence/logs/ich9-vbox-debug-check.log) |
| hosted | 全量0 failures；硬件路径明确skip | [日志](../tests/evidence/logs/ich9-hosted.log) |
| QEMU KVM i440FX/e1000 SMP4，Linux关 | 全量0 failures，既有INTx与UDP1730B硬件delta通过 | [串口](../tests/evidence/logs/ich9-qemu-e1000.log)、[检查](../tests/evidence/logs/ich9-qemu-e1000-check.log) |
| QEMU KVM Q35/e1000 SMP4，Linux关 | 全量0 failures，未知路由明确poll-10ms，UDP/TCP实际收发通过 | [串口](../tests/evidence/logs/ich9-qemu-fallback.log)、[检查](../tests/evidence/logs/ich9-qemu-fallback-check.log) |
| QEMU KVM virtio-net SMP4，Linux+interp开 | 全量0 failures，原静态/动态/hush/文件/迁移黄金与原生网络回归 | [串口](../tests/evidence/logs/ich9-qemu-virtio.log)、[检查](../tests/evidence/logs/ich9-qemu-virtio-check.log) |

完整VBox debug访问宿主echo另须 `VBoxManage modifyvm VM --nat-localhostreachable1 on`，并启动
`python3 scripts/udp-echo.py`（9999）、`python3 scripts/tcp-echo.py`（9998）。
这与入站HTTP hostfwd不同：性能测试只用入站转发，不需要修改localhostreachable。

保留非成功尝试：

- 初版只识别29e0+2918时，VBox隐藏host bridge及默认27b9导致auto仍回退；
  [探测不足试跑](../tests/evidence/logs/ich9-preliminary-auto.log)明确没有IRQ，不计入本轮A/B。
- debug首轮未设置localhostreachable，ARP/ICMP/IRQ成功但宿主echo没有收到包，UDP/TCP/用户ELF合计3 failures；
  [失败串口](../tests/evidence/logs/ich9-vbox-first-failure.log)保留。补测试配置后重跑全量通过，未放宽断言或改RTO。

[内核构建](../tests/evidence/logs/ich9-kernel.log)无新增warning/error。

[源码读取哈希](../tests/evidence/logs/ich9-source-ids.log)、[旧格式基准检查器兼容](../tests/evidence/logs/ich9-legacy-check.log)。
[默认产品ISO](../tests/evidence/logs/ich9-product-iso.log)构建成功，
[默认启动串口](../tests/evidence/logs/ich9-product.log)到用户shell ready；空闲timeout=124，仅启动检查，非全量selftest。
