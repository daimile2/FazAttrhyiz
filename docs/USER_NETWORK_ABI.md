# 普通 ELF 网络 syscall ABI

普通 CapMix x86_64 静态 ET_EXEC 现在可以通过真正的 SYSCALL 使用 TCP，
而非只调用内核 P1d 测试。产品 init 补装 `/bin/tcpwire.elf`；原有
`/bin/udpwire.elf` 及 UDP syscall 保留。TCP 内核算法保持 P1d 原型限制。

## 固定编号与参数

CapMix 原生 ABI：RAX=编号/有符号返回值，RDI/RSI/RDX/R10=四个参数。
RCX/R11 被 SYSCALL 破坏。不是 Linux/BSD syscall ABI，不通过 libc socket。
新增编号固定为 **26–30**，已有 0–25、128/129 编号不变。

| nr | 调用 | RDI | RSI | RDX | R10 | 成功返回 |
|---|---|---|---|---|---|---|
| 26 | TCP_OPEN | 0 | 0 | 0 | 0 | 正 capability handle |
| 27 | TCP_CONNECT | handle | request 地址 | 0 | 0 | 0 |
| 28 | TCP_SEND | handle | 只读数据地址 | 长度 1–4096 | timeout_ms | 已确认发送字节数 |
| 29 | TCP_RECV | handle | 可写缓冲地址 | 容量 1–4096 | 0 | 实际读取字节数；EOF=0 |
| 30 | TCP_CLOSE | handle | timeout_ms | 0 | 0 | 0 |

CONNECT request 是 **16 字节、native little-endian、4 字节对齐**：

```
typedef struct {
    uint32_t ifid;        // offset 0; UINT32_MAX = 自动选择 up 的主网卡
    uint32_t address;     // offset 4; 10.0.2.2 = 0x0a000202
    uint16_t port;        // offset 8
    uint16_t reserved;    // offset 10; 必须为 0
    uint32_t timeout_ms;  // offset 12
} cm_tcp_connect_request_t;
```

AUTO 优先 up 的 vnet0，再选择 up 的 enet0；两者都不存在返回 NOENT，
有接口但未 up 返回 DEAD。显式 ifid 越界为 INVAL，不存在为 NOENT；TCP
lo/非受支持接口为 NOTSUP。地址、端口不能为 0。timeout_ms 为 1–10000。

返回的 handle 直接索引**调用者 task 的 capability 表**，不是 FILE fd。
TCP/UDP 都用 typed PORT 对象，由独立析构身份识别协议；不能把 FILE fd 或
UDP endpoint 当作 TCP 连接。connect/send/close 检查 WRITE，recv 检查 READ。
普通 ELF 未开放通用 DUPLICATE syscall；debug 由内核授予只读别名来检验
实际 ring3 权限拒绝。不能用别人的 handle 编号获得别人的对象。

## 指针和等待语义

- CONNECT 的 16B 请求先 copyin 到内核栈。
- SEND 先验证完整用户范围可读，分配一个 4 KiB 临时页，copyin 整段，
  再调用 TCP。等待 ACK/yield 期间持有内核副本，不持有用户缓冲指针。
- RECV 先验证**完整请求容量**对应可写映射，再分配临时页和消费流数据；
  仅实际收到的字节 copyout。因此只读/未映射目标不导致流数据丢失。
- 拒绝内核地址、整数溢出、映射空洞、只读目的页、超 4096B 的单次 I/O。
  不扩展 ELF 虚址窗口，也不改变 P1a 的文件 I/O 上限。
- CONNECT/SEND/CLOSE 有限等待并通过既有 scheduler yield；返回用户前
  调用既有 `cm_usermode_resume_entry()` 恢复入口状态。未修改 syscall.S、
  KPTI、CR3 或 usermode iretq 设计。
- RECV **非阻塞**，没有数据为 AGAIN；应用可以重试。测试使用 CLOCK 的
  原始 TSC tick 作有限期限，不把 CLOCK 当作毫秒或 UTC。
- SEND 可能部分成功，返回已确认字节数；发生超时后连接可能已 ERROR，
  不能假设可以无条件重发完整应用请求。
- CLOSE 成功/超时通常消费 handle；BUSY 或参数/权限错误保留 handle。
  EXIT 会自动 abort/release 所有 TCP capability，包括未显式关闭对象。
- 已支持多个 ELF 并发、固定 CPU 亲和；镜像在 off-CPU reap 前固定，没有
  用户线程同时 unmap 缓冲的 API。入口现场与后台 run 见 [ELF_CONCURRENCY.md](ELF_CONCURRENCY.md)。

## 错误码

