# 用户态示例：hello、nc、cat、ls、http-get

五个程序均由宿主 GCC 使用 `sdk/crt0.S`、`sdk/capmix.ld` 和
`sdk/include/capmix.h` 构建为静态 ET_EXEC，进入 CPL=3；不链接 Linux libc。
它们调用普通用户 syscall，不借用内核自测入口。

## 构建、装盘和无 ISO 运行

仓库根一行构建：

```bash
make examples iso
```

独立 ELF 在 `build/example-{hello,nc,cat,ls,http-get}.elf`。ISO 内核携带这些初始文件；
产品启动仅在路径不存在时通过 VFS 写入并 fsync。路径为：
`/bin/hello-sdk.elf`、`/bin/nc.elf`、`/bin/cat.elf`、`/bin/ls.elf`、`/bin/http-get.elf`。
原 `/bin/hello.elf` 测试镜像保持原有用途。另提供 `/example.txt` 文本。

新盘一行安装（目标必须不存在或是符合安装器条件的全零普通 RAW 文件）：

```bash
make install-disk DISK_IMAGE=/tmp/capmix-examples.img
```

沿用 [INSTALL_DISK.md](INSTALL_DISK.md) 的 BIOS/UEFI 引导和 COW 根布局；
第一次**无 ISO**从盘启动会初始化空根并写入系统 ELF 和上述示例，正常关机后
再次冷启动仍从同一盘读取。未知非空盘、已安装盘不会被安装器重新格式化。
系统 provisioning 不覆盖同名文件，因而这不是旧内核/旧 shell 的自动升级器。

AHCI 无 ISO 启动示例：

```bash
qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -drive id=disk,file=/tmp/capmix-examples.img,if=none,format=raw \
  -device ich9-ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial stdio -display gtk -no-reboot
```

也可用 `-drive file=...,if=virtio,format=raw` 和 virtio-net。
键盘从 QEMU 图形窗口输入，serial 镜像输出；serial stdin 不是输入终端。

```text
run /bin/hello-sdk.elf first second
run /bin/ls.elf /bin
run /bin/cat.elf /example.txt
run -i /bin/nc.elf 10.0.2.2 9998
```

nc 运行前，在宿主另一个终端启动 `python3 scripts/tcp-echo.py`。
QEMU user-net 的 `10.0.2.2:9998` 可达这个宿主 echo 服务。输入一行文字，
回显之外应再收到 echo，出现 `nc tx bytes=N` 和 `nc rx bytes=N`。
输入 `/quit` 主动关闭，shell 显示 `exit=0 reap=ok`。
最后 `poweroff`，再启动同一个盘，可重复运行以上程序。

对于**已使用本版内核与 shell**、已初始化且 VM 关闭的安装盘，也可以离线导入
不同的新路径（拒绝覆盖，父目录须存在）：

```bash
make examples build/cow-put
python3 scripts/copy-to-disk.py /path/system.img build/example-cat.elf /bin/mycat.elf
```

nc/ls/hello 同理。普通文本也可用同一脚本导入 `/mytext.txt` 再 `run /bin/cat.elf /mytext.txt`。
离线导入的锁、staging、重挂校验等边界见 [EXTERNAL_ELF.md](EXTERNAL_ELF.md)。

## 控制台授权与 run -i

普通 `run` 或 `run ... &` 不赋予键盘/原始控制台 capability；fd1/fd2 仍可诊断输出。
`run -i PATH ARGS...` 显式委托 console READ|WRITE 至子进程 handle **1**，
仅接受前台；`run -i ... &` 拒绝。它不委托 CONTROL、BOOT 或 DUPLICATE 权限。
内核要求父进程持有 console READ|WRITE|DUPLICATE。shell 自身保留派生权限，
受委托子程序不能再转授。交互子进程固定 BSP，保证授权完成后才执行；
shell 等待时不读取键盘，避免抢走 nc 输入。普通后台程序保留原并发语义。

