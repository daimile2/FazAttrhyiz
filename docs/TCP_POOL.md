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

## 环境矩阵

2026-10-17，Ubuntu 宿主，KVM SMP4 / VBox 7.2.20。以下为实际结果。

| 环境 | 结果 | 日志 |
| --- | --- | --- |
| hosted 默认16KiB，Linux关/开且interp关/双开 | 3份全量 `PASS (0 failures)`，池/RX/L3 fd与backend边界通过；硬件ELF跳过 | `/tmp/tcp32-hosted-{off,static,on}.log` |
| hosted RX=4096/32768 | 两份全量 `PASS (0 failures)` | `/tmp/tcp32-hosted-rx4096.log`、`/tmp/tcp32-hosted-rx32768.log` |
| KVM默认RX，Linux/interp双开，e1000 | 全量 `PASS (0 failures)`，multi8/payload/清理通过 | `/tmp/tcp32-on-e1000-matrix.log` |
| KVM默认RX，双NUMA，virtio-net+e1000 | 全量 `PASS (0 failures)`，两块网卡各自multi8通过 | `/tmp/tcp32-on-dual-vnet-matrix.log` |
| KVM默认RX，双开，无网卡 | 全量 `PASS (0 failures)`，机制通过，wire明确skip | `/tmp/tcp32-on-no-nic-matrix.log` |
| KVM默认RX，Linux开/interp关 | 全量 `PASS (0 failures)`，静态Linux与原生、multi8通过 | `/tmp/tcp32-interp-off-matrix.log` |
| KVM默认RX，Linux运行时关 | 全量 `PASS (0 failures)` | `/tmp/tcp32-linux-off-matrix.log` |
| KVM默认RX，编译Linux关，双节点/两网卡 | 全量 `PASS (0 failures)` | `/tmp/tcp32-compiled-off-matrix.log` |
| KVM RX=4096，e1000 | 全量 `PASS (0 failures)`，multi8×8193B通过 | `/tmp/tcp32-rx4096-matrix.log` |
| KVM RX=32768，双节点/两网卡 | 全量 `PASS (0 failures)`，两网卡multi8通过 | `/tmp/tcp32-rx32768-matrix.log` |
| 无ISO产品盘，virtio-blk+e1000 | Linux-net echo、native nc/http-get/httpd两次200、文件重定向、动态/TLS/hush、关机通过 | `/tmp/tcp32-final-product-first.log`、`/tmp/tcp32-final-httpd-client.log` |
| 默认产品ISO，Linux关闭 | pool=32/RX=16384，用户shell、原生hello、ACPI关机通过 | `/tmp/tcp32-default-product.log` |
| 同盘无ISO冷启动 | 动态/TLS、hush、持久文件读回及关机通过 | `/tmp/tcp32-final-product-second.log` |
| VBox UEFI/AHCI/SMP4，两次产品启动 | 池初始化32/16KiB，动态/TLS/L0/hush、原生持久文件读回、关机通过；NIC显示down，未宣称wire通过 | `/tmp/tcp32-final-vbox-first.log`、`/tmp/tcp32-final-vbox-second.log` |
| VBox SMP4，debug，**禁用网卡**，Linux关 | 全量 `PASS (0 failures)`；TCP池/RX机制通过，wire跳过；宿主checker通过 | `/tmp/tcp32-final-vbox-debug-nonic.log` |
| VBox SMP4，debug，NAT e1000 | **FAIL (4 failures)**；M8 e1000收发=-9，随后P1b UDP、P1d echo、TCP ELF失败；本轮池/RX机制通过 | `/tmp/tcp32-vbox-debug.log` |
| 非法 RX=4095/32769 | 编译期边界断言按预期拒绝 | `/tmp/tcp32-invalid-rx4095.log`、`/tmp/tcp32-invalid-rx32769.log` |

8份 QEMU 与5份 hosted日志均经 `tests/check_tcp_pool.py` 递归检查既有 Linux/动态/TLS/W^X
及全量结果，启用wire时强制检查8连接payload。QEMU每趟为512MiB、4CPU、30秒上限；
双节点使用两个显式256MiB memory-backend。phase0–9、NUMA/TRACE/pressure/COW随全量通过。
实际RTO丢SYN/data/FIN恢复日志仍在，且源函数体比较确认retain/acknowledge/due/submitted/
service/advance/connect/send/recv/close未改。

VBox结果按场景区分，不能把禁网卡PASS写成VBox网络PASS，也未声称已修复e1000问题。
专用VM为 `CapMix-TCP32-20261017`，磁盘均为本轮 `/tmp/tcp32-*` 测试文件，未修改旧VM/用户盘。
带网卡debug的失败是实际保留记录，不在本轮扩大范围修网卡或修改RTO。

## 修改文件

`include/capmix/tcp.h`、`net/tcp.c`、`Makefile`；
`boot/p1d_test.c`、`boot/linux_l3_test.c`、`tests/check_tcp_pool.py`；
本文及总览、HTTPD、USER_NETWORK_ABI、P1D_TCP、TCP_RTO 与文档导航的当前容量说明。
