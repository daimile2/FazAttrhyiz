# 最小 TCP：连接级 RTO 重传

> 当前容量已续作至全局 32 槽、backlog 8、默认 RX 16KiB（编译可选 4–32KiB）；
> 本文历史参数不覆盖 [TCP_POOL](TCP_POOL.md)，RTO 语义保持不变。

> 本文记录当时的主动连接/RTO交付。后续已新增用户态listen/accept与HTTP服务端，
> 当前服务端ABI、边界及验证见 [HTTPD.md](HTTPD.md)。

## 范围与默认参数

主动 TCP 保持原 connect/send/recv/close syscall 和 capability ABI。本轮为原
stop-and-wait 补充待确认段与超时重发，没有新增用户 syscall 或 socket option。
每连接只有一个待确认 SYN、数据段或 FIN，数据副本最多 536B，存于连接池中。

| 参数 | 首版值 |
| --- | --- |
| 初始 RTO | 200ms |
| 重传退避 | 400、800、1600、1600ms，上限 1600ms |
| 每段上限 | 原始发送 + 最多 4 次重传，共最多 5 次成功提交 |
| 无进展放弃时间 | 约 4600ms；从首次成功提交算，受调度延迟影响 |
| API 总期限 | 原 timeout_ms（1..10000ms），可先于重试上限终止 |

参数是受控 QEMU 场景的固定策略，**没有 RTT/SRTT/RTTVAR 自适应估计**，不是完整
标准 TCP RTO 实现。SYN/每个新数据段/FIN 重新以 200ms 起步；部分 ACK 代表进展，
剩余尾段也重置 RTO/重试预算，但不会延长 API 原总期限。

重传使用已有 cm_rdtsc/cm_tsc_hz，不更改时钟标定、APIC 或 timer ISR。
`net/runtime.c` 后台 RX 每轮调用 cm_tcp_poll；connect/send/close 的等待路径也推进
同一段的定时处理。无需用户另发 syscall 才能推进后台 RTO；仍依赖已有网络 worker
和调度器获得运行机会，不是独立 hrtimer。无网卡的硬件测试 skip，机制测试仍执行。

## 序号、所有权与并发

待确认记录保存 flags、seq、字节副本、下一到期时间、操作期限和重试次数。
重发不增加 SND.NXT；ACK 使用已有累计确认/序号校验。

- 完整 ACK 清除段和定时状态；重复 ACK 不重置计时，不增加已确认长度。
- 部分 ACK 丢弃已确认前缀，剩余副本从新的 seq 开始重传；覆盖了 32-bit 序号回绕测试。
- 用户 syscall 的 bounce buffer 和原用户地址不作为重传存储。send 返回后不会继续
  引用它们；连接内部保存独立副本。
- TCP 锁保护 pending/generation/tx_busy。pending 的发送同一时刻最多一个执行者。
  NIC TX 在锁外，使用私有副本；generation 阻止旧发送完成结果覆盖新 ACK/新段状态。
- 后台 poll 临时 pin 连接槽；原 cap acquire 仍保护 API 对象。关闭后清除 pending，
  即使已取得的 TX 快照正在收尾也不能复用/释放它访问的连接槽。
  已提交的帧或已进入 NIC TX 的快照无法撤回；不承诺网络上关闭后绝无旧帧。
- ARP 尚未解析或 TX 返回 AGAIN 不消耗成功重传次数，短暂重试最多每 10ms 一次；
  外层 API deadline 保持有限。RST 和纯 ACK 不进入此可靠发送记录。

## 失败与回收

上限或 API 期限到达后：连接进入 ERROR，清除 pending 数据/计时，尝试发送 RST|ACK。
RST 是 best effort，网卡关闭、路由不可用或过滤可使其失败，不能假称对端已收到。

上限路径使等待 API 返回 DEAD；外层 API 总超时可能返回 AGAIN，沿用原错误习惯。
致命 wire-send 错误也终止连接。部分 send 已成功时仍返回已确认字节数，连接进入
ERROR；应用不能把整个请求无条件重发，之前的字节可能已经交付对端。

ERROR 不强行销毁持有者的 capability；close 消费当前 handle，最后引用或进程退出
释放连接槽，aliases 共享状态。recv 保留原行为：缓存可先读出，正常 peer FIN 后
读尽是 0，否则 ERROR 且无缓存是 DEAD。上限 selftest 同时验证别名释放后 live
恢复基线、pending=0；没有绕过正在执行的 syscall 去回收栈/用户页。