spawn ABI 原 32B request 不变，flags 新增 `CM_SPAWN_CONSOLE=2`，与
`CM_SPAWN_SHELL=1` 互斥。console syscall 31 READ、32 WRITE 的 ABI 不变，
子程序使用 handle1；SDK `cm_console_*` 包装这些调用。READ 非阻塞，空队列 -5；
WRITE 返回写入字节数，暂忙 -5；普通 ELF 尝试访问返回 -3（DENIED）。
不通过 fd0 提供 stdin，不实现完整终端行规程、信号或作业控制。

## 各程序语义和限制

| 程序 | 调用与行为 | 退出状态 |
| --- | --- | --- |
| hello-sdk | main 接收 argc/argv/envp，打印 argv；支持普通前后台 run | 0 成功；非零为参数/输出失败 |
| ls | syscall35 READDIR，缺省 `/`，最多256项；目录名后 `/` | 0 成功，1 查询失败，2 用法，3 超限 |
| cat | OPEN/READ/CLOSE 的 FILE/fdio 路径，127B 分块显示；最多1MiB | 0 EOF成功，1 I/O失败，2 用法，3 达显示上限 |
| nc | TCP OPEN/CONNECT/SEND/RECV/CLOSE；控制台输入按行发送 | 0 正常关闭，1 网络/控制台失败，2 用法，3 缺控制台授权，124 空闲超时 |
| http-get | HTTP/1.0 GET，打印状态行及前1024B正文，读完有界响应再关闭 | 0 为2xx，1 网络/协议/大小错误，2 用法，4 为3xx–5xx，124 接收超时 |

cat 是文本查看器：保留 ASCII、换行和 tab，其他字节显示 `.`，不保证二进制原样输出。
诊断 stdout 每次最多127B，可能带 `user[pid]:` 日志前缀。ls/cat 只读，不改变文件。
nc 接受数字 IPv4 与1～65535端口；每行最多254个可打印 ASCII 字符，随后发送换行；
超长行额外字符忽略。网络接收每次256B，可能分多次显示，日志计数不能当报文边界。
`/quit` 是本地命令，不发送给对端；无输入/无网络数据30秒退出124，避免永久挂前台。
CONNECT/SEND/CLOSE 各有3000ms等待限制，RECV 非阻塞并主动 yield；没有 listen、
DNS、TLS、管道、重定向或 nc 的完整选项集。底层沿用现有 TCP 的 MSS、窗口、RTO
限制，见 [P1D_TCP.md](P1D_TCP.md) 和 [TCP_RTO.md](TCP_RTO.md)。

无网卡：hello/cat/ls 正常使用根与诊断输出；交互 nc 的 connect 返回现有负错误码，
程序记录 `nc connect error=...` 后 exit1，控制台授权会随 reap 释放。
普通 `run nc...` 总是先拒绝控制台访问并 exit3，不会为了失败测试创建 TCP 连接。
无 echo 服务则连接超时或拒绝，记录失败并回收，不影响 UDP/DHCP。
EOF 或 `/quit` 后调用 CLOSE，退出后 shell 的 CHILD_WAIT 消耗 child handle 并 reclaim。

## 验收关键词

- `hello from external gcc static ELF`、`argv[0]=/bin/hello-sdk.elf`、参数值。
- `ls entries=N`、`cat bytes read=N`，冷启动后仍能读出同一盘的内容。
- `user console delegated`、`nc connected`、`nc tx bytes=N`、`nc rx bytes=N`、`nc closed`。
- 普通 run nc：`console denied`、`exit=3 reap=ok`。
- 自测：`user console delegation verified: granted=READ/WRITE denied=no-duplicate no-control/boot reap=no-leak`。
- debug 完整通过标志仍为 `selftest result: PASS (0 failures)`；产品启动不自动跑完整自测。

## 最小 HTTP GET

