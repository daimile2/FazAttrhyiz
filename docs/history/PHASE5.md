# 阶段 5：lo IPv4 UDP 与用户可见 datagram 收发

> 历史阶段记录：保留当时实现与验收边界；后续功能可能已扩展。当前状态见
> [内核总览](../KERNEL_OVERVIEW.zh-CN.md)，操作测试见 [VirtualBox手册](../VIRTUALBOX_TESTING.zh-CN.md)。

## 支持范围

新增的是有界 IPv4 UDP datagram API，不是完整 BSD socket API。仅支持本地
127.0.0.1、显式非零端口绑定、1..256 字节 payload、16 个 endpoint、每端点
8 个完整 datagram。没有 TCP、ARP、路由、NAT、IP options、分片重组、真实 NIC
或 DMA，没有 eBPF JIT。recv 非阻塞，空队列返回 AGAIN。

endpoint 使用 `CM_OBJ_PORT` capability，payload 用专属析构函数标识 UDP 类型，
避免将其他 PORT 对象当 socket。操作先检查对象类型和 handle rights，再取得
稳定引用。WRITE 用于 bind/send，READ 用于 recv；DUPLICATE/TRANSFER 按现有
capability 语义工作。不是 POSIX fd，不能使用 fdio 文件读写接口。

声明位于 `include/capmix/net.h`：

```c
cm_udp_open(task, &hid);
cm_udp_bind(task, hid, lo_ifid, port);
cm_udp_sendto(task, hid, destination_port, payload, length);
cm_udp_recv(task, hid, &datagram);
cm_udp_close(task, hid);
```

同一 iface/port 只能由一个 endpoint 绑定，重复返回 EXIST。未绑定对象不能收发。
关闭单个 duplicate 不解除绑定，最后引用释放时解除绑定并丢弃其排队 datagram。
非 UDP 对象不能经 UDP close 关闭。endpoint/队列来自固定内核池，不涉及 VMO
映射或整缓冲零拷贝；本阶段明确选择有界 payload 拷贝。

## 网络路径与过滤

`cm_udp_sendto()` 构造软件 Ethernet + IPv4 + UDP frame，计算 IP checksum 和
UDP pseudo-header checksum，然后进入 `cm_udp_inject()`：

```text
bound capability -> frame encode -> strict UDP ingress validation
                 -> existing cm_net_rx() XDP -> nft
                 -> local destination-port lookup -> receive queue
```

严格 ingress 检查 Ethernet 类型、IPv4 version/IHL、TTL、总长度、UDP 长度、
本地地址、非零端口、fragment/reserved flags 和 checksum。IHL 只支持 20 字节，
拒绝 MF/非零 offset。IPv4 UDP 的 checksum=0 按协议允许；本模块发送始终计算
checksum，计算结果为 0 时编码为 0xffff。支持奇数字节 payload。

旧 `cm_pkt_parse()` 和 metadata-only XDP/nft 测试保留；严格检查仅在新 UDP ingress
执行，不能把旧演示构造的简化报文当真正 UDP 输入。

只有 XDP_PASS 且 nft 不 DROP 才进入 UDP 队列。关闭 eBPF 只跳过 XDP，nft 仍执行。
XDP_TX/ABORTED 等不投递 UDP，返回 NOTSUP；没有新实现的 TX/REDIRECT 硬件路径。
本轮不重写 eBPF verifier/interpreter 或 nft 匹配机制。

TX 现在只在实际 UDP frame 发送尝试或原 XDP_TX 动作增加；取消了原 RX PASS 时
增加 TX 的“交给协议栈”占位。合法 ingress 经现有 RX 路径增加 RX，坏 checksum/
长度等报文计入 RX+drop；无绑定端口、队列满及策略拒绝也计 drop。
send 返回 NOENT（无接收端）、AGAIN（接收队列满）、DENIED（过滤 DROP）等明确状态。
TX/RX 不等同于最终用户 recv 成功数。

## syscall 与用户 ELF

增加原生 syscall 号 17..21，遵循原 x86_64 ABI：

