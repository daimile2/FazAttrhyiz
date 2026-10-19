# httpd：有界多进程示例与SMP测量

2026-10-19。使用同一份原生 `/bin/httpd.elf`，默认Linux关；不改内核ABI、权限、调度、TCP/RTO或IRQ。
本轮选择**shell启动的独立监听多进程模型**：每个进程一个监听端口，独占accept/服务/关闭自己的连接。
不是父进程accept后传递cap，也不是多个进程共享同一监听socket；没有新增单端口前端/负载均衡器。

原因：普通原生ELF没有管理spawn的CONTROL capability，现有spawn也不交接accepted TCP cap。
不为一个示例放宽授权或加入线程运行时；shell已有并发spawn/reap能力可直接用于进程工作者。
基准客户端显式对连续端口均衡发请求。需要单个公开端口时，须由外部前端处理，属于后续工作。

## 同一二进制与有界行为

原用法保留：`run /bin/httpd.elf [PORT [REQUESTS]] [&]`。默认单监听串行，既有HTTP200/25B正文不变。
新增可选标签：`run /bin/httpd.elf PORT REQUESTS WORKER_ID &`，WORKER_ID为1..4，REQUESTS为1..100。
WORKER_ID用于日志身份，不是CPU编号或全内核线程池限制；同一标签可由另一次独立启动重复使用。
本轮编排器明确只启动1/2/4个实例，进程总量/后台job/TCP池仍受原有全局上限约束。

例如四个实例，每个服务17个请求，共68个：

```text
run /bin/httpd.elf 8080 17 1 &
run /bin/httpd.elf 8081 17 2 &
run /bin/httpd.elf 8082 17 3 &
run /bin/httpd.elf 8083 17 4 &
```

每进程监听backlog=4，accept非阻塞，空队列yield；请求头最大2048B/3秒，GET最小校验，
发送/关闭沿用原有有界TCP API。没有无界工作队列、用户线程或跨进程共享连接。
配额结束关闭listener后退出；每个已accept连接都走关闭路径，任何请求/关闭失败使该实例最终非零退出，
不会让后续成功请求覆盖先前失败。异常终止沿用现有进程cap清理，shell kill/poweroff语义不变。
未给REQUESTS的旧模式仍可一直等待客户端，需kill/poweroff结束；本轮worker示例使用明确配额。

新增完整行：

```text
httpd worker ready: id=1 port=8080 handled=0 status=0
httpd worker done: id=1 port=8080 handled=17 status=0
```

相同ELF编入内核初始文件及新安装盘；旧已安装的同名文件不被静默覆盖，更新旧盘应显式更新文件或新建演示盘。

## 转发与复现

QEMU netdev可指定四条规则，guest设备仍为e1000：

```sh
-netdev user,id=n0,hostfwd=tcp:127.0.0.1:18100-:8080,hostfwd=tcp:127.0.0.1:18101-:8081,hostfwd=tcp:127.0.0.1:18102-:8082,hostfwd=tcp:127.0.0.1:18103-:8083 \
-device e1000,netdev=n0
```

VBox在关机状态分别添加 `--natpf1 'worker0,tcp,127.0.0.1,18100,10.0.2.15,8080'`，
后续三条依次worker1..3/18101..18103/8081..8083。是四个公开测试端口，不是一个端口的共享accept。
`perf-baseline.py`会自动创建规则/新盘并启动/回收实例，不需要手动编排：

```sh
# 两组都用4个并发客户端；只改变服务进程数
python3 scripts/perf-baseline.py --backend qemu --http-only --workers 4 --concurrency 4 \
  --cpus 1,4 --lab /tmp/http-qemu-w4 --port 18100
python3 scripts/perf-baseline.py --backend qemu --http-only --workers 1 --concurrency 4 \
  --cpus 1,4 --lab /tmp/http-qemu-w1 --port 18100
python3 tests/check_http_workers.py /tmp/http-qemu-w1 /tmp/http-qemu-w4
# QEMU完全退出后，再用VBox；需要ICH9受限INTx已验证配置
python3 scripts/perf-baseline.py --backend vbox --chipset ich9 --http-only --workers 4 --concurrency 4 \
  --cpus 1,4 --lab /tmp/http-vbox-w4 --port 18100
python3 scripts/perf-baseline.py --backend vbox --chipset ich9 --http-only --workers 1 --concurrency 4 \
  --cpus 1,4 --lab /tmp/http-vbox-w1 --port 18100
python3 tests/check_http_workers.py /tmp/http-vbox-w1 /tmp/http-vbox-w4
```

新目录/盘，AHCI根，无ISO运行，512MiB，Linux关，e1000；每种CPU/进程数先预热8次，再3批×20请求。
每批请求均衡分配到所有worker端口；数量须被worker数整除。默认workers=1/concurrency=1，历史串行客户端模式仍可用。
客户端用固定大小线程执行器，记录实际client_peak（本轮均为4），无失败重试；每次核对状态、25B正文及EOF。
HTTP耗时包括连接、服务与关闭；每请求延迟从客户端线程开始该请求计，不含等待executor空位的时间。
整批吞吐计入线程启动/排队/关闭的全部墙钟时间。p50/p95算法沿用perf-baseline，表中取三批统计量中位数。

每个worker必须打印正确配额和status=0，shell必须出现对应PID exit=0/reap=ok；未回收或配额不符则失败。
记录worker→PID/CPU/port到 `smpN-workers.json` 和每条样本；四核/四worker必须跨CPU。
不要求任何速度提升才能PASS，功能/配额/回收不允许为了数字而跳过失败。

