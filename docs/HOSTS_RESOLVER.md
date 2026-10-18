# Hosts-only IPv4 解析

这是用户库 stub，不是递归 DNS 解析器，也没有 UDP 53 客户端或 DNSSEC。
TCP/UDP 内核 ABI 仍接收数字地址；名称转换发生在调用网络 syscall 之前。
原生 ABI 与 Linux persona 使用相同解析代码，Linux 默认关闭的规则不变。

## 接口和规则

`sdk/include/cm_hosts.h` 提供 `cm_hosts_resolve(name, &ipv4, reader, context)`。
reader 返回实际字节数、0 表示 EOF、负数表示读取失败；打开/关闭文件和授权由调用者负责。
成功返回 0，输出是 `10.0.2.2 == 0x0a000202` 的整数；Linux sockaddr 需按网络字节顺序填入。
原生程序包含 `cm_resolve.h`，调用 `cm_resolve_ipv4()`；此适配器通过已有 FILE capability syscall
打开 `/etc/hosts`，每次解析后关闭 fd，不增加新的全局 DNS 权限或 syscall。
Linux 示例 `examples/linux-hosts-http.c` 使用 Linux open/read/close 和 socket ABI，复用解析器。
`make build/linux-hosts-http.elf` 可从源码复现（无 libc，沿用 Linux fixture crt/链接脚本并显式标记 persona）。
外部 Linux 程序需要链接/调用此库；本次不自动替换 musl/glibc 的 getaddrinfo，
不宣称任意现成 wget 已获得新解析能力。

查找顺序：逐行查 hosts 中的主名称或别名，首个匹配生效；未命中（或文件不存在）再解析严格四段十进制 IPv4。
名称匹配不区分 ASCII 大小写。支持空白、制表符、CRLF、`#` 注释和末尾无换行。
IPv6 与非法地址行忽略；不支持搜索域、尾点归一化、缓存和 IDN。
名称只接受 ASCII 字母、数字、点、连字符、下划线，最大 253 字节。
IPv4 每段最多三位且 ≤255；延续原生接口策略，拒绝 0.0.0.0 和 255.255.255.255 作为目标。

解析器无动态分配：行最多 1024 字节、扫描最多 16384 字节、读取块 128 字节。
找到匹配后立即返回，不检查文件剩余部分；尚未匹配即超限返回错误。
读取失败、权限拒绝、NUL 或超限不会伪装成成功或查不到。
没有缓存或并发编辑快照保证；修改 hosts 时应避免同时发起解析。

| 返回值 | 含义 |
|---|---|
| 0 | hosts 命中或 IPv4 字面量 |
| -1 | 无匹配且不是支持的 IPv4 字面量 |
| -2 | 非法名称、空指针输出或 hosts 含 NUL |
| -3 | 打开/读取/关闭失败或拒绝 |
| -4 | 行/扫描字节上限 |

这些是解析库状态，不是 Linux errno 或原生 syscall 错误编号。
`http-get.elf`、`nc.elf` 与 Linux 探针解析失败均打印 `resolve: ...`、退出 2，未发起 TCP 连接。

## 产品使用

启动只创建缺失的 `/etc/hosts`，不会覆盖盘上已存在文件。初始内容：

```text
127.0.0.1 localhost
10.0.2.2 host.capmix.test hostgateway
```

`10.0.2.2` 是本测试 QEMU/VBox NAT 的宿主访问地址，其他网络必须自行修改。
localhost 记录不代表新增了 TCP 本机回环能力。

宿主启动 HTTP：

```sh
mkdir -p /tmp/capmix-hosts-http
printf 'hosts stub body verified\n' > /tmp/capmix-hosts-http/hello.txt
python3 -m http.server 8000 --bind 127.0.0.1 --directory /tmp/capmix-hosts-http
```

产品 shell：

```text
run /bin/http-get.elf host.capmix.test 8000 /hello.txt
echo 10.0.2.2 demo.host alias.host > /etc/hosts
run /bin/http-get.elf alias.host 8000 /hello.txt
run /bin/http-get.elf 10.0.2.2 8000 /hello.txt
run /bin/http-get.elf no-record.invalid 8000
run /bin/linux-hosts-http.elf demo.host 8000
```

Linux 探针仅 Linux 开启时补建，固定 GET `/hello.txt`，响应上限 64KiB，验证状态 200；
宿主脚本进一步校验正文。不是完整 wget（无 TLS/重定向/HTTP 分块）。
旧安装盘不会自动更新已有工具；用最新内核安装新的测试盘，或停机后明确更新对应 ELF。

