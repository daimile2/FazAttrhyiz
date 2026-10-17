# TCP：32 槽连接池与有界接收窗口

本轮只扩连接容量和接收缓冲，保持既有 TCP 状态机、connect/listen/accept ABI、
单待确认段、MSS、RTO 和 close/kill 所有权规则。与 Linux persona 开关正交。
不是无限连接、拥塞控制或高吞吐 TCP 产品化。

## 固定容量与资源含义

| 项目 | 当前值 |
| --- | --- |
| 全局 TCP 槽 `CM_TCP_CONNECTIONS` | **32**，原为 8 |
| 单监听器 backlog 最大值 | **8**，原为 4 |
| 每槽 RX 默认容量 | **16384 字节（16KiB）**，原为 4096 |
| 可配置 RX 范围 | 编译期 **4096–32768 字节**，越界编译拒绝 |
| 本地通告窗口 | 剩余 RX 容量，最大不超过 32768；不使用 window scale |
| MSS/每连接待确认数据副本 | 仍最多 **536 字节** |
| RTO | 仍初始 200ms，退避至 400/800/1600ms，1600ms 封顶，最多 4 次重传 |

32 是整个内核的共享上限，不是每进程上限，也不是“32 条 established 外加监听器”。
一个已 open 但未 connect 的 TCP 对象、监听器、SYN_RECEIVED、已建立但未 accept 的排队子连接、
已 accept 的连接都占槽。cap 别名共享同一个槽；后台 TX/RTO 的短暂 pin 可延迟最后释放。
多个监听器的 backlog 之和也不能越过全局池。backlog 约束的是未 accept 队列，
accept 后该连接继续占全局槽，但从监听器队列摘除。

原生 task 的句柄表仍为 256；Linux 每进程 fd 表仍为 32（包含标准 I/O、文件、UDP 等），
没有为了填满全局池扩大 Linux ABI。仅有 stdio 时最多还能用 29 个 fd，其他资源限制可更早出现。
Linux L3 自测保留一个可用 fd，并用直接 capability 补足全局槽，分别验证 ENOBUFS 与 EMFILE。

## 内存与 no-overcommit

继续使用静态 `connections[32]`，RX 与 pending 内嵌在每个槽中，启动时已真实驻留。
不在 IRQ、入站 SYN 或 open 时临时申请 RX 页，不延迟兑现接收内存，也不从用户指针保留重传数据。
这是内核镜像/BSS 预留，freestanding 可用物理内存统计排除该区域，不重复计入匿名 commit。
日志 `static-resident commit=0` 指该池新增的**匿名 commit 收费为零**，不是 RAM 开销为零，
也不是系统全局 committed=0。

x86-64 当前结构布局，槽元数据与 pending 合计 688B；实际 `sizeof` 在初始化日志打印：

| RX 配置 | 每槽字节数 | 32 槽总字节数 |
| --- | --- | --- |
| 4096 | 4784 | 153088（149.5KiB） |
| **16384 默认** | **17072** | **546304（533.5KiB）** |
| 32768 | 33456 | 1070592（1045.5KiB） |

原 8 槽/4KiB 池为 38272B，默认池增加 508032B（约 496KiB）。
此外已有两个机制测试 fixture 随 RX 容量增长，新增 8 路 wire 测试包含固定发送缓冲；
freestanding 默认测试 fixture 总量约 98KiB，未包含在池的 `pool_bytes` 中。
内核当前把这些自测源码也编入产品，因此不能把 `pool_bytes` 当作整个 TCP 模块/BSS 增量。
Linux socket wrapper、线程栈、cap/object/fd 表等另有既有物理分配与限额；本轮不重做其记账。

```text
tcp pool: slots=32 backlog=8 rx_bytes=16384 slot_bytes=17072 pool_bytes=546304 static-resident commit=0
```

配置方法（无运行时 socket option/自动扩张）：

```sh
make kernel TCP_RX_BYTES=32768
make iso TCP_RX_BYTES=32768
make test TCP_RX_BYTES=4096
# 恢复默认：
make kernel
make iso
```

