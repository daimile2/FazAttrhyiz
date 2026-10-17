# 最小用户态 TCP / HTTP 服务端

> 当前容量已续作至全局 32 槽、backlog 8、默认 RX 16KiB（编译可选 4–32KiB）；
> 本文历史参数不覆盖 [TCP_POOL](TCP_POOL.md)，RTO 语义保持不变。

`/bin/httpd.elf` 是使用 CapMix SDK/crt 构建的静态 ET_EXEC，在 CPL=3 通过
普通 TCP syscall 监听、accept、读取请求和发送响应。不是内核自测代发响应。

## 构建与运行

```bash
make examples iso
```

新产品根启动时通过 VFS 写入 `/bin/httpd.elf` 并 fsync；安装盘无 ISO 启动也会
provision 缺失路径。已有安装盘必须先更新内核到含 listen/accept ABI 的版本，
仅复制 ELF 到旧内核不够。系统 provisioning 不覆盖已有同名程序。
对已使用新内核、VM 已关闭的旧根，可额外导入缺失的服务 ELF：

```bash
make examples build/cow-put
python3 scripts/copy-to-disk.py /path/system.img build/example-httpd.elf /bin/httpd.elf
```

guest shell：

```text
run /bin/httpd.elf 8080 &
# 或只处理两个已 accept 的连接后退出，便于验收
run /bin/httpd.elf 8080 2 &
```

缺省端口8080。第二参数为1..100个连接尝试，不是成功响应数；省略则持续监听。
后台运行不需要 `run -i`，不接收键盘输入。结束用 `kill PID`；`poweroff` 仍沿用
5秒等待、2秒 kill/reap 的既有策略。前台无限监听会占住 shell，建议使用后台或指定次数。

### QEMU：宿主访问 guest

无 ISO 的 AHCI 安装盘示例（磁盘须已安装，不能把任意数据盘拿去格式化）：

```bash
qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -drive id=disk,file=/tmp/capmix-httpd.img,if=none,format=raw \
  -device ich9-ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:18080-:8080 \
  -device e1000,netdev=n0 -serial stdio -display gtk -no-reboot
```

也可换为 `-device virtio-net-pci,netdev=n0`；或以 `-cdrom capmix.iso` 启动产品ISO。
网卡优先级沿用 virtio-net > e1000。示例固定guest地址10.0.2.15，需与实际 DHCP/static
地址一致；QEMU未显式写 guestaddr 时采用 user-net 缺省 guest 地址。
在guest启动服务后，宿主执行：

```bash
python3 tests/check_httpd.py --port 18080 --requests 2
curl --http1.0 --max-time 5 http://127.0.0.1:18080/
printf 'GET / HTTP/1.0\r\nHost: test\r\n\r\n' | nc -w 5 127.0.0.1 18080
```

有限次数服务会在次数用尽后退出；运行多个客户端前需相应增加次数或重启服务。
客户端应拿到 `HTTP/1.0 200 OK`、`Content-Length: 25` 和正文
`hello from CapMix httpd!` 加换行。校验脚本检查字节内容、长度及正常EOF。
这里是宿主进程经NAT连接guest；**没有新增guest本机TCP回环**。

### VirtualBox：UEFI + AHCI + e1000 NAT

使用已能启动 CapMix 的独立VM，关机时设置（名称只是示例）：

```bash
VBoxManage modifyvm CapMix-test --nic1 nat --nictype1 82540EM --cableconnected1 on
VBoxManage modifyvm CapMix-test --natpf1 'httpd,tcp,127.0.0.1,18081,10.0.2.15,8080'
VBoxManage startvm CapMix-test --type headless
```

已有同名转发规则时，先在VM关闭状态执行
`VBoxManage modifyvm CapMix-test --natpf1 delete httpd` 再添加。
UEFI、AHCI盘及串口设置参见 [VirtualBox测试指南](VIRTUALBOX_TESTING.zh-CN.md)。
启动guest服务后，宿主执行：

```bash
python3 tests/check_httpd.py --port 18081 --requests 2
curl --http1.0 --max-time 5 http://127.0.0.1:18081/
```

确保实际guest地址与转发目标一致。启动横幅可能记录网卡早期link状态；RX worker
仍会刷新link。若服务报 `DEAD`，等待link稳定后重试，不能将其写成网络测试PASS。
本机不要同时运行QEMU KVM与VirtualBox硬件加速guest；本轮按顺序运行。

## syscall ABI / capability

旧TCP syscall26..30保持不变；新增：

| 号 | 名称 | 参数 / 返回 |
| --- | --- | --- |
| 40 | tcp_listen | `(handle, request_user_ptr, 0, 0)`，成功0 |
| 41 | tcp_accept | `(listener_handle, 0, 0, 0)`，成功返回新TCP capability handle |

listen request恰为8字节：`uint32_t ifid; uint16_t port; uint16_t backlog;`。
`ifid=UINT32_MAX` 选主网卡，仅支持 vnet0/enet0，不接受lo。
request经image-backed copyin；非法指针/保留参数拒绝，accept没有用户指针输出。
先用原 `tcp_open` 创建TCP对象，再listen。SDK包装为 `cm_tcp_serve(port, backlog)`、
`cm_tcp_accept(listener)`，已接收的句柄沿用 `cm_tcp_read/write/hangup`。

