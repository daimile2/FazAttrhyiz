# Linux ABI L3：有界 IPv4 socket 映射

Linux persona 仍是可选翻译层，产品启动默认关闭。原生 syscall、capability 与
`/bin/*.elf` 保留；新原生软件建议 `.cmx` 或 `/bin/cm/`。关闭编译/启动开关不会删除
程序或用户数据。本轮没有扩大为通用 Linux、通用 BusyBox 或任意 musl 兼容。
L1 仍是**定制 musl 静态 BusyBox 子集 + 有界 BSS 分配**；其文件工具不变。

## 开关与双轨

```sh
make iso LIMINE_CONFIG=boot/limine-linux-selftest.conf  # 显式开启的自测
make iso                                             # 默认 product，运行期开关关
make iso LINUX_ABI=0                                  # 编译禁止 Linux persona
```

产品/安装盘的 Limine cmdline 加 `capmix.linux=1` 才启用。沿用 L0 的 ELF 标记与
persona 识别；PT_INTERP 仍拒绝。原生进程不进入 Linux socket 分发。
Linux 启用时首次产品启动补入 `/bin/linux-net.elf`，已有同名文件不覆盖。
默认关闭时不自动安装该文件，已有文件保留但不能作为 Linux persona 执行。

## syscall 与布局

| x86-64 Linux 号 | 支持内容 |
| --- | --- |
| 41 socket | AF_INET=2；SOCK_STREAM=1/TCP、SOCK_DGRAM=2/UDP；协议 0/6/17 |
| 42 connect | TCP 有界同步连接；UDP 设置默认对端 |
| 49 bind / 50 listen | 本地 IPv4/端口；TCP backlog 限 1–4 |
| 43 accept / 288 accept4 | 返回独立 fd；accept4 支持 NONBLOCK/CLOEXEC |
| 44 sendto / 45 recvfrom | 六寄存器参数；UDP 地址与数据，TCP 无目标地址发送 |
| 51 getsockname / 52 getpeername | IPv4 地址、实际端口，socklen 截断复制规则 |
| 0 read / 1 write / 3 close | 根据 fd 对象类型转到文件、控制台或网络 |
| 19 readv / 20 writev | TCP 沿用有界 iovec；UDP 返回 ENOSYS，避免拆成多报文 |
| 32 dup / 33 dup2 / 292 dup3 / 72 fcntl | 共享对象引用；每 fd CLOEXEC；GETFL/SETFL 的 NONBLOCK 子集 |
| 5 fstat / 8 lseek | socket 类型元数据；lseek 返回 ESPIPE |