Makefile 配置 stamp 确保切换 RX 参数会重编 hosted 与 freestanding；所有部分必须使用同一值。
默认选 16KiB，不默认扩大到 32KiB。4–32KiB 内允许整数容量，环形索引使用取模，不要求 2 次幂。

## 满池、窗口与失败语义

- native `cm_tcp_open` 在没有槽时返回 `CM_ERR_NOSPC=-6`，失败不发布新 handle。
  cap/object/句柄资源不足也沿用有界失败，不靠覆盖活连接腾位。
- 入站 SYN 若 backlog 或全局池满，返回 NOSPC、打印 `tcp listen refused`，尝试 best-effort RST；
  不能假称对端一定收到 RST。
- Linux socket 映射 backend NOSPC 为 `-ENOBUFS=-105`，fd 表满为 `-EMFILE=-24`，
  wrapper 物理页分配失败为 `-ENOMEM=-12`。没有修改错误映射。
- RX 装不下一个有效入序段的剩余 payload 时，不覆盖旧数据、不推进 RCV.NXT，
  置 ACK pending 并沿用既有拒绝/通告窗口行为。没有新增乱序队列或接收端重传算法。
- recv 读出后减少占用并触发窗口更新 ACK；无数据仍非阻塞 AGAIN，正常 FIN 读尽仍 EOF=0。
  仍无 window scaling、SACK、完整 zero-window persist 或拥塞控制。
- 大 RX 窗口不等于提高单次用户 syscall 的 I/O 上限：原生 TCP SEND/RECV 仍最多 4096B；
  Linux TCP 路径每次 bounce 最多一页，可能短读/短写。更大应用数据继续循环处理。
- close、最后 cap 引用、队列 TTL 和进程退出仍使用原所有权/RTO 清理路径。
  后台 pin 尚在时等待其释放，不能强行复用其访问的槽。

RTO 的 `retain/acknowledge/due/submitted/service` 算法、重试次数、操作总期限及主动连接语义未改。
扫描池从 8 增至 32，有更多固定扫描/锁工作；没有引入多队列、批量 ACK 或每连接并行 writer。

## 新增测试

1. 机制测试创建 32 个真实 cap，检查槽地址不重复；第 33 个 open、满池入站 queue 均 NOSPC。
   留下 RX canary 后关闭一槽并重新 open，验证复用及 head/count/缓冲清零；全部 teardown 回到基线。
2. 同时排队 8 个被动握手，下一次入队失败；完成 ACK+独立 payload，accept/recv 逐连接校验。
   再排一个 half-open，process_exit 同时回收监听器、已 accept 和未 accept 连接。
3. 已 accept 的 RX 环从非零 head 填满整个配置容量，超出一字节拒绝且序号不变，
   小块读取校验全部 payload/环回，再接收新字节，验证可用容量从 0 恢复。
   此机制测试使用虚拟无路由接口，不把它宣传成抓包证明所有窗口 ACK 已发上网。
4. freestanding wire 测试：**8 条主动连接同时完成握手并保留到 payload 校验结束**，
   各发 8193B、333B 小块读回、按连接不同的 pattern 校验并关闭，检查槽/页/commit 回收。
   对每个可用 e1000/virtio-net 分别执行；无网卡或未配置 echo 则明确跳过 wire，机制测试仍运行。
5. 原有被动生命周期、SYN/data/FIN 丢包恢复、UDP/DHCP、用户 TCP、Linux-net 与产品 httpd/nc 回归。

wire 测试没有扩大线程栈：大发送缓冲固定存放测试上下文，线程栈仅放小接收块。
测试 worker 有限期限；若不能退出，报告 FAIL 并保留仍活动的上下文，不强行回收正在运行的栈。

```sh
python3 scripts/tcp-echo.py   # 宿主 9998
python3 scripts/udp-echo.py   # 宿主 9999
# Linux wire 回归还需 HTTP 8000，/hello.txt 为 linux L3 host body\n
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
# 执行既有 QEMU SMP4 串口命令并保存日志后：
python3 tests/check_tcp_pool.py --wire /tmp/capmix_run.log
# 可选 --rx 4096 / --rx 32768；--hosted / --disabled / --interp-off 适配其他矩阵。
```

