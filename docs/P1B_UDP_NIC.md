# P1b：后台 RX 与实网卡 UDP

## 实现和边界

`net/runtime.c` 使用一个独立 EEVDF 内核线程消费 virtio-net/e1000 DMA RX
环。每轮每接口最多处理 16 个 completion，随后 yield；产品 PID1 创建后
自动启动，持续在 shell 等待输入、用户 ELF 执行期间收包。它不依赖应用
调用硬件 receive 或 BSP 运行 M4/M8 probe 才收包。首版仍关闭硬件 IRQ，
不是中断驱动，不修改 LAPIC 标定、buddy 锁或 shell/FB。产品现有 BSP
调度限制保留：后台线程可在 BSP 上独立被调度，不宣称产品 RX 跑在 AP。
无包时仍会轮询并 yield；尚无 IRQ 唤醒/节能睡眠。

驱动原有 IRQ-save 锁保护描述符和同步 TX；新增邻居锁只保护缓存，不在
持锁时发包。RX 线程负责 ARP 和 UDP 分流，UDP 校验 IPv4/UDP 长度、
校验和、无分片、目的地址，经过既有 XDP → nft 后进入 endpoint 队列。
不支持 VLAN/IP options/IPv4 分片。硬件 UDP 可含 Ethernet 最小帧填充，
填充不会误算进 UDP payload。非 UDP/ARP 帧仅分类，不是完整 ICMP 服务。

- 静态地址仍为 10.0.2.15/24，网关 10.0.2.2。
- bind 可以指定 up 的 lo、vnet0 或 enet0；主网卡优先 vnet0 > enet0。
- **payload 1–1472B**，对应 IPv4 MTU 1500，不支持零长度/分片/Jumbo。
- 每 endpoint 8 个 datagram，16 个 endpoint。满时丢弃新包并增加 drop；
  内核 inject 返回 AGAIN。recv 非阻塞，无包 AGAIN；仍检查 READ right。
- 每接口一个 ARP 邻居项，更新会替换旧项，尚无到期机制。直接同网段
  ARP，否则 ARP 静态网关；无完整路由表、DHCP、NAT/TCP。
- ARP 未解析时 send 返回 AGAIN，发送 ARP 请求最多每 100ms 一次。
  UDP 不会悄悄保存等待；调用者必须重试。硬件 TX 仍同步、超时有限。
- 自身 NIC 地址、广播、多播目的暂不支持；本机通信使用 lo。
- task exit 关闭原有 typed UDP cap，释放端口、丢弃队列。

## 用户 ABI

旧编号 17/18/19/20/21 仍为 open/bind/send/recv/close。19 在 lo 上不变；
在实网卡上默认发送到所绑定接口的静态网关。25 `UDP_SENDADDR` 增加显式
目的 IPv4：RDI=endpoint handle、RSI=请求地址、RDX=R10=0。

```
struct request {                 // 1480 bytes, native little-endian fields
    uint32_t daddr;              // e.g. 0x0a000202
    uint16_t dport, len;
    uint8_t data[1472];
};
struct datagram {                // 1488 bytes
    uint32_t saddr, daddr;
    uint16_t sport, dport, len, reserved;
    uint8_t data[1472];
};
```

25 要求整个请求结构可读，并统一 copyin；20 要求整个结果结构可写，再
copyout；非法内核指针/只读目标拒绝。发送成功返回 payload 长度。
旧 272B datagram ABI 的应用需要按新头文件重编译，不能继续使用小缓冲。
`tests/net_elf.S` 已同步，原 phase5 lo/filter/cap/lifecycle 测试保留。
这仍是 capability UDP ABI，不是 BSD socket/fd，也不是 Linux syscall ABI。

## 与 M4/M8 的关系

M4/M8 原有 ARP/ICMP 同步探针和 e1000 ring-wrap 测试保留。debug 先执行
它们和 phase5，然后才启动后台线程，避免两个 RX 消费者抢同一回复。
P1b 测试结束等待线程退出/reap，随后继续其他 phase；产品不跑探针，直接
启动后台 RX。debug 调试时不要在后台线程活动期间直接调用旧 raw probe。

## 验收方法

QEMU user netdev 没有通用 UDP echo 服务。宿主先运行：

```
python3 scripts/udp-echo.py
```

服务只绑定宿主 127.0.0.1:9999，QEMU 的 10.0.2.2 别名可以访问。
然后用 opt-in 配置（含 `capmix.udp_echo=1`）：

```
make kernel 2>&1 | grep -iE 'warn|error'
make iso LIMINE_CONFIG=boot/limine-udp-selftest.conf
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -serial stdio -display none -no-reboot 2>&1 | tee /tmp/capmix_run.log
```

e1000 将 device 替换为 `e1000,netdev=n0`。可同时加第二个 user netdev
和另一种设备，分别测试两张网卡；各后端保持独立子网。
开启 echo 测试而服务缺失时，有限超时后 **FAIL**，不会假报 payload verified。
普通 `boot/limine-selftest.conf` 不要求外部服务：有卡验证后台 ARP RX、
UDP TX，注明 echo skipped；`-nic none` 明确无卡 skip，lo 仍完整验收。

产品默认 `make iso` 后，在 shell 使用：

```
run /bin/udpwire.elf
run /bin/filetest.elf
poweroff
```

init 仅补装缺失的 udpwire ELF。测试程序默认 interface 0（产品首先注册的
硬件接口），本轮固定 peer 10.0.2.2:9999、源端口 9700，执行 1/257/1472B
发送、接收、完整内容比较。它不调用 raw receive；后台线程由 tick 抢占
获得运行机会。首版测试超时按固定 TSC tick 门限，debug 按标定 Hz 修正；
它不是具有可移植时钟库的通用客户端。重复并发运行同端口尚不支持。

## 已执行结果

- hosted：`/tmp/p1b-hosted.log`，全量 PASS (0 failures)。
- 4 CPU virtio：`/tmp/p1b-vnet.log`，全量 PASS。
- 4 CPU e1000：`/tmp/p1b-e1000.log`，全量 PASS。
- 双网卡 + 双 NUMA：`/tmp/p1b-dual.log`，两接口 payload verified，全量 PASS。
- 无 NIC：`/tmp/p1b-nonic.log`，明确 skip，全量 PASS。
- 普通 debug 无 echo 选项：`/tmp/p1b-normal.log`，后台 RX 验证，全量 PASS。
- AHCI 产品根：`/tmp/p1b-product-vnet.log` 与 `/tmp/p1b-product-e1000.log`。
  shell run udpwire 的 CPL=3/1/257/1472B 校验，filetest 的先前启动文件
  内容校验及 exit=0；产品启动不等同全量 selftest PASS。

关键词：`net bg RX started`、`p1b UDP vnet0/enet0 bg RX verified`、
`bytes=1730 echo=payload verified`、`p1b udp wire user verified`、
`p1b selftest: PASS`。bytes=1730 是 1+257+1472，各自校验，不是一个超 MTU 包。

内核客户端在调用任何 recv 前，先等待后台成功入队计数增长，日志为
`queue-before-recv verified`；因此检验的不是在 recv 内临时轮询硬件。
