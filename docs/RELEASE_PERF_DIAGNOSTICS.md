# 发布前：HTTP 延迟分解、hush 放置对照与 ENOSYS

2026-10-19。使用真实安装盘、PS/2 shell、e1000/NAT；QEMU/KVM 与 VirtualBox 顺序运行，保持内核哈希一致。固定192000B管道负载，比较结果由独立用户ELF读取文件校验，不以exit0替代内容正确。

## HTTP：约117ms发生在哪里

本轮在客户端对每个请求分别记录 connect、send、首字节等待、接收剩余正文、正文完成到EOF/本地socket关闭。逐请求五段之和必须等于总时间；各段的中位数不保证相加等于总时间中位数。NAT hostfwd 的本地 connect 完成不代表 guest 三次握手已完成。

同时给原生 httpd 加了可选用户态计时：接受连接后读取完整请求、发送并等待ACK、close。仅用诊断参数 `httpd PORT REQUESTS WORKER_ID 1` 开启；普通用法保持原状。没有修改TCP状态机、RTO或100ms参数。

第一组QEMU单核实测：连续请求p50 **117.768ms**，首字节等待中位 **113.661ms**；connect约0.075ms、发送约0.005ms、余下正文约0.012ms、EOF/关闭约3.243ms。guest读请求中位31.5µs、发送/等ACK 2308µs、close **107810µs**（guest统计含8次预热及两组请求）。

关键调用链是 [net/tcp.c](../net/tcp.c) 的 `cm_tcp_close`：进入TIME_WAIT后同步等待100ms；单工作者httpd要等这个调用返回，才继续accept。客户端已收到上一连接的正文和FIN/EOF，可以立即发下一请求，因此这段等待主要落在**下一请求首字节**，不能把guest close时长再加到同一请求的客户端EOF段。

对照实验在每次请求开始前等待130ms，等待不计入该请求延迟，但计入整个batch时长。QEMU单核p50降至 **19.535ms**，差值约 **98.233ms**；VBox单核连续请求 **130.857ms**，间隔后 **22.963ms**，guest close中位 **107.732ms**。这是对固定关闭等待的直接对照。正文/状态/EOF仍全部校验，未丢弃失败请求。

余下时间分别保留为实际测到的NAT转发、guest握手/accept与调度、收尾开销。没有把全部残余算为网卡轮询，也没有把100ms认作RTO重传。该实验用新连接/HTTP1.0关闭语义，不代表keep-alive或批量TCP吞吐。

**发布边界：同步close的100ms等待仍存在，现作为已量化限制记录。** 后续可做异步TIME_WAIT保留与重复FIN处理；本轮不直接删等待来刷请求率。

## hush：历史2.77×和1.33×的口径

历史脚本按shell回车到重定向完成的宿主wall time计时。它对每次hush运行和校验器都使用默认spawn，`proc/control.c` 中 `cpu_next` 递增；4CPU时，测量hush常落在CPU1/CPU3，而1CPU时只能落在CPU0。日志显示同一管道的fork子进程继承父进程CPU，未跨CPU并行执行管道。因此历史表格混合了CPU放置、内核后台工作、虚拟设备/日志与宿主计时开销，不能把两比值作为hush跨核扩展性或两个虚拟化器的通用性能结论。

本轮新增独立Linux诊断包装器 `tests/perf_hush_timer.c`：

- 记录guest TSC，使用实际PIT标定的TSC频率换算；不假定1GHz。
- 包含包装器fork、exec固定hush、wait回收；不包含shell加载包装器及最后的重定向FSYNC。
- 自动放置和显式sched_setaffinity到CPU0分别记录实际CPU；每次fork仍继承亲和。
- 用只执行`exit 0`的空脚本测包装器/启动基线，再运行32×6000B管道；每次输出均独立读取校验。
- 宿主回车计时仍保留，其与guest计时的差额包括CLI/监视器回车、加载、最终sync/回收和5ms检查采样，不把差额全称为CLI时间。

QEMU同一4CPU guest的已测样本：自动落CPU1的guest时间 **2.612s**，自动落CPU0的样本 **7.297s**；显式CPU0的两次中位 **5.959s**。1CPU显式CPU0中位 **7.455s**。显式CPU0对照约 **1.25×**，与把CPU0和AP混在一起得到的旧2.77×口径不同。QEMU宿主计时比guest计时多约几十毫秒，不能解释数秒的主要差异。

另提供无虚拟显卡/GOP的对照（不改console代码），保持串口与键盘注入。它量出移除显示设备和framebuffer日志路径的整体效应；PCI位置也可能改变，必须核对实际IRQ路由，不能把全部差值精确称为字形绘制耗时。该对照只有一个计量样本/CPU，用于机制检查，不用于统计加速承诺。