关键日志：`tcp pool verified`、`full=NOSPC`、`reuse=ok`、`tcp RX verified`、
`tcp multi verified: ... simultaneous=8 ... bytes_each=8193`、`pages/commit unchanged`。
最终仍以全量 `selftest result: PASS (0 failures)` 为准。

## 环境矩阵（连接池扩容原轮次）

2026-10-17，Ubuntu 宿主，KVM SMP4 / VBox 7.2.20。以下为实际结果。

| 环境 | 结果 | 日志 |
| --- | --- | --- |
| hosted 默认16KiB，Linux关/开且interp关/双开 | 3份全量 `PASS (0 failures)`，池/RX/L3 fd与backend边界通过；硬件ELF跳过 | [tcp32-hosted-off.log](../tests/evidence/logs/tcp32-hosted-off.log)、[tcp32-hosted-static.log](../tests/evidence/logs/tcp32-hosted-static.log)、[tcp32-hosted-on.log](../tests/evidence/logs/tcp32-hosted-on.log) |
| hosted RX=4096/32768 | 两份全量 `PASS (0 failures)` | [tcp32-hosted-rx4096.log](../tests/evidence/logs/tcp32-hosted-rx4096.log)、[tcp32-hosted-rx32768.log](../tests/evidence/logs/tcp32-hosted-rx32768.log) |
| KVM默认RX，Linux/interp双开，e1000 | 全量 `PASS (0 failures)`，multi8/payload/清理通过 | [tcp32-on-e1000-matrix.log](../tests/evidence/logs/tcp32-on-e1000-matrix.log) |
| KVM默认RX，双NUMA，virtio-net+e1000 | 全量 `PASS (0 failures)`，两块网卡各自multi8通过 | [tcp32-on-dual-vnet-matrix.log](../tests/evidence/logs/tcp32-on-dual-vnet-matrix.log) |
| KVM默认RX，双开，无网卡 | 全量 `PASS (0 failures)`，机制通过，wire明确skip | [tcp32-on-no-nic-matrix.log](../tests/evidence/logs/tcp32-on-no-nic-matrix.log) |
| KVM默认RX，Linux开/interp关 | 全量 `PASS (0 failures)`，静态Linux与原生、multi8通过 | [tcp32-interp-off-matrix.log](../tests/evidence/logs/tcp32-interp-off-matrix.log) |
| KVM默认RX，Linux运行时关 | 全量 `PASS (0 failures)` | [tcp32-linux-off-matrix.log](../tests/evidence/logs/tcp32-linux-off-matrix.log) |
| KVM默认RX，编译Linux关，双节点/两网卡 | 全量 `PASS (0 failures)` | [tcp32-compiled-off-matrix.log](../tests/evidence/logs/tcp32-compiled-off-matrix.log) |
| KVM RX=4096，e1000 | 全量 `PASS (0 failures)`，multi8×8193B通过 | [tcp32-rx4096-matrix.log](../tests/evidence/logs/tcp32-rx4096-matrix.log) |
| KVM RX=32768，双节点/两网卡 | 全量 `PASS (0 failures)`，两网卡multi8通过 | [tcp32-rx32768-matrix.log](../tests/evidence/logs/tcp32-rx32768-matrix.log) |
| 无ISO产品盘，virtio-blk+e1000 | Linux-net echo、native nc/http-get/httpd两次200、文件重定向、动态/TLS/hush、关机通过 | [tcp32-final-product-first.log](../tests/evidence/logs/tcp32-final-product-first.log)、[tcp32-final-httpd-client.log](../tests/evidence/logs/tcp32-final-httpd-client.log) |
| 默认产品ISO，Linux关闭 | pool=32/RX=16384，用户shell、原生hello、ACPI关机通过 | [tcp32-default-product.log](../tests/evidence/logs/tcp32-default-product.log) |
| 同盘无ISO冷启动 | 动态/TLS、hush、持久文件读回及关机通过 | [tcp32-final-product-second.log](../tests/evidence/logs/tcp32-final-product-second.log) |
| VBox UEFI/AHCI/SMP4，两次产品启动 | 池初始化32/16KiB，动态/TLS/L0/hush、原生持久文件读回、关机通过；NIC显示down，未宣称wire通过 | [tcp32-final-vbox-first.log](../tests/evidence/logs/tcp32-final-vbox-first.log)、[tcp32-final-vbox-second.log](../tests/evidence/logs/tcp32-final-vbox-second.log) |
| VBox SMP4，debug，**禁用网卡**，Linux关 | 全量 `PASS (0 failures)`；TCP池/RX机制通过，wire跳过；宿主checker通过 | [tcp32-final-vbox-debug-nonic.log](../tests/evidence/logs/tcp32-final-vbox-debug-nonic.log) |
| VBox SMP4，debug，NAT e1000 | **FAIL (4 failures)**；M8 e1000收发=-9，随后P1b UDP、P1d echo、TCP ELF失败；本轮池/RX机制通过 | [tcp32-vbox-debug.log](../tests/evidence/logs/tcp32-vbox-debug.log) |
| 非法 RX=4095/32769 | 编译期边界断言按预期拒绝 | [tcp32-invalid-rx4095.log](../tests/evidence/logs/tcp32-invalid-rx4095.log)、[tcp32-invalid-rx32769.log](../tests/evidence/logs/tcp32-invalid-rx32769.log) |