## 验收方法

宿主先开 TCP/UDP echo：

```bash
python3 scripts/tcp-echo.py
# 另一终端：
python3 scripts/udp-echo.py
# 再一个终端：
make test
make kernel 2>&1 | grep -iE 'warn|error'
make iso LIMINE_CONFIG=boot/limine-tcp-rto-selftest.conf
```

```bash
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -cdrom capmix.iso -serial stdio -display none -no-reboot
```

也可把网卡改为 `virtio-net-pci`。配置含 udp_echo/tcp_echo/tcp_drop=1；
P1d 测试给**自己的连接**设置丢包掩码，丢弃第一份 SYN、第一份数据段和第一份 FIN。
这发生在 TCP 发帧出口，模拟成功发送但网络丢失，不是简单睡眠延迟。
该 native 注入接口受 selftest 模式及 WRITE right 门控，无用户 ABI，产品默认关闭。
其他连接（包括用户 TCP ELF）不被这个掩码影响。统计是成功提交的重传次数；
除了人工丢包，还可能包含真实 QEMU 网络延迟导致的重传。

关键日志：

```text
tcp rto mechanism verified: SYN/data/FIN retained ...
tcp rto exhausted cleanup verified: ... pending=0 ... live=0
tcp test dropped: flags=0x2 / 0x18 / 0x11
tcp rto retransmit: ... retry=N next_rto_ms=N
tcp rto loss recovery: dropped=SYN/data/FIN ... payload=2049 close=ok live=0
tcp ELF syscall verified: CPL=3 send/recv=2049 ... close/exit=0
selftest result: PASS (0 failures)
```

机制测试用显式虚拟时钟覆盖退避/上限，不依赖 hosted 未标定的硬件时钟；
上限清理调用实际 service/ERROR 路径和真实 cap 别名回收，但无 NIC 下 RST 仅尝试、
没有伪造收包。普通 `boot/limine-tcp-selftest.conf` 不设置 drop，验证无丢包成功路径。
无 echo 服务时使用普通 selftest 配置；不能启用 echo 配置后把连接失败算 PASS。

## 保持的限制

- 无拥塞控制、cwnd/ssthresh/慢启动、CUBIC、fast retransmit、SACK。
- MSS=536B，遵守对端更小 SYN MSS；一次仅一段在途，吞吐量受往返时延限制。
- 当前全局 32 个连接槽；每连接接收缓冲/未缩放窗口默认 16KiB（4–32KiB 编译配置）。首次分段受 peer window 限制；
  重传保留已在途尾段，没有完整零窗口 persist/probe、窗口缩放或 WL1/WL2 规则。
  窗口持续为零时可能在期限内失败，不能宣称通用流量控制完备。
- 无乱序接收队列；乱序段丢弃并 ACK，依赖对端重传；无 IP 分片、IPv6、PMTU、
  keepalive、ECN、完整 socket 选项。纯 ACK 发失败仍依赖后续输入/API 活动重试。
- 仅主动连接，无 listen/accept；TIME_WAIT 仍为 100ms，不是完整 2MSL/端口隔离。
- 不改变 fdio/cap 权限、UDP/DHCP、KPTI、buddy 锁、时钟与安装布局。

这是最小可靠性增量，不能据此称为完整互联网 TCP 栈。

## 本轮实测记录

- `/tmp/tcp-rto-hosted.log`：机制/上限/别名回收及 hosted 全量 PASS。
- `/tmp/tcp-rto-e1000-final.log`：e1000 首次 SYN/数据/FIN 丢包，恢复 2049B、FIN；用户 ELF TCP 与全量 PASS。
- `/tmp/tcp-rto-virtio-numa.log`：virtio-net 双 NUMA，同样丢包恢复及全量 PASS。
- `/tmp/tcp-rto-normal.log`：无人工丢包成功路径，内核与用户 ELF TCP 全量 PASS。
- `/tmp/tcp-rto-product.log`：产品用户 shell 分别在 CPU0/1 后台 run TCP ELF 均 exit=0，随后文件、UDP ELF exit=0，ACPI 关机。

close 后后台 RX/RTO 可能尚持有短暂 slot pin，P1d 清理测试最多给后台 1 秒收尾再
核对 live 基线，超时仍报 FAIL；没有提前释放该引用，也没有放宽最终泄漏校验。