原始按实际CPU分组的数据与空脚本开销在矩阵中单列。**历史2.77×/1.33×保留为历史wall-time观测，撤回作为SMP扩展性结论的用法。** 本轮不从新版本反算旧版本每个内部函数的周期，也不把平台特有guest成本差异宣传成固定比例。

## 测量时发现的fork失败

压力轮捕获一次hush `fork: Resource temporarily unavailable`，继而Broken pipe。hush仍退出0，独立比较器发现不足192000B，整轮判失败。失败日志保留，未重试后只留下成功数值。

原始故障没有区分门闩忙/进程槽耗尽的分支计数，不能反向证明那一次EAGAIN的唯一来源；该轮只作为失败证据。核对 [proc/spawn.c](../proc/spawn.c)，生命周期门闩忙原先可立即返回EAGAIN；此分支不同于实际进程槽耗尽。现在仅对门闩争用做最多64次、按TSC检查100ms截止的让出CPU后重试；调度可能使截止检查晚到，不能宣称硬实时100ms。失败获取后已恢复IRQ状态，整个yield不持门闩；进入后仍只调用一次fork，保留槽耗尽/内存不足等真实错误，不改COW/commit或发布回收纪律。hosted不执行ring3调度重试。重跑仍保留独立字节校验；不能据一次通过宣称所有资源EAGAIN都消失。

## 干净构建与ENOSYS出口

`arch/x86_64/ioapic.c` 的 `g_ioapic_phys` 仅在freestanding分支使用，现将声明放入同一编译条件，未改IOAPIC路由或IRQ算法。新构建日志应无warning/error，旧警告日志保留作历史记录。

- 每进程ENOSYS文本从8条增加到 **32条**，定义在 `include/capmix/abi.h`。
- 直方图原先已经按nr统计 **0..511**，超出范围单列overflow；计数始终不受文本预算限制。没有宣称overflow桶能恢复每个大于511的原始号。
- 实际 `cm_poweroff()` 调用 `cm_linux_enosys_dump("poweroff", NULL)`，在ACPI或安全hlt前打印带nr/count的全局累计值；零计数不刷空行。已有poweroff capability授权不变，没有新增无授权全局查询开关。
- 强制关机若尚有活线程，逐桶读取是尽力而为，不是冻结全部CPU的事务快照。
- 机制测试同一nr=511调用35次，文本预算为32，直方图仍为35；另验overflow不丢计数。`tests/check_release_observation.py`检查文本条数、完整nr桶和hosted实际poweroff打印。

## 可复现命令

实验目录必须不存在，脚本创建一次性安装盘，不碰用户磁盘。诊断只支持单工作者/单客户，避免混淆前一连接关闭等待。FB配置每CPU两组×8请求，另8预热；hush两计量样本和一预热。所有样本与失败原样保留。

```sh
python3 scripts/perf-baseline.py --lab /tmp/faz-diag-qemu --backend qemu \
  --cpus 1,4 --samples 2 --requests 8 --pipes 32 --diagnose
python3 tests/check_perf_baseline.py /tmp/faz-diag-qemu
python3 tests/check_perf_investigation.py /tmp/faz-diag-qemu

# 等QEMU退出后再跑VBox；PIIX3与历史hush配置对应
python3 scripts/perf-baseline.py --lab /tmp/faz-diag-vbox --backend vbox \
  --cpus 1,4 --samples 2 --requests 8 --pipes 32 --diagnose
python3 tests/check_perf_investigation.py /tmp/faz-diag-vbox

# 无显卡对照：另建目录，每CPU一个计量样本
python3 scripts/perf-baseline.py --lab /tmp/faz-diag-qemu-serial --backend qemu \
  --cpus 1,4 --samples 1 --requests 4 --pipes 32 --diagnose --serial-only
# VBox对应命令将backend换为vbox，目录也换新；两种虚拟化不得同时运行
```

`diagnosis.json`保留每请求的五段时间、全部计量样本、CPU与guest/host时间；`results.json`保留原口径。内核哈希、虚拟化版本、标定频率、GPU和网卡IRQ状态均可从配置与串口证据核对。VBox新脚本按最多4字符批量注入扫描码，回车仍单独计时，不改变计时起点。

## 完整实测表（2026-10-19 UTC）

FB组每CPU两计量样本，串口组每CPU一个计量样本；全部使用同一内核ELF哈希 `34ba2a780d8bae3c0ceb1be0b4b90e9b0579f7b92654f7de2245b39007a2b065`。VBox FB主轮逐字符注入，后续无显卡轮改为最多4字符批量注入；输入文字均在计时前，回车仍单独计时。单样本GPU对照不能提供置信区间。