- listen要求TCP对象WRITE；accept要求READ|WRITE，检查具体TCP对象类型，不能拿UDP/FILE代替。
- accepted对象独立持有READ|WRITE|DUPLICATE；不借用监听句柄或用户指针。
- **accept非阻塞**：没有已完成握手的连接返回`AGAIN=-5`，应用yield后重试；半连接不返回。
  返回顺序为可用槽顺序，不承诺严格FIFO。recv也非阻塞；EOF返回0。
- 主要错误：INVAL=-2，DENIED=-3，AGAIN=-5，NOSPC=-6，NOTSUP=-7，BUSY=-8，
  DEAD=-9，NOENT=-12。无网卡通常NOENT；网卡down为DEAD；端口冲突为BUSY。
- accept时capability/handle资源不足返回NOSPC，并中止该候选连接，避免隐藏资源泄漏；
  监听器仍可继续使用。

## 有界行为与关闭

- 全局32个TCP槽：监听器、主动连接、排队半连接/已连接、accepted连接都计入。
  最多32个监听器，但占满后无法再建立连接；不为任何监听器预留专属槽。
- backlog为1..8，半连接与已握手未accept连接合计；非法backlog返回INVAL。
  队列或全局槽满，内核记录`tcp listen refused ... error=-6`并尝试RST拒绝，
  不覆盖现有连接。RST发送仍可能遇到ARP/TX失败，客户端必须有超时。
- 被动握手为LISTEN→SYN_RECEIVED→ESTABLISHED；SYN/ACK用原有RTO保留/重发，
  握手截止5秒，重试预算沿用4次重传；未accept连接从SYN起最多保留10秒。
  后台poll回收错误/到期排队连接。建立后沿用既有数据、FIN、关闭状态与RTO。
- close监听器会中止所有未accept连接并释放队列引用，不等待远端关闭；已accept连接独立存活。
  close任意监听别名使该共享监听状态失效，其余别名最终drop时释放槽。
  进程exit/kill释放监听及accepted句柄；RX/TX临时引用可能稍后才退出，但不会重新分配仍被引用的槽。
- httpd每次只处理一个accepted连接；最多读2048B请求头，完整CRLFCRLF才响应，
  总读取时间3秒。只检查最小GET前缀，不承诺完整HTTP语法/Host/版本/Content-Length验证。
  不消费请求body，不做keep-alive；所有路径返回同一文本，不访问文件或解析URL。
  请求不完整、超大、超时或不是GET则关闭；没有完整HTTP错误响应集。
- 响应发送和连接关闭各沿用3000ms；无TLS、线程池、DNS、本机回环、SACK或拥塞控制。
  TCP仍沿用MSS536、RX4096B、stop-and-wait及有限TIME_WAIT；不是通用生产HTTP服务。

## 验证与环境矩阵（2026-10-15）

硬件/软件：Linux宿主，Intel i5-13500H，QEMU8.2.2/KVM、VirtualBox7.2.20r175154。
以下按实际结果填写；产品路径不会输出全量selftest PASS。

| 环境 | 结果 | 证据 |
| --- | --- | --- |
| hosted | PASS，0 failures | `/tmp/httpd-hosted-final.log` |
| QEMU KVM SMP4单节点e1000 / 双NUMA双NIC debug | 两次PASS，0 failures；virtio-net/e1000主动TCP/RTO丢包、UDP及NUMA回归 | `/tmp/httpd-qemu-debug.log`、`/tmp/httpd-qemu-debug-final.log` |
| QEMU SMP4 AHCI盘，无ISO | PASS：两次HTTP200/25B/EOF，满队列拒绝、重复监听拒绝、kill后重绑定；nc/http-get、重定向、poweroff | `/tmp/httpd-qemu-product.log`、`/tmp/httpd-qemu-client-rebind.log`、`/tmp/httpd-qemu-limits.log` |
| VirtualBox UEFI/AHCI/e1000 NAT SMP4，无ISO | PASS：宿主两次HTTP200/25B/EOF，服务exit0/reap，ACPI关机；未运行VBox全量debug | `/tmp/httpd-vbox-product.log`、`/tmp/httpd-vbox-client.log` |

关键日志：`tcp listen`、`tcp established ... passive=1`、`tcp accept`、
`httpd response: HTTP/1.0 200 payload=25 verified`、`tcp listener close`、
`tcp passive verified ... reclaim live=0`、`exit=0 reap=ok`。
独立客户端的`httpd client: PASS`才证明实际收到正确响应；guest自身输出不是客户端校验替代品。

## 本轮修改文件

- `net/tcp.c`、`include/capmix/tcp.h`：被动握手、有界队列、accept引用转移、超时/关闭回收及机制测试。
- `proc/syscall.c`、`include/capmix/syscall.h`：新ABI、ELF白名单、copyin与capability检查。
- `sdk/include/capmix.h`、`examples/httpd.c`：用户包装与静态HTTP服务ELF。
- `Makefile`、`proc/init.c`：构建、嵌入与根文件provisioning。
- `boot/tcp_elf_test.c`、`tests/check_httpd.py`：非法指针/权限测试与宿主端到端校验。
- `docs/HTTPD.md`及文档索引、EXAMPLES、EXTERNAL_ELF、总览、P1D_TCP、TCP_RTO：当前用法和历史边界说明。

未修改RTO保留/退避算法、NIC IRQ、shell重定向、KPTI、buddy、时钟及安装布局。