号表与结构依据固定版本 Linux v6.6：
[syscall_64.tbl](https://github.com/torvalds/linux/blob/v6.6/arch/x86/entry/syscalls/syscall_64.tbl)、
[in.h](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/in.h)。
`include/capmix/linux_uapi.h` 定义 16 字节 sockaddr_in（family 偏移0，port偏移2，
IPv4偏移4）与32位 socklen，编译期断言大小/偏移。family 为宿主小端，port/IP 为网络字节序。
沿用 L1 的固定 stat 布局，不把内核私有结构直接返回用户。
所有地址/长度参数经现有用户窗口检查和 copyin/out；非法指针 EFAULT，写出支持 L2 COW。
单次用户缓冲仍受已有 1MiB 访问窗口策略约束。

## 权限、生命周期与容量

每 socket 是 `CM_OBJ_LINUX_SOCKET`，fd 映射到有 READ/WRITE/DUPLICATE 权限的句柄。
包装对象内部私有 capability 表持有现有 TCP/UDP 后端句柄；不是新调度任务。
这样 fork、dup、子进程退出不会误杀其他别名仍持有的连接。
最后一个引用 close/exit/kill 才关闭后端；execve 只关闭 CLOEXEC fd。
每个包装对象占2页，fd 上限32；TCP 全局8连接（listener也占槽），UDP全局16端点，
沿用现有后端连接/队列容量，不承诺每进程独占。
TCP listen backlog 最大4，共享同一个原生 TCP 池，满时明确失败；失败路径释放包装页与能力。
同一接口端口 bind 冲突返回 EADDRINUSE；未绑定 connect/listen 自动选择临时端口。

Linux 进程仍沿用 L2 的 BSP 亲和。TCP 状态机、RTO、RX IRQ/worker 都复用原生实现；
仅增加本地 bind 与端点快照接口，没有复制一套传输状态机。
ANY 绑定选择一个当前 up 接口（virtio-net > e1000），不是所有网卡的通配监听。
无 NIC 的 TCP connect 返回 ENETUNREACH；UDP lo 仍可用。

## 首版限制与错误

- 只支持 IPv4 数字地址；无 TCP 本机回环、DNS、TLS、动态链接。
- connect/send/默认 read/accept 的等待上限约3秒，通过 yield 等待；read/accept 到期 EAGAIN，
  connect 到期 ETIMEDOUT。不是无限阻塞的 BSD socket。最后 close 最多等约1秒，失败则中止并回收。
- NONBLOCK/MSG_DONTWAIT 支持接收与 accept；TCP 非阻塞 connect/send 返回 ENOSYS，
  不伪造 EINPROGRESS。accepted fd 不继承 listener NONBLOCK，需 accept4 显式指定。
- TCP 单次 I/O 最多4096字节，可以短读/短写，调用者应循环。UDP 发送最多1472字节、队列8包；
  接收缓冲较小时截断并丢弃报文剩余部分。零长度 UDP 尚不支持，返回 ENOSYS。
- MSG_NOSIGNAL 仅允许发送（当前无 SIGPIPE）；其他消息标志、setsockopt/getsockopt、shutdown、
  IPv6、复杂 fcntl 返回 ENOSYS 并累加该进程的 `linux_enosys`。未知调用同样计数。
- fd 无效 EBADF、非 socket ENOTSOCK、未连接 ENOTCONN、无目的地址 EDESTADDRREQ、
  发送过长 EMSGSIZE、资源满 EMFILE/ENOBUFS、空非阻塞接收 EAGAIN。
- connect 失败后请 close 并新建 socket 重试；不承诺在 ERROR 后复用同一个连接对象。
- 没有增加 Linux socket poll/select/futex/线程兼容；原生网络 API、RTO 限制仍适用。

## 固定测试 ELF 与产品命令

`tests/linux_l3_start.S` + `tests/linux_l3.c` + `tests/linux_l3.ld` 是仓库固定源码的
无 libc 静态 Linux ET_EXEC，构建产生 `build/linux-l3.elf` 并嵌入内核供安装。
不依赖宿主 glibc 或下载的可变二进制。可以用 `sha256sum build/linux-l3.elf` 记录本次产物。

宿主先运行 `python3 scripts/tcp-echo.py`（9998），并准备 HTTP：

```sh
mkdir -p /tmp/l3-www
printf 'linux L3 host body\n' > /tmp/l3-www/hello.txt
python3 -m http.server 8000 --bind 0.0.0.0 --directory /tmp/l3-www
```

启用 Linux 的用户 shell：

```text
run /bin/linux-net.elf udp       # lo，1024字节、六参数、错误指针不出队
run /bin/linux-net.elf echo      # 10.0.2.2:9998，2049字节、dup/close
run /bin/linux-net.elf inherit   # fork退出不杀连接；exec CLOEXEC/保留fd
run /bin/linux-net.elf http      # 10.0.2.2:8000/hello.txt，检查状态/body
run /bin/linux-net.elf server &  # ANY:8082，接收一次请求，HTTP/1.0 200 后退出
```

服务端是单连接固定响应测试，不是 HTTP 产品服务；没有并发线程池。
QEMU 加 `-netdev user,id=n0,hostfwd=tcp:127.0.0.1:18082-:8082 -device e1000,netdev=n0`，
然后宿主 `python3 tests/check_linux_httpd.py --port 18082`（默认只请求一次）。
原生 `/bin/httpd.elf` 继续使用8080，不与 Linux 测试混用。
VBox NAT 示例（VM须关机）：
`VBoxManage modifyvm VM --natpf1 "linux-l3,tcp,127.0.0.1,18082,,8082"`。
VBox 宿主服务通常通过10.0.2.2访问；以实际 NIC/link 日志为准。

## 验证与日志

`python3 tests/check_linux_l3.py --wire LOG` 串联检查 L0–L2 和 L3；
hosted 加 `--hosted`，禁用配置加 `--disabled`。
关键字：`linux L3 fd verified`、`ENOSYS=2 observed`、`CPL3 udp/echo/inherit verified`、
`socket/commit/pages reclaimed`、`exec CLOEXEC socket verified`、`linux L3 selftest: PASS`。
最后还必须有全量 `selftest result: PASS (0 failures)`。

首次 QEMU 新用例因漏启 RX worker 失败，修复为测试负责启停 worker；不调整 TCP/RTO。
实际验收矩阵见本文末尾，未运行的项目不会记 PASS。

### 本轮环境矩阵（2026-10-15）

| 环境 | 实测结果 | 日志（本次宿主 /tmp，非仓库永久产物） |
| --- | --- | --- |
| hosted，Linux 开 | 全量 PASS (0 failures)，L0–L2 与 L3 fd/cap/UDP/容量/回收 | `/tmp/linux-l3-hosted-release.log` |
| hosted，LINUX_ABI=0 | 全量 PASS (0 failures)，persona 跳过、原生保留 | `/tmp/linux-l3-hosted-off-final.log` |
| KVM host SMP4 单节点 e1000，Linux 开 | 全量 PASS (0 failures)，真实 CPL3 UDP/TCP/fork+exec | `/tmp/linux-l3-qemu-dual-single-final.log` |
| KVM host SMP4 双节点，virtio-net + e1000 | 全量 PASS (0 failures)，优先 virtio-net，L0–L3/NUMA/原生测试 | `/tmp/linux-l3-qemu-dual-on-final.log` |
| KVM SMP4，Linux 编译关闭 | 全量 PASS (0 failures) | `/tmp/linux-l3-qemu-compiled-off-final.log` |
| KVM SMP4，Linux 运行时关闭 | 全量 PASS (0 failures) | `/tmp/linux-l3-qemu-runtime-off-final.log` |
| 无 ISO 安装盘，AHCI/e1000，Linux 开 | 冷启动 UDP/TCP/HTTP/fork+exec 全部 exit=0；旧持久文件读回；服务端回应校验 | `/tmp/linux-l3-product-final.log`、`/tmp/linux-l3-product-final-client.log` |
| 同一安装盘，Linux 关 | Linux ELF 拒绝；文件保留；原生重定向/cat/httpd 两次响应、poweroff 正常 | `/tmp/linux-l3-product-off.log`、`/tmp/linux-l3-native-off-httpd.log` |
| QEMU 原生网络与 kill 回归 | nc 控制台 echo、http-get、httpd 两次响应；kill 等待 accept 的 Linux pid 后 reap、8082 重新监听成功 | `/tmp/linux-l3-product.log`、`/tmp/linux-l3-native-httpd-client.log`、`/tmp/linux-l3-server-after-kill.log` |
| VirtualBox 7.2.20，UEFI/AHCI/e1000 NAT，4 CPU，无 ISO | 产品 UDP lo/TCP echo/fork+exec/HTTP 客户及服务端全部 exit=0；冷启动持久文件、poweroff 正常；未跑 VBox 全量 debug | `/tmp/linux-l3-vbox-final.log`、`/tmp/linux-l3-vbox-client.log` |

VBox 首次启动横幅可能显示 link down；后台 RX 后续会更新 link。第一次用 `nonet`
探针时网卡已经 up，因其严格预期 ENETUNREACH 而 exit=12；这不是无网卡测试通过。
随后冷启动改跑真实 TCP 客户/服务路径，上表以最终日志为准。
QEMU 首次产品盘缺 lo 导致 UDP bind 失败；最终产品初始化已补，最终日志 UDP exit=0。
当前固定测试产物 SHA-256：
`6c301eb984b5129ce46d4e29a387da4996a826edd01d4e0f91f699cff4ae89b6`。

### 修改文件

- `abi/linux_socket.c`、`include/capmix/linux_socket.h`：socket 翻译与引用生命周期。
- `abi/linux.c`、`abi/linux_file.c`、`proc/syscall.c`：persona 分发与统一 fd I/O/close。
- `include/capmix/cap.h`、`cap/object.c`：追加独立 socket 对象类型。
- `include/capmix/linux_uapi.h`：固定 sockaddr 布局断言。
- `net/tcp.c`、`include/capmix/tcp.h`：本地端口绑定与端点快照；不改 RTO 状态机。
- `proc/init.c`：Linux 开启才补测试 ELF 与产品 lo 初始化。
- `boot/linux_l3_test.c`、`boot/kernel.c`：fd/cap/容量/回收与实际 CPL3 测试。
- `tests/linux_l3.c`、`tests/linux_l3_start.S`、`tests/linux_l3.ld`、`Makefile`：固定 Linux 静态 fixture 构建与嵌入。
- `tests/check_linux_l3.py`、`tests/check_linux_httpd.py`：日志链与真实 HTTP 响应断言。
- 本文、`docs/README.md`、`docs/KERNEL_OVERVIEW.zh-CN.md`、`docs/LINUX_ABI_L2.md`、
  `docs/VIRTUALBOX_TESTING.zh-CN.md`：当前能力、边界与实测环境。

### 无网卡附加回归的已知问题

显式 `-nic none`（否则 QEMU 会默认提供 e1000）下，L3 的 UDP lo 与
`linux L3 no NIC: ENETUNREACH verified`、`CPL3 nonet ... socket/commit/pages reclaimed`
均通过，L3 selftest 为 PASS；但全量是 **FAIL (1 failures)**，失败点是更早的
`linux L0 image/page-table reclaim mismatch`。两次重跑可复现。
保留的 L2 验收 ISO `/tmp/linux-l2-on-final.iso` 在同样 `-nic none` 下也报该断言，
对照日志 `/tmp/linux-l3-qemu-dual-l2-baseline-no-nic-final.log`。
因此没有把无网卡全量标 PASS，也没有在本轮修改 L0/内存核心来掩盖旧断言。
L3 当前日志为 `/tmp/linux-l3-qemu-dual-no-nic-final.log`。
