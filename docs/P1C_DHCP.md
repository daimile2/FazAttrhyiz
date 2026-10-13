# P1c：最小 DHCP 客户端

## 当前语义

默认在已 up 的 vnet0/enet0 上尝试 DHCP，产品在进入 shell 前完成配置；
多网卡分别协商，主网卡优先级仍为 virtio-net > e1000。没有网卡或网卡
未 up 时明确 skipped。协商全过程保留原静态地址，DISCOVER 用 0.0.0.0
发送广播，而不是提前将接口地址清零。有效 ACK 才提交 lease 地址、掩码、
网关，并清除该接口旧 ARP 缓存；超时、NAK、发送失败均保持原配置。

状态机是 SELECTING（DISCOVER）→ OFFERED → REQUESTING（REQUEST）→
BOUND（ACK），NAK → REJECTED。单接口总等待默认最多 3000ms，分阶段
最多发送三次，重试间隔 1 秒；这是启动时有界子集，不是完整 RFC 的随机
指数退避策略。等待时让出调度，RX 沿用 P1b 独立后台线程。

日志示例：

```
dhcp enet0 discover: UDP 68->67 broadcast timeout_ms=3000
dhcp enet0 offer/request: offered=0xa00022a server=0xa000202 attempt=1
dhcp enet0 ack: ip=10.0.2.42 mask=0xffffff00 gateway=0xa000202 server=0xa000202 lease=86400 seconds renew=unimplemented expire=not-scheduled
dhcp enet0 fallback: error=-5 ip=10.0.2.15 ... (static unchanged)
```

lease 单位为秒，仅记录并打印；**没有续租、到期定时器或自动停止使用过期
地址**。到期前需重新启动/由后续网络管理实现重配，不声称长期动态地址
管理已完成。无租约文件、无 DHCPRELEASE、ARP 冲突检测、DNS 解析器。
DHCP 不是认证协议，首版假设网络中的 DHCP 服务可信。

## 配置

- 默认，或 `capmix.net=dhcp`：尝试 DHCP，失败回退。
- `capmix.net=static`：保留静态地址，不发送 DHCP；若两种选项同时出现，
  static 优先。初始 NIC 地址仍是 10.0.2.15/24、网关 10.0.2.2。
- `capmix.dhcp_drop=1`：测试开关，丢弃 DHCP 回复，强制验证真实超时回退。

debug 新配置：

```
make iso LIMINE_CONFIG=boot/limine-dhcp-selftest.conf
make iso LIMINE_CONFIG=boot/limine-static-selftest.conf
make iso LIMINE_CONFIG=boot/limine-dhcp-fallback-selftest.conf
```

产品可在选用的 Limine conf 的 cmdline 中加入这些选项；默认 `make iso`
仍为产品 shell。普通 debug 会先跑 M4/M8 固定地址探针和 phase5，再做
P1c 协商，之后执行 P1b，避免 DHCP 改址影响原先静态 ICMP fixture。
P1c 默认先进行 50ms 的强制丢包测试并比较原地址/掩码/网关，随后真实
协商；static 模式不发送该负例测试报文。获取不到 lease 本身不使全量
selftest FAIL，但保持配置的断言失败会 FAIL。

## 接口与校验

`net/dhcp.c` 为本项目自包含实现，依据 [RFC 2131](https://www.rfc-editor.org/rfc/rfc2131)
与 [RFC 2132](https://www.rfc-editor.org/rfc/rfc2132) 的 BOOTP/DHCP 子集。
消息检查 UDP/IP 长度与校验和、无分片、BOOTP op/htype/hlen、事务 ID、
客户端 MAC、magic cookie 和 options TLV 边界。识别 message type(53)、
server identifier(54)、subnet mask(1)、router(3)、lease time(51)；
REQUEST 携带 requested IP(50)、server identifier 和 client identifier(61)。
仅接受所选 server/address 的 ACK；ACK 必须含非零 lease 和连续掩码。
router 缺省记为 0；多个 router 仅取首个。重复已知 option、option overload、
BOOTP relay 均不支持并拒绝。解析失败不会提交部分配置。

普通 UDP 的广播与目的地址限制不放宽。仅活动 DHCP 会话的 67→68 回复
可以走专用 hook，目的必须是 broadcast、当前地址或已选 OFFER 地址；
经过 XDP/nft 后再进入状态机。NIC UDP 68 保留给内核 DHCP，普通 endpoint
bind 返回 DENIED。窄广播发送 hook 只允许 0.0.0.0:68→255.255.255.255:67，
无新增用户广播 syscall。地址提交 API 仅用于 BSP 启动/无用户网络应用
的 debug 生命周期，不是运行中多 CPU 动态改址 API。

`net/runtime.c` 仅追加两个 bootstrap helper，后台 RX 循环与调度核心
保持不变；buddy、时钟、shell/FB、AHCI/COW、VMX/EPT 均未修改。

## 实际验收

默认 QEMU user netdev 提供 DHCP：

```
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial stdio -display none -no-reboot
```

替换为 virtio-net-pci 可测另一种网卡。为证明不是始终使用静态地址，
验收加 `-netdev user,id=n0,dhcpstart=10.0.2.42`，日志应出现 ACK ip=10.0.2.42。
P1b 大 payload 回归需另开宿主 `python3 scripts/udp-echo.py`，并使用
`boot/limine-udp-selftest.conf`；该配置默认也尝试 DHCP。

已取得日志：

- `/tmp/p1c-hosted.log`：codec、ACK/NAK 状态、非法字段检查；全量 PASS。
- `/tmp/p1c-e1000.log`：默认 user netdev ACK 10.0.2.15，全量 PASS。
- `/tmp/p1c-vnet-lease42.log`、`/tmp/p1c-e1000-lease42.log`：ACK 10.0.2.42，
  P1b 内核及 CPL=3 ELF 的 1/257/1472B echo verified，全量 PASS。
- `/tmp/p1c-static.log`：mode=static，无 DHCP 请求，全量 PASS。
- `/tmp/p1c-fallback.log`：回复丢弃、3000ms fallback、原配置不变，全量 PASS。
- `/tmp/p1c-nonic.log`：无卡 skipped，全量 PASS。
- `/tmp/p1c-product.log`：AHCI root=disk，先 ACK 10.0.2.42，再进入 shell；
  udpwire ELF 校验通过、filetest 读到先前启动内容，均 exit=0，ACPI 关机。

产品运行成功不等同全量 selftest；debug 日志的
`selftest result: PASS (0 failures)` 才是全量验收标志。