## 自测和环境

`boot/hosts_test.c` 加入 hosted/freestanding 全量测试：别名、大小写、CRLF、1–128 字节分片、
末尾无换行、数字回退、缺失名称、错误输入、I/O 失败、行/文件上限。
日志：`hosts stub: ... PASS (0 failures); DNS network=disabled`。

产品回归脚本（会覆盖**测试盘** `/etc/hosts`，只用一次性新安装盘）：

```sh
mkdir -p /tmp/capmix-hosts-lab
make install-disk DISK_IMAGE=/tmp/capmix-hosts-lab/system.img INSTALL_LINUX=1
touch /tmp/capmix-hosts-lab/.capmix-hosts
python3 tests/check_hosts_product.py --backend qemu --lab /tmp/capmix-hosts-lab
# VBox 使用另一个新 lab，命令相同，将 backend 改为 vbox。
```

脚本分别运行两次无 ISO 冷启动，验证原生和 Linux 的名称、别名、数字 HTTP 正文，
未知名称 exit=2，并确认 hosts 重启后保留。VBox 为 UEFI/AHCI/e1000 NAT；QEMU 为 KVM/AHCI/e1000 usernet。
VBox 必须启用 `VBoxManage modifyvm VM --nat-localhostreachable1 on` 才能访问绑定于宿主 127.0.0.1 的服务；测试脚本已设置。
不同时运行 VBox 与 KVM。环境测试结果见下表。

### 2026-10-18 实测矩阵

| 环境 | 结果与证据 |
|---|---|
| hosted，Linux 开/关 | 全量 `PASS (0 failures)`；[开启](../tests/evidence/logs/hosts-hosted-on.log)、[关闭](../tests/evidence/logs/hosts-hosted-off.log) |
| QEMU 8.2.2 KVM，4 CPU，Linux+interp 开，无 NIC | 全量 `PASS (0 failures)`；[日志](../tests/evidence/logs/hosts-qemu-debug-on.log)、[黄金用例链检查](../tests/evidence/logs/hosts-qemu-debug-on-check.log)。网络 wire 项明确 skip，网络实际收发由下一行覆盖 |
| QEMU KVM，4 CPU，AHCI/e1000，Linux 开，产品盘无 ISO | 两次冷启动 PASS；[结果](../tests/evidence/logs/hosts-qemu-result.log)、[首次](../tests/evidence/logs/hosts-qemu-first.log)、[冷启动](../tests/evidence/logs/hosts-qemu-cold.log) |
| VBox 7.2.20，UEFI，4 CPU，AHCI/e1000 NAT，Linux 开，产品盘无 ISO | 两次冷启动 PASS；[结果](../tests/evidence/logs/hosts-vbox-result.log)、[首次](../tests/evidence/logs/hosts-vbox-first.log)、[冷启动](../tests/evidence/logs/hosts-vbox-cold.log) |
| QEMU KVM，4 CPU，Linux 关，无 NIC | 全量 `PASS (0 failures)`：[日志](../tests/evidence/logs/hosts-qemu-debug-off.log)、[检查](../tests/evidence/logs/hosts-qemu-debug-off-check.log) |
| 默认产品 ISO，Linux 默认关 | [提示符烟测](../tests/evidence/logs/hosts-product-iso-boot.log)，进入 shell 后闲置超时；不当作全量 selftest |

保留的联调失败：

- [Linux 初版探针被拒绝](../tests/evidence/logs/hosts-fixture-empty-load.log)：没有可写全局对象导致链接脚本产生空的 RW LOAD，
  触发现有 ELF 验证；将探针接收缓冲放入 BSS 后重建，未放松内核 ELF 检查。
- [首轮日志断言失败](../tests/evidence/logs/hosts-split-error-assert.log)、[对应 guest](../tests/evidence/logs/hosts-split-error-guest.log)：
  原生错误信息分三次 write，被日志前缀拆开；改为一次完整 write，仍明确 exit=2。
- [VBox 首轮超时](../tests/evidence/logs/hosts-vbox-localhost-failure.log)、[对应 guest](../tests/evidence/logs/hosts-vbox-localhost-guest.log)：
  hosts 命中后 TCP timeout；测试 VM 缺少 NAT localhostreachable。显式启用后重新完整通过，未改网络核心。

产品检查不是全量 selftest；仅上表标为全量并确有 `selftest result` 的日志代表全量结果。

构建：`make kernel`、`make iso` 成功，无新增 freestanding warning/error。hosted 保留原有 arch 未用符号警告。