## QEMU实测（2026-10-19）

QEMU 8.2.2/KVM、host CPU、i440FX e1000 INTx、AHCI、无ISO、Linux关；两组使用同内核。

| 服务进程 | vCPU | req/s中位 | p50 ms | p95 ms | 实际worker CPU |
|---|---:|---:|---:|---:|---|
| 1 | 1 | 8.083 | 122.725 | 2335.963 | 0 |
| 1 | 4 | 8.104 | 123.670 | 2337.082 | 0 |
| 4 | 1 | 26.245 | 143.900 | 186.640 | 0/0/0/0 |
| 4 | 4 | 31.429 | 94.963 | 297.957 | 0/1/2/3 |

四进程1→4 vCPU吞吐 **+19.76%**；单进程仅+0.26%。
四进程相对单进程的收益包含并行等待网络/关闭，不能把它全称为多核算力收益。
四进程的p95在四核反而升高，小样本与共享TCP/RX/串口/宿主调度仍有限制，不能宣称延迟全面改善。

[QEMU对照核验](../tests/evidence/logs/httpworkers-qemu-compare.log)、
[四worker原始JSON](../tests/evidence/logs/httpworkers-qemu-w4-results.log)、
[单worker原始JSON](../tests/evidence/logs/httpworkers-qemu-w1-results.log)、
[四核worker串口](../tests/evidence/logs/httpworkers-qemu-w4-smp4.log)。

## VBox ICH9实测（2026-10-19）

VirtualBox 7.2.20、UEFI/ICH9、AHCI、82540EM INTx GSI19、NAT四端口、512MiB、无ISO、Linux关。
1/4worker两组均为相同内核/4并发客户端/相同请求数；未将先前串行客户端的IRQ A/B数字直接拿来混比。

| 服务进程 | vCPU | req/s中位 | p50 ms | p95 ms | 实际worker CPU |
|---|---:|---:|---:|---:|---|
| 1 | 1 | 7.936 | 126.966 | 2393.530 | 0 |
| 1 | 4 | 8.614 | 116.213 | 2204.538 | 0 |
| 4 | 1 | 26.505 | 149.646 | 166.567 | 0/0/0/0 |
| 4 | 4 | 29.974 | 92.878 | 299.642 | 0/1/2/3 |

四进程1→4 vCPU吞吐 **+13.09%**；单进程+8.54%。四进程p95同样未随CPU数改善。
单进程/并发客户端下的长尾包含排队、连接握手及关闭；本轮不修改RTO/拥塞/监听队列算法以“美化”数字。

[VBox对照核验](../tests/evidence/logs/httpworkers-vbox-compare.log)、
[四worker原始JSON](../tests/evidence/logs/httpworkers-vbox-w4-results.log)、
[单worker原始JSON](../tests/evidence/logs/httpworkers-vbox-w1-results.log)、
[四核worker串口](../tests/evidence/logs/httpworkers-vbox-w4-smp4.log)。

所有四组性能测量使用同一内核：
`d317253b81058386486b540d736d8e91ce6f3c51de11ac989dc153478bc6945d`。
吞吐只是1与4CPU的两点对照，不是完整CPU数曲线，也不是任意应用/负载下的扩展保证。
本轮不是通用HTTP服务器、TLS、线程池产品或单端口负载均衡实现。

## 验收矩阵

| 环境 | 实际范围 | 证据 |
|---|---|---|
| QEMU KVM 1/4CPU，1/4进程，4客户端 | HTTP200/25B正文/EOF/配额、跨CPU配置与全部worker exit=0/reap=ok通过 | [1进程检查](../tests/evidence/logs/httpworkers-qemu-w1-check.log)、[4进程检查](../tests/evidence/logs/httpworkers-qemu-w4-check.log) |
| VBox ICH9 1/4CPU，1/4进程，4客户端 | 同上，另有实际IRQ RX帧证据，所有四次性能VM正常关机 | [1进程检查](../tests/evidence/logs/httpworkers-vbox-w1-check.log)、[4进程检查](../tests/evidence/logs/httpworkers-vbox-w4-check.log) |
| QEMU KVM 4CPU，2进程，2客户端，4请求单样本 | 仅功能补测，非正式吞吐对照；双PID/CPU、配额、回收通过 | [检查](../tests/evidence/logs/httpworkers-qemu-w2-check.log) |

编译：[kernel](../tests/evidence/logs/httpworkers-kernel.log)，无新增warning/error。
性能PASS与全量selftest是不同验收口径。

补充回归：[QEMU旧CLI完整旅程](../tests/evidence/logs/httpworkers-journey-check.log)为三次无ISO冷启动PASS，
包含旧 `httpd PORT REQUESTS`、HTTP正文、文件重定向、MicroPython开/关及ACPI关机。
[hosted](../tests/evidence/logs/httpworkers-hosted.log)、[QEMU四核debug](../tests/evidence/logs/httpworkers-debug.log)、
[全量断言链](../tests/evidence/logs/httpworkers-debug-check.log)为0 failures（debug无NIC wire明确skip，真实HTTP由产品基准/旅程验证）。
[旧IRQ A/B格式复算](../tests/evidence/logs/httpworkers-legacy-check.log)仍通过。
[默认ISO构建](../tests/evidence/logs/httpworkers-product-iso.log)成功，
[默认ISO启动](../tests/evidence/logs/httpworkers-product.log)到shell ready；空闲timeout=124仅为启动检查。