构建仍是一行 `make examples iso`，产品新盘自动 provision `/bin/http-get.elf`。
已有安装盘无需重装内核：关闭 VM 后可只导入这个新文件（不覆盖已有路径）：

```bash
make examples build/cow-put
python3 scripts/copy-to-disk.py /path/system.img build/example-http-get.elf /bin/http-get.elf
```

宿主准备一份文本并启动 HTTP 服务：

```bash
mkdir -p /tmp/capmix-www
printf 'hello from HTTP server\n' > /tmp/capmix-www/hello.txt
python3 -m http.server 8000 --bind 127.0.0.1 --directory /tmp/capmix-www
```

QEMU 使用上文 user-net + e1000 或 virtio-net；在产品 shell（也可无 ISO 从盘启动）：

```text
run /bin/http-get.elf 10.0.2.2 8000 /hello.txt
```

无需 `run -i` 或额外 console capability；它使用普通 ELF 的网络句柄和诊断 stdout。
可观察 `http-get status: HTTP/1.0 200 OK`、文本、`http-get body bytes=N`、
`http-get shown bytes=N`、`http-get exit=0`、shell 的 `exit=0 reap=ok`。
路径省略时 GET `/`。404 也显示状态和部分正文，但退出4；不会误报为成功。

协议与资源边界：

- 只接受数字 IPv4、必填端口1–65535、以 `/` 开头的 origin-form path，最多255B；
  不解析完整 URL，不做 DNS/TLS/代理、认证或任何自动重定向。path 禁止空格、控制字符、
  非 ASCII、`#`，不做自动 URL 编码；可自行传已编码路径或 query。
- 发送 `GET ... HTTP/1.0`、`Host: IPv4:PORT`、`Connection: close`。
  接受 HTTP/1.0 或 HTTP/1.1 的单个最终响应；1xx、transfer-encoding（含 chunked）
  拒绝，不解压 gzip，不支持连接复用。
- 流式解析，不假设 TCP recv 对齐行或 header/body 边界；要求 CRLF，状态/每行头内容
  最多254B，总头部最多8192B。Content-Length 必须是有界十进制；不同的重复值、
  与实际正文长度不符、截断头/正文均拒绝。无 Content-Length 时以 EOF 定界。
- 最多接收1MiB正文，始终只显示前1024B；其余继续 drain 至 EOF 以正常收尾。
  超限失败，不是无限下载器。正文输出保留 ASCII、换行和tab，其他字节显示 `.`；
  沿用 fd1 的日志前缀，不是二进制下载或保存文件工具。
- CONNECT/SEND/CLOSE 各沿用 SDK 的3000ms等待；接收非阻塞，AGAIN 时 yield，
  接收阶段总计10秒、空闲3秒超时。失败仍 CLOSE，退出由 shell wait/reap；
  无网卡/不可达服务记录负错误，不改变 UDP/DHCP/TCP 核心。

本轮实际验证：无 ISO virtio 磁盘 + virtio-net，Python http.server 返回200，
54B正文完整显示；5124B正文仅显示1024B但完整接收，均 exit0；404 exit4；
非法 path exit2。日志 `/tmp/httpget-product.log`，hosted 与 QEMU debug 全量 PASS
分别见 `/tmp/httpget-hosted.log`、`/tmp/httpget-debug.log`。

## 标准I/O重定向续作

前台shell已支持 `>`、`>>`、`<`，如 `run /bin/ls.elf /bin > /listing.txt`、
`run /bin/cat.elf < /example.txt > /copy.txt`。cat可省略路径读fd0；SDK的
`cm_report`摘要走fd2，其余 `cm_print/cm_output`走可重定向fd1。
`run -i`及后台`&`不与重定向混用；nc仍用交互方式，无新增batch/pipe语义。
覆盖/追加、权限、失败副作用、冷启动和QEMU/VBox验收见 [SHELL.md](SHELL.md)。