| 配置 | CPU数 | 请求数/条件 | 连续p50 ms | 间隔130ms p50 ms | guest close中位 ms |
|---|---:|---:|---:|---:|---:|
| qemu-fb | 1 | 16 | 117.768 | 19.535 | 107.810 |
| qemu-fb | 4 | 16 | 118.965 | 9.926 | 106.966 |
| qemu-serial | 1 | 4 | 115.167 | 6.814 | 105.800 |
| qemu-serial | 4 | 4 | 120.371 | 13.835 | 106.284 |
| vbox-fb | 1 | 16 | 130.857 | 22.963 | 107.731 |
| vbox-fb | 4 | 16 | 119.992 | 10.967 | 105.978 |
| vbox-serial | 1 | 4 | 119.305 | 9.172 | 104.254 |
| vbox-serial | 4 | 4 | 115.362 | 9.240 | 104.785 |

原口径速度比（4CPU/1CPU）与显式CPU0 guest耗时比：

| 配置 | 原wall吞吐比 | CPU0 guest时间：1CPU/4CPU | CPU0 1CPU秒 | CPU0 4CPU秒 |
|---|---:|---:|---:|---:|
| qemu-fb | 2.477× | 1.251× | 7.455 | 5.959 |
| qemu-serial | 3.129× | 0.935× | 5.967 | 6.380 |
| vbox-fb | 1.299× | 0.825× | 7.082 | 8.587 |
| vbox-serial | 1.258× | 0.831× | 4.058 | 4.886 |

按实际CPU拆开guest与宿主附加时间（不能把不同CPU混成一条“加速”）：

| 配置 | CPU数 | 放置方式 | 实际CPU | 样本数 | guest秒中位 | 宿主额外ms中位 |
|---|---:|---|---:|---:|---:|---:|
| qemu-fb | 1 | auto | 0 | 2 | 6.383 | 49.9 |
| qemu-fb | 1 | bsp | 0 | 2 | 7.455 | 50.4 |
| qemu-fb | 4 | auto | 0 | 1 | 7.297 | 51.2 |
| qemu-fb | 4 | auto | 1 | 1 | 2.612 | 46.2 |
| qemu-fb | 4 | bsp | 0 | 2 | 5.959 | 54.1 |
| qemu-serial | 1 | auto | 0 | 1 | 3.703 | 34.3 |
| qemu-serial | 1 | bsp | 0 | 1 | 5.967 | 43.0 |
| qemu-serial | 4 | auto | 1 | 1 | 1.396 | 22.0 |
| qemu-serial | 4 | bsp | 0 | 1 | 6.380 | 31.7 |
| vbox-fb | 1 | auto | 0 | 2 | 5.897 | 101.3 |
| vbox-fb | 1 | bsp | 0 | 2 | 7.082 | 93.2 |
| vbox-fb | 4 | auto | 0 | 1 | 9.457 | 105.2 |
| vbox-fb | 4 | auto | 1 | 1 | 4.853 | 92.7 |
| vbox-fb | 4 | bsp | 0 | 2 | 8.587 | 109.6 |
| vbox-serial | 1 | auto | 0 | 1 | 4.250 | 62.4 |
| vbox-serial | 1 | bsp | 0 | 1 | 4.058 | 93.0 |
| vbox-serial | 4 | auto | 1 | 1 | 2.908 | 90.5 |
| vbox-serial | 4 | bsp | 0 | 1 | 4.886 | 66.8 |

无显卡并未消除CPU0/AP与平台差异；本轮不把全部差异归为framebuffer，也不声称已经逐函数归因到虚拟化器内部。可关闭的结论是：旧表改变了执行CPU、包含虚拟设备/日志成本，不能支持管道跨核扩展性结论；现在以固定放置和guest/host分段公布实测范围。QEMU原口径本轮约2.48×、VBox约1.30×；改成CPU0控制后约1.25×与0.82×。这组对照使口径混杂可见，而不是把旧2.77/1.33当作稳定常数。

## 环境矩阵与仓库证据

四组均为512MiB、AHCI安装盘、e1000/NAT、Linux=1/interp=0，1/4CPU，无ISO；VBox为7.2.20、UEFI、PIIX3。均验证内容、EOF、正常退出和回收，性能不设必须提速的断言。