8份 QEMU 与5份 hosted日志均经 `tests/check_tcp_pool.py` 递归检查既有 Linux/动态/TLS/W^X
及全量结果，启用wire时强制检查8连接payload。QEMU每趟为512MiB、4CPU、30秒上限；
双节点使用两个显式256MiB memory-backend。phase0–9、NUMA/TRACE/pressure/COW随全量通过。
实际RTO丢SYN/data/FIN恢复日志仍在，且源函数体比较确认retain/acknowledge/due/submitted/
service/advance/connect/send/recv/close未改。

原轮次VBox结果按场景区分：禁网卡PASS不是网络PASS，当时尚未修复e1000问题；本轮结果见下方续作。
历史测试使用独立VM与临时盘，未修改旧VM/用户盘。
带网卡debug的失败作为历史记录保留；下方续作给出原4项关闭证据，没有修改RTO算法。

## 修改文件

`include/capmix/tcp.h`、`net/tcp.c`、`Makefile`；
`boot/p1d_test.c`、`boot/linux_l3_test.c`、`tests/check_tcp_pool.py`；
本文及总览、HTTPD、USER_NETWORK_ABI、P1D_TCP、TCP_RTO 与文档导航的当前容量说明。

## VBox e1000 debug：原4项失败对账与关闭

2026-10-17续作。旧失败保留在[原始轮次日志](../tests/evidence/logs/tcp32-vbox-debug.log)，
[独立VM复现](../tests/evidence/logs/e1000fix-vbox-baseline.log)仍为`FAIL (4 failures)`。
这些记录不改写为PASS；下文最终日志对应修复后的构建。

| 原失败断言 | 根因与处理 | 关闭证据 |
| --- | --- | --- |
| M8 `tx/rx failed: -9` | `cm_net_probe()`看见`iface->up=0`即返回DEAD；原测试把设备存在当成链路就绪。M8现在等待并重读STATUS.LU，最多6000ms | 最终VBox `e1000 link ready`、9轮probe跨16项环、`tx_delta=18 rx_delta=18` |
| P1b open/bind断言 | UDP bind明确拒绝down接口，失败在开始收包之前。按接口up筛选，down明确SKIP | `irq rx verified: enet0`、UDP `bytes=1730`、用户UDP `bytes=1/257/1472` |
| P1d echo `result=-2 bytes=0` | connect参数检查拒绝down接口，未进入SYN握手；不是RTO或连接池耗尽。down明确跳过wire，始终执行池/RX/RTO机制 | `tcp payload verified`、`simultaneous=8 clients=8 bytes_each=8193` |
| TCP ELF `report==(nic?1u:2u)` | 原primary selector返回存在但down的enet0，测试预期联网；PRIMARY connect却找不到up接口，ELF报告无NIC。primary与syscall统一选择up接口，virtio-net优先；PRIMARY无up接口为NOENT，显式选择down接口仍DEAD；PRIMARY无up接口为NOENT，显式选择down接口仍DEAD | `tcp ELF syscall verified: CPL=3 send/recv=2049`；拔线时`connect=NOENT skip exit=0 rights denied` |

