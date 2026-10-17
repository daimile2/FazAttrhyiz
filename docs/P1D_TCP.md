# P1d：最小主动 TCP

> 当前容量已续作至全局 32 槽、backlog 8、默认 RX 16KiB（编译可选 4–32KiB）；
> 本文历史参数不覆盖 [TCP_POOL](TCP_POOL.md)，RTO 语义保持不变。

> 本文记录当时的主动连接/RTO交付。后续已新增用户态listen/accept与HTTP服务端，
> 当前服务端ABI、边界及验证见 [HTTPD.md](HTTPD.md)。

本轮交付**内核 capability API + 真实网卡 selftest**，没有新增 TCP syscall、
用户 TCP ELF 或 BSD socket ABI。原 UDP syscall/ELF、DHCP、产品 shell 保留。
实现为本项目原创代码，报文/状态语义参考
[RFC 9293](https://www.rfc-editor.org/rfc/rfc9293.html)，不宣称完整 TCP 标准兼容。

## 接口与容量

接口见 `include/capmix/tcp.h`：

| API | 语义 |
|---|---|
| cm_tcp_open(task, &handle) | 创建连接对象、安装当前 task 的 typed PORT capability |
| cm_tcp_connect(task, handle, ifid, IPv4, port, timeout_ms) | 主动握手；成功返回 0 |
| cm_tcp_send(task, handle, data, length, timeout_ms) | 顺序分段，等待 ACK；返回实际已确认字节数 |
| cm_tcp_recv(task, handle, buffer, capacity) | 非阻塞字节流读取；无数据 AGAIN，peer FIN 后缓冲读尽为 0 |
| cm_tcp_close(task, handle, timeout_ms) | 主动 FIN/有限等待后消费该 handle；失败也清理 |

- 当前全局 32 个连接槽；每连接 RX 默认 16KiB（编译可选4–32KiB）；本地临时端口从 49152 起分配。
- 发送每段最大 **536B**，SYN 携带 MSS=536；对端更小 MSS 也遵守。
- send 单次最大 65536B，始终只允许一个未确认段。
- connect/send/close 超时参数为 1–10000ms；等待时 yield，测试使用 3000ms。
- 只支持已 up 实网卡的 IPv4 主动连接，不支持 TCP lo 或 listen/accept。
- API 的数据指针是可信内核指针，不能直接作为不可信用户 syscall 暴露。
- control/send/close 检查 WRITE，recv 检查 READ。UDP/TCP 虽共用 PORT 类型，
  析构函数身份进一步校验协议类型，不能用 UDP handle 操作 TCP。
- capability 可 DUPLICATE，未授予 TRANSFER。别名共享连接状态；close 关闭
  整个连接但只释放调用者 handle，其余别名仍需释放。busy 时 close 返回
  BUSY 并保留 handle，不并发抢占正在进行的 writer。
- 同一连接一次只允许一个 connect/send/close writer；recv 可读取已入队数据。
  task exit/final handle release best-effort RST，并回收槽。协议失败只影响
  该连接，不关闭 NIC、UDP endpoint 或 DHCP 配置。

send 部分成功后发生错误时返回已确认长度，连接转 ERROR；这些字节可能
已经到达对端，不能假设可以无条件重发整个应用请求。连接出错后，recv 可先读出已缓存
数据；若此前未收到 peer FIN，读尽后返回 DEAD。尚未连接的 recv 返回 AGAIN。已在 ERROR 的 close
仅执行清理，不假装完成了 FIN 握手。

## 状态子集

| 状态 | 本轮行为 |
|---|---|
| CLOSED | 新建/已关闭 |
| SYN_SENT | SYN 已发，接受匹配 SYN+ACK；正确 RST 或超时失败 |
| ESTABLISHED | 数据、ACK、peer FIN |
| FIN_WAIT_1 | 本端 FIN 等 ACK/peer FIN |
| FIN_WAIT_2 | FIN 已确认，等 peer FIN |
| CLOSING | 同时关闭：peer FIN 已到，等本端 FIN 的 ACK |
| TIME_WAIT | 最后 ACK 后仅等待 **100ms**，处理重复 FIN；不是完整 2MSL |
| CLOSE_WAIT | peer 已 FIN，本端仍可发送，close 发 FIN |
| LAST_ACK | 等待本端 FIN 的 ACK |
| ERROR | RST/超时/发送错误；等待 capability 清理 |

输入校验 IPv4/TCP 长度、无分片、checksum、四元组、TCP header/options 边界，
经过 XDP→nft。未来 ACK 和不匹配 RST 拒绝；重复/重叠数据去重，仅接受
连续序号，不存储乱序段。接收缓冲不足时不推进 RCV.NXT，发送当前 ACK/
可用窗口。只有准确的 RCV.NXT 才接受已连接 RST。

`net/runtime.c` 只加 TCP 分流分支；后台 RX 的线程/调度循环不变。
TCP 自有 IRQ-save 锁保护池、序号、缓冲，cap acquire 保持对象引用，
packet 引用保护槽在 RX 处理期间不被复用。发送、日志和 yield 均在锁外。
DHCP 核心、buddy 锁、时钟、shell/FB 和 AHCI/COW 没有修改。

## 明确缺失

- 已补 SYN/数据/FIN 待确认段、固定 RTO 与指数退避，见 [TCP_RTO.md](TCP_RTO.md)。
  仍无 RTT 自适应估计和 fast retransmit；ARP 重试单独处理。
- 没有拥塞控制、cwnd/ssthresh、慢启动；stop-and-wait 不是拥塞控制实现。
- 只使用未缩放的 peer window，最大接收窗口 4 KiB；没有 window scaling、
  完整 WL1/WL2 更新规则、persist/zero-window probe。零窗口有限等待后失败。
- 无乱序队列、SACK、Nagle、timestamps、ECN、URG、keepalive、IPv6、PMTU。
  不支持的 flags/IPv4 options/fragment 拒绝。未知 TCP options 仅边界校验后跳过，
  不表示对应扩展被启用。
- 无主动安全 ISN 生成器（当前 TSC 取值），无 SYN cookies、完整挑战 ACK/
  RST 防护、端口复用的完整 TIME_WAIT 隔离。
- 纯 ACK 失败的重试依赖后续输入/API 活动；待确认段 RTO 由后台 RX/API 等待推进。
- 无被动监听、BSD socket、HTTP/TLS；后续已提供 TCP 用户 syscall/ELF，原 ABI 沿用。

这是受控 QEMU 网络的机制验证与原型，不能作为可靠互联网 TCP 栈发布。

## 验收

宿主启动两个服务：

```
python3 scripts/tcp-echo.py  # 127.0.0.1:9998
python3 scripts/udp-echo.py  # 127.0.0.1:9999
```

另一个终端：

```
make kernel 2>&1 | grep -iE 'warn|error'
make iso LIMINE_CONFIG=boot/limine-tcp-selftest.conf
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso \
  -netdev user,id=n0,dhcpstart=10.0.2.42 -device e1000,netdev=n0 \
  -serial stdio -display none -no-reboot 2>&1 | tee /tmp/capmix_run.log
```

替换 device 为 virtio-net-pci 可测另一种 NIC。该配置包含
`capmix.udp_echo=1 capmix.tcp_echo=1`，因此 DHCP 拿到 lease 后，先做 UDP
回归，再做 TCP。显式打开 echo 测试而服务缺失会有限超时并 FAIL。
普通 debug 不依赖宿主 TCP 服务，运行纯状态/校验测试并注明 echo skipped。
无网卡 skip，保持全量 PASS。默认 make iso 仍进产品 shell。

TCP 测试：建立连接；确认只读 cap 发送被拒；发送 2049B（多段、奇数尾段）；
以每次最多 73B 读回并逐字节比较；FIN 关闭；连接未监听的 9997 端口验证
RST 或超时失败、live=0；之后再次验证 UDP lo/NIC payload，并检查 DHCP
地址/掩码/网关不变。测试不在客户端轮询硬件 RX。

日志关键词：`tcp syn`、`tcp established`、`tcp payload verified`、
`tcp close ... fin=complete`、`tcp failed-connect cleanup verified`、
`tcp failure isolation verified`、`p1d selftest: PASS`。

已执行记录：

- `/tmp/p1d-hosted.log`：机制测试，全量 PASS；无硬件 TCP 测试。
- `/tmp/p1d-e1000.log`、`/tmp/p1d-vnet.log`：4 CPU、lease 地址、真实 TCP
  2049B payload 与 FIN，全量 PASS。
- `/tmp/p1d-dual.log`：两种 NIC + 双 NUMA，TCP/UDP/DHCP 和失败隔离，全量 PASS。
- `/tmp/p1d-nonic.log`：无卡 skip，全量 PASS。

本轮修改文件：`net/tcp.c`、`include/capmix/tcp.h`、`net/runtime.c`、
`net/filter.c`、`proc/syscall.c`（仅 exit 清理 hook）、`boot/p1d_test.c`、
`boot/kernel.c`、`boot/limine-tcp-selftest.conf`、`scripts/tcp-echo.py`、Makefile
及本说明/README/总览。UDP/DHCP 模块本身保持不变。