| 调用 | 参数 | 成功返回 |
|---|---|---|
| UDP_OPEN (17) | 无 | capability handle |
| UDP_BIND (18) | handle, iface ID, port | 0 |
| UDP_SEND (19) | handle, destination port, buffer, length | payload 字节数 |
| UDP_RECV (20) | handle, `cm_udp_datagram_t *` | payload 字节数 |
| UDP_CLOSE (21) | handle | 0 |

UDP_RECV 的输出包含 saddr/daddr、sport/dport、len、reserved 和固定 256 字节 data。
SEND copy-in 前校验完整 ELF 用户可读范围，RECV copy-out 前校验完整可读写范围；
拒绝内核地址、RX text、缺页和溢出。recv 失败不修改输出，不阻塞或切换上下文。
这些 syscall 当前只面向有实际 ELF 镜像的进程；其他内核 C 模型进程使用上面的
native kernel API。

正常 SYS_EXIT 调用 `cm_udp_process_exit()` 清理本 task 的 UDP 句柄。其他持有者的
duplicate/transfer 引用不受影响。该 helper 可用于其他 teardown，但本轮没有
扩展完整信号/kill/task 资源回收体系。

KPTI、VMO/VMAR、ELF loader、serial runner、入口汇编和 phase4 驱动收发路径没有
改动。仅给既有 ELF syscall 分发新增已校验的 UDP 调用；phase4 的受限 IRQ recv
分支原样保留。

## selftest 与镜像生成

`boot/phase5_test.c` 在 phase2 后、phase4 设备离线前执行，先创建/复用 lo。
原 net 分类测试随后复用该 lo，并继续在 eth0 验证原 XDP/nft/kill-switch 行为。

测试分两条路径：

1. **内核两端 UDP echo**：client 绑定 9000，server 绑定 9001，发送、接收、反向
   echo；tx/rx 各 +2，17 字节 payload 相同。补测权限、重复绑定、队列满/空、
   malformed checksum/长度/fragment、close 引用生命期，以及真实 UDP 过滤。
2. **用户 CPL=3 UDP self-loop**：`/bin/net.elf` 创建 endpoint，绑定 9100，向自身
   本地端口发送，经整个内核 UDP/XDP/nft 路径后接收并逐字节校验，再检查空队列
   AGAIN、显式关闭、正常 EXIT 自动清理第二个绑定 9101 的 endpoint。
   用户测试是 self-loop，不声称它在运行另一个并发用户 echo server。

`tests/net_elf.S` 复用已有 ET_EXEC 链接布局，Makefile 构建 `build/net.elf`，
`embed-elf.py` 生成数组，selftest 通过 VFS 写入/fsync `/bin/net.elf` 后加载。
用户代码记录 CS 的 CPL 和 recv 长度，打印 `udp lo rx ok`，exit 0。
hosted 只验证 kernel API、队列与生命周期，明确不执行 ELF syscall/CPL=3。

日志关键字：

```text
udp lo echo ok: tx +2 rx +2 bytes=17 payload verified (odd-length checksum)
udp filters ok: XDP DROP blocks recv, kill-switch restores delivery, nft still DROP
udp queue/rights/close ok
user[...]: udp lo rx ok
udp user ok: CPL=3 lo tx/rx payload verified bytes=17 exit=0
udp lo counters: tx=... rx=... user_delta=1/1 payload verified
udp exit cleanup ok
phase5 selftest: PASS (0 failures)
```

默认 hosted 与四核 KVM 实际日志均出现 `selftest result: PASS (0 failures)`。
四核 KVM 实际记录上述 CPL=3 收发与 payload 校验；phase0/1/2/3 通过，phase4
仍包含 `driver irq user ok` 与 `driver crash isolation ok`。内核构建没有新增
warning/error。

```sh
make test
make kernel 2>&1 | grep -iE 'warn|error'
make iso
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -serial stdio -display none \
  -no-reboot -d cpu_reset 2>&1 | tee /tmp/capmix_run.log
```

QEMU selftest 后 HLT，timeout 124 为正常停止方式，以串口 PASS 为验收。