| 配置 | 结果与证据 |
|---|---|
| qemu-fb | [算术/功能 PASS](../tests/evidence/logs/release-perf-qemu-fb-check.log)、[分段/亲和 PASS](../tests/evidence/logs/release-perf-qemu-fb-analysis.log)；[配置](../tests/evidence/logs/release-perf-qemu-fb-config.log)、[完整数值JSON](../tests/evidence/logs/release-perf-qemu-fb-diagnosis.log)、[原口径结果](../tests/evidence/logs/release-perf-qemu-fb-results.log)、[1 vCPU串口选录](../tests/evidence/logs/release-perf-qemu-fb-smp1-serial.log)、[4 vCPU串口选录](../tests/evidence/logs/release-perf-qemu-fb-smp4-serial.log) |
| qemu-serial | [算术/功能 PASS](../tests/evidence/logs/release-perf-qemu-serial-check.log)、[分段/亲和 PASS](../tests/evidence/logs/release-perf-qemu-serial-analysis.log)；[配置](../tests/evidence/logs/release-perf-qemu-serial-config.log)、[完整数值JSON](../tests/evidence/logs/release-perf-qemu-serial-diagnosis.log)、[原口径结果](../tests/evidence/logs/release-perf-qemu-serial-results.log)、[1 vCPU串口选录](../tests/evidence/logs/release-perf-qemu-serial-smp1-serial.log)、[4 vCPU串口选录](../tests/evidence/logs/release-perf-qemu-serial-smp4-serial.log) |
| vbox-fb | [算术/功能 PASS](../tests/evidence/logs/release-perf-vbox-fb-check.log)、[分段/亲和 PASS](../tests/evidence/logs/release-perf-vbox-fb-analysis.log)；[配置](../tests/evidence/logs/release-perf-vbox-fb-config.log)、[完整数值JSON](../tests/evidence/logs/release-perf-vbox-fb-diagnosis.log)、[原口径结果](../tests/evidence/logs/release-perf-vbox-fb-results.log)、[1 vCPU串口选录](../tests/evidence/logs/release-perf-vbox-fb-smp1-serial.log)、[4 vCPU串口选录](../tests/evidence/logs/release-perf-vbox-fb-smp4-serial.log) |
| vbox-serial | [算术/功能 PASS](../tests/evidence/logs/release-perf-vbox-serial-check.log)、[分段/亲和 PASS](../tests/evidence/logs/release-perf-vbox-serial-analysis.log)；[配置](../tests/evidence/logs/release-perf-vbox-serial-config.log)、[完整数值JSON](../tests/evidence/logs/release-perf-vbox-serial-diagnosis.log)、[原口径结果](../tests/evidence/logs/release-perf-vbox-serial-results.log)、[1 vCPU串口选录](../tests/evidence/logs/release-perf-vbox-serial-smp1-serial.log)、[4 vCPU串口选录](../tests/evidence/logs/release-perf-vbox-serial-smp4-serial.log) |

串口选录明确省略重复COW/exec行，保留全部fork亲和、时序、错误和关机关键行及完整源日志SHA-256；不冒称全文。JSON保留所有测量数值，未裁剪慢样本。

[本轮比值汇总](../tests/evidence/logs/release-perf-summary.log)；[源文件/夹具及内核身份](../tests/evidence/logs/release-perf-identities.log)。编译开关矩阵后的重建ELF文件级哈希不同，但所有PT_LOAD地址、权限、大小与加载字节SHA均相同，身份记录已断言一致；性能四组自身使用完全相同ELF文件。基准运行时工作树有修改，不能只用基线git_revision代表源状态，应核对内核/源文件哈希。

| 全量回归 | 结果 |
|---|---|
| hosted Linux编译开 | [PASS (0 failures)](../tests/evidence/logs/release-perf-hosted-on.log) |
| hosted Linux编译关 | [PASS (0 failures)](../tests/evidence/logs/release-perf-hosted-off.log) |
| QEMU smp4 Linux+interp开，无NIC，单节点 | [PASS (0 failures)](../tests/evidence/logs/release-perf-debug.log) |
| QEMU smp4 Linux+interp开，无NIC，双NUMA | [PASS (0 failures)](../tests/evidence/logs/release-perf-numa.log) |
| ENOSYS文本/号码桶验证 | [hosted+poweroff](../tests/evidence/logs/release-perf-observation-hosted.log)、[单节点](../tests/evidence/logs/release-perf-observation-qemu.log)、[双节点](../tests/evidence/logs/release-perf-observation-numa.log)均PASS |

[kernel构建](../tests/evidence/logs/release-perf-kernel.log)无warning/error；[产品ISO已恢复构建](../tests/evidence/logs/release-perf-product-iso.log)。VBox矩阵为产品性能/功能，并非本轮VBox全量debug验收。

失败也归档：[诊断计时器最初生成空RW段被加载器拒绝](../tests/evidence/logs/release-perf-failed-timer-run.log)（只修探针BSS，不改ELF校验）、[hush内容校验失败](../tests/evidence/logs/release-perf-failed-hush-run.log)及[串口选录](../tests/evidence/logs/release-perf-failed-hush-serial.log)、[VBox无GPU仍设置VRAM的配置错误](../tests/evidence/logs/release-perf-failed-vbox-setup.log)（修脚本，不冒称guest通过）。成功轮使用新实验盘，从头校验，不筛选失败请求或以hush exit0代替字节校验。