| 返回 | 含义 |
|---|---|
| -1 NOMEM | 临时页不足 |
| -2 INVAL | 参数/用户指针/句柄/协议类型无效，或连接状态不适合操作 |
| -3 DENIED | capability 不具备所需 READ/WRITE |
| -5 AGAIN | recv 暂无数据，或有限 connect/send/close 等待超时 |
| -6 NOSPC | 连接池/句柄表资源不足 |
| -7 NOTSUP | 不支持的接口；非 ELF 调用这组 syscall |
| -8 BUSY | 连接已有 writer 或不能重复 connect |
| -9 DEAD | link/连接断开或有效 RST |
| -12 NOENT | AUTO 无网卡，或指定接口不存在 |

零长度或大于 4096 的 SEND/RECV 为 INVAL，不做隐式分块；更大数据用多次
调用。失效的已关闭 handle 为 INVAL。先做参数/映射检查，因此同一请求
含多项错误时不要依赖某一种错误的绝对优先级。

## 测试 ELF 和产品运行

`tests/tcp_elf.S` + `tests/hello_elf.ld` → `build/tcp.elf` →
`build/tcp_elf.inc`，init 使用 VFS 安装缺失的 `/bin/tcpwire.elf`，已安装版本
不覆盖。测试程序默认 AUTO、peer 10.0.2.2:9998、3000ms 内核超时，发送
2049 个确定性字节，按最多 73B 分次读回并比较，FIN 关闭后检查失效 handle。
它还检查内核指针、只读目标、溢出、超上限和 UDP/TCP 类型混用。
产品 fixture 的 recv 等待预算为 15000000000 个原始 TSC tick，并非可移植
毫秒时钟；debug 根据标定 Hz 设置为约 3 秒。内核 TCP 超时参数仍为毫秒。

宿主另开服务：

```
python3 scripts/tcp-echo.py
python3 scripts/udp-echo.py
```

产品 `make iso` 后启动 QEMU user netdev（e1000 或 virtio-net-pci）：

```
timeout 60 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial stdio -display gtk -no-reboot
```

shell 输入：

```
run /bin/tcpwire.elf
run /bin/udpwire.elf
run /bin/filetest.elf
poweroff
```

成功日志：`tcp user ELF verified: CPL=3 bytes=2049 ... FIN exit=0`，
随后 `shell run done ... exit=0 reclaim=0`。无 NIC 时 connect=NOENT，
测试 ELF 明确打印 skipped、close/reclaim 后 exit=0；有 NIC 但 echo 不存在
时有限失败并 exit=1，不能将其视为 payload 成功。

原 UDP 编号 17–21、25 保留，1472B datagram、非阻塞 recv；详细布局见
[P1b](P1B_UDP_NIC.md)。DHCP 的 lease/static fallback 不变，见
[P1c](P1C_DHCP.md)。TCP 当前为全局 32 槽、默认 RX 16KiB（4–32KiB 编译配置，见 [TCP_POOL](TCP_POOL.md)）、MSS≤536、单个未确认段，
具有连接级 RTO、listen/accept 和最小 HTTP 工具；TIME_WAIT=100ms，
仍无完整拥塞控制、select/poll 或 TLS。详见 [TCP_RTO](TCP_RTO.md)、[HTTPD](HTTPD.md)。

## Debug 与证据

```
make iso LIMINE_CONFIG=boot/limine-tcp-selftest.conf
```

保留 P1d 内核 selftest，随后增加真实用户 ELF 测试。内核授予只读 TCP
cap，用户 SEND 实测 DENIED；用户 EXIT 留一个未关闭对象检查自动清理。
页数基线在 fixture 写入后采样，排除需要保留的文件缓存；断言临时页/
ELF 图片、连接全部回收。hosted 测试调真实分发器及 image-backed uaccess，
明确不声称执行 CPL=3。

已执行：

- `/tmp/tcpelf-hosted.log`：ABI、copy/权限检查，全量 PASS。
- `/tmp/tcpelf-e1000.log`、`/tmp/tcpelf-vnet.log`：CPL=3 TCP syscall、2049B
  内容验证、FIN、EXIT/页回收；全量 PASS。
- `/tmp/tcpelf-nonic.log`：真实 ELF 的 NOENT/skip/exit=0，全量 PASS。
- `/tmp/tcpelf-product-vnet.log`、`/tmp/tcpelf-product-e1000.log`：AHCI root=disk，
  DHCP lease 10.0.2.42，shell run TCP/UDP/文件 ELF；TCP/UDP/文件 ELF 都 exit=0，reclaim=0。

关键字：`tcp ELF syscall verified`、`readonly-right`、`tcp ELF selftest: PASS`、
`bounce/image pages reclaimed`、`tcp user ELF skipped: no NIC`。
产品 exit=0 与 debug 的全量 `selftest result: PASS (0 failures)` 分别验收。