[VBox设备配置证据](../tests/evidence/logs/e1000fix-vbox-device.log)记录
`LinkUpDelay=5000ms`；硬件STATUS在等待后变为up，本机约等3秒（不是从VM上电起计时）。
[VirtualBox官方设备源码](https://github.com/VirtualBox/virtualbox/blob/main/src/VBox/Devices/Network/DevE1000.cpp)
也定义`LinkUpDelay`默认5000ms。源码链接用于解释机制，实际本机值以归档设备日志为准。
没有通过关闭VBox延迟、强制写up或放宽payload断言来消除失败。

`cm_e1000_wait_link()`仅在M8启动验收中调用，不给产品启动强加6秒延迟；
每次读STATUS时短暂持设备锁，调度/等待时释放锁并恢复原中断状态，使用既有标定TSC。
超时返回AGAIN；无可用标定时返回NOTSUP并明确说明限制；设备死亡/内部错误仍FAIL。
注册接口丢失仍FAIL。链路已经up后发生ARP/ICMP/UDP/TCP、IRQ或payload错误仍FAIL，
不把任意网络错误转成skip。未修改TCP RTO算法、KPTI、buddy锁或定时器标定。

### 降级范围，不能写成网络PASS

[拔线负例](../tests/evidence/logs/e1000fix-vbox-down-final.log)使用同一ISO、NAT e1000、
`--cableconnected1 off`。6秒后日志逐项包含：

- `m8 e1000 wire SKIP: ... no tx/rx claim`
- `p1b UDP/bg RX wire SKIP`及`p1b UDP user ELF SKIP`
- `p1d TCP echo/multi wire SKIP`，池/RX/RTO机制照常执行
- `tcp ELF wire SKIP`，继续验证无可用NIC错误码、CPL3正常退出与页/槽回收

该场景全量`PASS (0 failures)`仅指实际执行的机制断言通过，wire覆盖明确缺失，
不代表拔线可联网。自定义更长的link-up延迟、后端不可用等导致6秒内无carrier时也按此限制报告；
本轮不承诺完整热插拔/链路恢复策略。

### 最终环境矩阵

Linux产品默认仍关；本轮主验收ISO显式`capmix.linux=0`。无盘selftest不需格式化用户盘。
以下wire测试启动宿主`python3 scripts/tcp-echo.py`和`python3 scripts/udp-echo.py`。
产品http-get另用127.0.0.1:8000提供`/hello.txt`，httpd用宿主18083转发guest8080。

| 环境 | 结果与范围 | 仓库证据 |
| --- | --- | --- |
| hosted Linux关，默认16KiB | 全量0 failures；硬件路径按原规则skip | [日志](../tests/evidence/logs/e1000fix-hosted-off-final.log) |
| hosted selector机制 | 两块up优先virtio；virtio down回退e1000；全down/缺设备返回NULL；状态up后可重选。仅selector，不是物理热插拔测试 | [断言](../tests/evidence/logs/e1000fix-primary.log) |
| VBox7.2.20r175154，UEFI/PIIX3/SMP4/512MiB/NAT 82540EM，Linux关 | **全量PASS (0 failures)**，原4项全过；IRQ RX、8连接×8193B及回收通过 | [最终日志](../tests/evidence/logs/e1000fix-vbox-off-final.log)、[断言](../tests/evidence/logs/e1000fix-check-vbox-off-final.log) |
| VBox同配置，拔线 | 全量0 failures；wire明确SKIP，池/lo/无NIC ELF通过；**不计网络PASS** | [最终日志](../tests/evidence/logs/e1000fix-vbox-down-final.log)、[断言](../tests/evidence/logs/e1000fix-check-vbox-down-final.log) |
| QEMU8.2.2/KVM，SMP4/512MiB/e1000，Linux关 | 全量0 failures，原4项、multi8及RTO丢包恢复通过 | [日志](../tests/evidence/logs/e1000fix-qemu-e1000-final.log)、[断言](../tests/evidence/logs/e1000fix-check-qemu-e1000-final.log) |
| QEMU双NUMA2×256MiB，virtio-net+e1000，Linux关 | 全量0 failures，两网卡各自multi8/UDP；virtio IRQ，e1000按既有路由策略poll fallback；virtio优先 | [日志](../tests/evidence/logs/e1000fix-qemu-dual-final.log)、[断言](../tests/evidence/logs/e1000fix-check-qemu-dual-final.log) |
| QEMU标准串口验收，无支持网卡，Linux关 | 全量0 failures，wire按原规则skip | [日志](../tests/evidence/logs/e1000fix-qemu-default-debug.log) |
| QEMU无ISO安装盘，virtio-blk+e1000/SMP4，Linux关 | 原生hello、`> >> <`持久文件、nc echo、http-get 200、httpd两次200、kill137/reap及关机通过；冷启动读回一致 | [首次](../tests/evidence/logs/e1000fix-final-product-first.log)、[冷启动](../tests/evidence/logs/e1000fix-final-product-second.log)、[客户端](../tests/evidence/logs/e1000fix-final-product-httpd-client.log)、[宿主断言](../tests/evidence/logs/e1000fix-final-product-assertions.log) |

产品测试不是全量selftest，不能给它补写`selftest result`标志。双卡QEMU的e1000日志明确`irq rx skipped ... fallback=poll-10ms`；其单卡IRQ已另验通过，不把双卡轮询写成IRQ通过。
VM均独立创建，未修改用户旧VM/盘。
历史矩阵中VBox NIC-down产品和禁网卡debug记录保持原范围；新增成功只适用于本表配置。

### 复现与检查

```sh
make kernel 2>&1 | grep -iE 'warn|error'
make test
cc -std=c11 -Wall -Wextra -Werror -DCAPMIX_HOSTED -Iinclude \
  net/if.c tests/net_primary.c -o /tmp/net-primary-test
/tmp/net-primary-test
make iso LIMINE_CONFIG=boot/limine-tcp-pool-selftest.conf
# 在独立VBox UEFI/SMP4/512MiB/NAT 82540EM VM挂此ISO，网线on，UART保存到新日志。
# 保留默认LinkUpDelay，启动宿主TCP/UDP echo；QEMU不要与VBox同时运行。
# 完整关机后将网线off，再启动一次生成拔线负例。
python3 tests/check_e1000_debug.py /tmp/vbox-up.log
python3 tests/check_tcp_pool.py --disabled --wire /tmp/vbox-up.log
python3 tests/check_e1000_debug.py --carrier-down /tmp/vbox-down.log
python3 tests/check_tcp_pool.py --disabled /tmp/vbox-down.log
```

两组checker分别要求真实wire与明确skip；不能把down日志交给wire checker获取PASS。
归档旧4失败日志也必须被checker拒绝。
无ISO产品回归可在**新建的可丢弃盘**运行（需要前述echo和HTTP测试端）：

```sh
python3 scripts/install-disk.py /tmp/capmix-net-regression.img --kernel capmix.elf
python3 tests/check_tcp_pool_product.py --disk /tmp/capmix-net-regression.img
```

脚本经QEMU monitor发送PS/2按键，两次冷启动，实际断言响应、持久文件与进程回收。
它会在测试根写`/irq-net*.txt`，不要对用户盘执行。

普通`make iso`依然使用默认产品配置；本轮没有改默认persona、安装布局或shell命令。

修改文件：`net/e1000.c`、`net/if.c`、`include/capmix/{e1000,net}.h`、
`proc/syscall.c`（只统一PRIMARY选择）、`boot/{m8,p1b,p1d,tcp_elf}_test.c`、
`boot/limine-tcp-pool-selftest.conf`、`tests/{net_primary.c,check_e1000_debug.py,check_tcp_pool_product.py}`、
本文与`tests/evidence/`。
