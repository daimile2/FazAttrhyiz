# 用户 shell：短命令与标准输入/输出重定向

> 证据归档：见 [tests/evidence 索引与脱敏规则](../tests/evidence/README.md)。矩阵链接为原轮次留存日志的脱敏副本；旧矩阵保留原轮次记录；短命令增量实测见本文末尾。命令中的 `/tmp` 是新运行的输出位置。

更新：2026-10-15。本轮交付前台 `>`、`>>`、`<` 与输入输出组合，适用于内建
`echo/ls/cat`、外部短命令和 `run /absolute/program.faz ...`。仍是用户态shell，使用现有VFS/FILE
capability；未改KPTI、buddy锁、时钟、TCP RTO或IRQ核心。

## 短命令优先

在 FazAttrhyiz 提示符直接输入命令，不需要 `run` 或后缀：

```text
hello
hello-sdk first second
cat /example.txt
ls /bin
httpd 8080 2 &
http-get 10.0.2.2 8000 /hello.txt
```

内建 `help/echo/ls/cat/kill/exit/poweroff/shutdown` 优先。其余短名称查找固定目录 `/bin`，先尝试 `.faz`，仅文件不存在时回退 `.elf`；不搜索当前目录、不展开环境 PATH，也不会因 persona 禁用或权限错误再尝试另一份程序。短名称限 1–63 个 ASCII 字母、数字、`-`、`_`；文件名区分大小写。未找到返回 `shell error=-12`。

`micropython /data/linux/micropython-demo.py` 使用同一解析规则，但仍须先开启 Linux persona；关闭时明确拒绝。原生短命令不依赖 Linux。

`run` 保留作显式路径/授权入口，例如 `run /bin/hello.faz`、`run /bin/cat.faz /example.txt`。内建 `cat/ls` 不创建子进程，若要验证对应外部 ELF 或规避内建长度限制，请显式使用 `run`。交互式 nc 仍用 `run -i /bin/nc.faz 10.0.2.2 9998` 委托控制台；短命令不会自动获得键盘权限。直接输入 `nc HOST PORT` 会返回 `-7` 并提示 `use run -i /bin/nc.faz HOST PORT`，不会尝试连接。

短命令支持原有前后台及前台重定向：`httpd 8080 2 &`、`hello > /hello-output.txt`、`micropython /data/linux/micropython-demo.py > /python-output.txt`。后台重定向、`run -i` 与重定向组合仍拒绝。不存在的外部命令与显式 `run` 一样，可能在打开输出文件之后才失败，因此不要用未知命令重定向覆盖重要文件。

旧安装盘不会自动覆盖已有 shell；复现本轮旅程请用新安装盘，或明确部署更新后的 `/bin/shell.faz` 与内核引导文件，不要重新格式化已有数据盘。

## 用法

```text
echo first > /note.txt
echo second >> /note.txt
ls /bin > /listing.txt
cat < /note.txt > /copy.txt
cat < /copy.txt > /elf-copy.txt
ls /bin > /elf-list.txt
http-get 10.0.2.2 8000 /hello.txt > /response.txt
cat /response.txt
```

重定向操作符可紧邻参数，如 `cat</note.txt>/copy.txt`；也可按上例用空格。
路径须绝对路径，父目录已存在。当前没有引号、转义、变量展开、通配符、
文件描述符编号选择、stderr重定向或here-document；请只使用上述最小语法。

| 操作 | 打开方式 | 已有文件/错误行为 |
| --- | --- | --- |
| `> /file` | WRONLY\|CREAT\|TRUNC | 创建缺失文件，或明确截断已有文件；没有noclobber |
| `>> /file` | WRONLY\|CREAT\|APPEND | 缺失则创建，每次写从当前末尾追加；不承诺多writer原子记录 |
| `< /file` | RDONLY | 必须存在且可读，不创建；读到文件末尾返回0 |
| `< /in > /out` | 先输入、再输出 | 输入打开失败不会截断输出；相同文本路径拒绝 |

每条命令最多一个输入目标、一个输出目标；重复、缺路径、后台`&`、`run -i`组合、
管理命令、非法短名称或管道语法在打开文件前拒绝。`run`程序路径须绝对路径。
但文件成功打开后，程序不存在/ELF加载失败/执行报错**不会恢复被截断的旧内容**；
已有部分写入也可能保留。这不是事务式重定向或原子文件替换。

输出命令完成后shell对仍持有的输出FILE执行FSYNC，再关闭自己的描述符，记录：

```text
shell redirect committed: status=0
```

底层写入/FSYNC/关闭或子进程执行失败会报告 `shell redirect failed` / `shell error`。
子进程自己的退出码仍在 `shell child done: ... exit=N reap=ok` 中可见；重定向run的
非零子退出被shell报告为IO错误，不当作成功提交。错误不保证撤销已有磁盘副作用。

内建 `cat` 仅在直接显示到屏幕时做最多4096B的文本预览并替换非打印字节；若文件仍有内容，明确返回 `CM_ERR_NOSPC (-6)` 和预览上限提示。重定向时按原始字节复制到 EOF，不转义、不截为4096B、不添加换行。`/bin/cat.faz` 也按原始字节复制到 EOF，无原先1MiB总量限制；底层单次调用/文件容量限制仍保留。ELF cat省略路径时读取fd0，指定路径时打开该文件。屏幕直接显示二进制请优先使用内建预览，避免终端控制字节。
SDK `cm_print/cm_output`写fd1，`cm_report`计数/错误摘要改用fd2，避免把
`ls entries`等诊断计数混入输出文件。不是所有示例都把所有诊断移至stderr。

## 权限与生命周期

文件仍通过 OPEN得到FILE capability，再由进程fd表索引；VFS路径/只读卷检查保留。
写入检查WRITE，输入检查READ；没有通过文件名绕过已有capability检查，也没有新增ACL系统。

原SPAWN syscall3的32B request和旧调用不变：

```text
a0=CONTROL handle, a1=spawn request*, a2=child result*, a3=0 或 stdio extension*
```

扩展为16B、native little-endian：

```c
typedef struct {
    uint32_t size;       /* 16 */
    uint32_t flags;      /* 0 */
    int32_t input_fd;    /* -1=默认，或父进程>=3的FILE fd */
    int32_t output_fd;   /* -1=默认，或父进程>=3的FILE fd */
} cm_user_stdio_t;
```

先copyin完整扩展，校验版本大小/保留位、父FILE类型和DUPLICATE+READ/WRITE。
内核保留稳定对象引用，在子ELF创建线程/发布runnable**之前**安装fd0/fd1；
只给子进程对应READ或WRITE，不给DUPLICATE/TRANSFER，不改变固定CPU亲和策略。
父子持有同一open-file对象和位置，父关闭不使子FILE失效。
首版shell仅前台串行使用这些描述符，不承诺同一open-file多进程同时改位置的完整POSIX语义。

FILE对象改为owned析构，最后引用统一关闭VFS；创建/失败/退出/kill清理包含绑定的
标准fd。关闭绑定fd1后再次write返回错误，不能偷偷回退到诊断stdout。
未绑定时保留原fd1/fd2诊断输出，fd0仍不是控制台stdin。

提示符、命令回显、shell错误和spawn/wait/reap日志始终在console；原始文件里只有
应用向stdout写的字节，没有内核日志前缀。现有文件syscall用户范围验证和1MiB单次
I/O限制不变，非法指针不会消耗输入流或写入文件。

## 与交互、后台和管道的关系

- `run -i /bin/nc.faz ...`沿用CONSOLE授权与PS/2交互，正常收发、`/quit`退出。
  不能与重定向或`&`混用；拒绝时不先打开/截断目标。
- 普通 `run ... &`、kill和有界poweroff保持原有行为；首版不支持后台重定向。
- `nc`本轮未增加文件批处理/半关闭语义，不能用 `< file`替代它的交互控制台。
- `|`明确返回NOTSUP（-7），尚未实现单段管道。没有用临时磁盘文件冒充具有
  EOF、阻塞、背压和关闭唤醒语义的pipe；管道需后续单独补齐流对象生命周期。

常见错误：INVAL=-2（语法/参数/用户范围）、DENIED=-3（cap权限）、NOTSUP=-7
（管道/未支持语法）、IO=-11（I/O或重定向子程序非零退出）、NOENT=-12（路径不存在）、
ISDIR=-14、ROFS=-16；资源不足仍返回NOMEM/NOSPC等原错误。FSYNC失败不算持久化通过。

## 自测与手工复现

`make test`及debug suite增加：

```text
stdio snapshot: files=0/0 free+clean-cache=X/X
stdio redirect verified: copyin/rights denied parent-close child-read/write payload=ok fd-reclaim=no-leak
selftest result: PASS (0 failures)
```

测试覆盖非法用户地址、缺DUPLICATE/读写权限、子fd最小权限、父提前close、真实ELF
stdin→stdout内容、标准fd释放与不回退console。hosted不能执行ring3，会明确跳过
用户执行并直接调用文件syscall验证；freestanding运行真实cat ELF。
写文件会合法保留clean cache，因此页快照精确比较free+clean-cache，另独立检查VFS
open-file数恢复；没有把普通进程/栈/页表/FILE泄漏放宽成可接受。

手工冷启动验收（新安装盘，光驱不挂ISO）：

```text
echo old > /redirect.txt
echo redir-first > /redirect.txt
echo redir-append >> /redirect.txt
cat < /redirect.txt > /copy.txt
cat < /copy.txt > /elf-copy.txt
ls /bin > /listing.txt
poweroff
```

同盘真正关机再启动：

```text
cat /redirect.txt
cat /elf-copy.txt
cat /listing.txt
poweroff
```

前两个文件均应只有`redir-first`和`redir-append`两行，无`old`、提示符或reap日志；
listing包含实际目录内容。不要保存VM运行状态替代冷启动。旧磁盘shell不会被自动覆盖，
本轮验收使用新安装镜像；已有系统升级不是重跑blank-only安装器。

## 环境矩阵与日志

日期：2026-10-15。各项以真实日志填写；产品通过不等于debug全量通过。

| 环境 | 配置 | 结果 | 证据 |
| --- | --- | --- | --- |
| hosted | Linux/C11 | PASS，全量0 failures | [redir-hosted-final.log](../tests/evidence/logs/redir-hosted-final.log) |
| QEMU debug | KVM、512MiB、4 CPU | PASS，全量0 failures | [redir-qemu-debug-final.log](../tests/evidence/logs/redir-qemu-debug-final.log) |
| QEMU产品/冷启动 | AHCI安装盘，无ISO、e1000、4 CPU | PASS，冷启动内容断言通过；nc20B/HTTP/kill回归通过 | [redir-qemu-final1.log](../tests/evidence/logs/redir-qemu-final1.log)、[redir-qemu-final2.log](../tests/evidence/logs/redir-qemu-final2.log) |
| VBox产品/冷启动 | 7.2.20、EFI64、PIIX3、512MiB、4 CPU、AHCI、82540EM | PASS，重定向写盘+冷启动内容断言通过 | [redir-vbox-final1.log](../tests/evidence/logs/redir-vbox-final1.log)、[redir-vbox-final2.log](../tests/evidence/logs/redir-vbox-final2.log) |

宿主为Linux 7.0.0-38-generic、Intel i5-13500H；QEMU 8.2.2/KVM，VBox 7.2.20r175154。
VBox启动时e1000报告link down，网络未作专项验收、不记PASS；本轮不修改其驱动核心。
QEMU已验证nc20B echo、HTTP重定向、kill137/reap/ACPI关机。VBox未跑全量debug，
其PASS仅指表内产品重定向/持久化测试，不代替hosted/QEMU的全量PASS。
VBox和KVM/QEMU测试应串行运行；本机重叠启动曾报KVM entry failed，不能记为内核回归PASS。
原轮次日志现已脱敏归档；测试使用独立临时VM，未修改用户原有VM。归档不包含虚拟磁盘或VM配置。

冷启动日志的逐字节断言脚本已保留，可复查当前证据：

```bash
python3 tests/check_shell_redirection.py qemu /tmp/redir-qemu-final2.log
python3 tests/check_shell_redirection.py vbox /tmp/redir-vbox-final2.log
```

对应输出 `shell redirection coldboot: PASS`；检查覆盖/追加最终字节、ELF复制、
目录无诊断污染、拒绝操作后guard内容、已有盘挂载与ACPI关机。QEMU另查HTTP正文，
不因仅出现键盘命令回显就判定成功。串口中init偶尔在提示符后插入日志，脚本匹配
完整命令而不要求prompt和命令相邻。断言结果保存在[redir-qemu-assertions.log](../tests/evidence/logs/redir-qemu-assertions.log)、[redir-vbox-assertions.log](../tests/evidence/logs/redir-vbox-assertions.log)。

## 控制台前缀行协议 v1（2026-10-18）

选择三种方案中的**前缀行协议**：不新增tty/kmsg设备、不改调度、不延迟逐键回显。
内核日志仍为 `[INF]`、`[ERR]` 等既有前缀；用户态产品shell增加：

```text
[SHELL v=1 event=ready seq=1 status=0]
fazattrhyiz> echo hello
[SHELL v=1 event=begin seq=1 status=0]
hello
[SHELL v=1 event=end seq=1 status=0]
[SHELL v=1 event=ready seq=2 status=0]
fazattrhyiz>
```

每条记录以换行开始、以换行结束，小于112字节，用一次既有CONSOLE_WRITE capability调用输出。
该接口已在整次调用期间持有现有console/klog消息锁；发生EAGAIN则沿用uputs的yield/retry，
不引入新锁、调度规则或内核全局观测开关。串口和FB均可见相同记录。

- shell每次启动seq从1开始；每次提交Enter（含空行、语法错误、输入过长）发begin，
  dispatch返回发end，下一个ready递增seq。退格和输入字符仍即时回显。
- status是shell dispatch原有返回码，不是子进程exit码。普通run等待成功但子进程非零退出时，
  仍须读取独立的 `shell child done: ... exit=... reap=ok`；没有改变原有shell返回语义。
- `exit`成功发end status=1，然后shell退出，不再发ready。
- `poweroff/shutdown`先发begin；成功关机不返回，因此不承诺end/ready。脚本需同时核对实际关机。
- shell错误行及child-done行也合并成单次console写，避免pid/exit等字段之间插入其他CPU日志。
- 记录直接走console，不进入命令的 `> / >>` 输出文件；普通重定向文件正文不被协议污染。

人工可读性改善在于：每次提交/完成有独立完整行，能区分键盘回显、shell状态和内核日志，
无需从夹杂日志的提示符行猜测命令是否结束。代价是每条普通命令多三条简短记录。
不保证用户程序多次write的正文、后台任务输出或逐键回显组成不可交错的整行；不是完整VT/多tty。
这是可信测试环境中的同步约定，不是防伪审计通道；用户程序也能打印相似文字。
旧debug内核shell和不含本轮shell ELF的旧安装盘不承诺v1协议。

`tests/check_user_journey.py`现在等待指定seq的ready/begin/end和预期status，
只在begin之后检查正文/child状态，排除输入命令中的路径、字符串所造成的假命中。
例如cat命令中的 `/journey-native.txt` 不再能替代实际文件正文 `journey-native`。
后台httpd仍须等待listening并通过宿主HTTP正文检查；协议end只表示后台启动命令返回。
关机等待begin及QEMU退出/VBox poweroff，不等待不可能到来的end。

### 本轮环境矩阵

| 环境（2026-10-18） | 结果与范围 | 证据 |
|---|---|---|
| QEMU 8.2.2 KVM SMP4 512MiB，AHCI安装盘/e1000，无ISO | 完整三次冷启动旅程PASS：默认原生→离线启用Linux/MicroPython/HTTP→禁用保留原生；各命令序号/status核对 | [旅程检查](../tests/evidence/logs/shellproto-qemu-check.log)、[原生](../tests/evidence/logs/shellproto-qemu-native.log)、[Linux](../tests/evidence/logs/shellproto-qemu-linux.log)、[禁用](../tests/evidence/logs/shellproto-qemu-disabled.log) |
| VBox 7.2.20 UEFI PIIX3 SMP4 512MiB，AHCI安装盘/e1000 NAT，无ISO | 相同三次冷启动旅程PASS，PS/2注入+串口，HTTP与ACPI关机通过 | [旅程检查](../tests/evidence/logs/shellproto-vbox-check.log)、[原生](../tests/evidence/logs/shellproto-vbox-native.log)、[Linux](../tests/evidence/logs/shellproto-vbox-linux.log)、[禁用](../tests/evidence/logs/shellproto-vbox-disabled.log) |
| hosted | 全量0 failures；不模拟PS/2/ring3旅程 | [日志](../tests/evidence/logs/shellproto-hosted.log) |

复现使用新的可丢弃目录（安装工具继续拒绝覆盖已有盘）：

```sh
scripts/demo-user-journey.sh prepare /tmp/capmix-protocol-qemu
scripts/demo-user-journey.sh check-qemu /tmp/capmix-protocol-qemu
# 待QEMU完全退出，再测VBox；两者不并发
scripts/demo-user-journey.sh prepare /tmp/capmix-protocol-vbox
scripts/demo-user-journey.sh check-vbox /tmp/capmix-protocol-vbox
```

旅程PASS不是全量selftest。完整命令及双轨限制仍见 [USER_JOURNEY](USER_JOURNEY.md)。

补充回归：[QEMU四核Linux+interp debug串口](../tests/evidence/logs/shellproto-debug.log)、
[完整检查链](../tests/evidence/logs/shellproto-debug-check.log)为全量0 failures（无NIC wire明确skip）。
[内核构建](../tests/evidence/logs/shellproto-kernel.log)无新增warning/error，
[默认产品ISO](../tests/evidence/logs/shellproto-product-iso.log)成功；
[默认ISO启动](../tests/evidence/logs/shellproto-product.log)看到ready seq=1与提示符，空闲timeout=124，非全量自测。

## 短命令旅程增量验收（2026-10-19）

当前源码原先仅内建 `cat/ls/echo`；本轮按授权补了用户态 shell 的 `/bin/*.faz` → `.elf` 有界查找，不改内核。

| 环境 | 本轮结果与证据 |
|---|---|
| hosted 全量 | [PASS (0 failures)](../tests/evidence/logs/short-commands-hosted.log) |
| QEMU/KVM，512MiB，4 CPU，AHCI/e1000，无 ISO | [三次冷启动旅程 PASS](../tests/evidence/logs/short-commands-qemu-check.log)：Linux off→on→off；短命令 hello/cat/httpd/micropython、显式 run、持久化/重定向、HTTP200、关机；[native](../tests/evidence/logs/short-commands-qemu-native.log)、[Linux](../tests/evidence/logs/short-commands-qemu-linux.log)、[disabled](../tests/evidence/logs/short-commands-qemu-disabled.log)、[HTTP 客户端](../tests/evidence/logs/short-commands-qemu-http-client.log) |
| VBox UEFI，512MiB，4 CPU，PIIX3，AHCI/e1000，无 ISO | [相同三次冷启动旅程 PASS](../tests/evidence/logs/short-commands-vbox-check.log)；[native](../tests/evidence/logs/short-commands-vbox-native.log)、[Linux](../tests/evidence/logs/short-commands-vbox-linux.log)、[disabled](../tests/evidence/logs/short-commands-vbox-disabled.log)、[HTTP 客户端](../tests/evidence/logs/short-commands-vbox-http-client.log) |

产品按键由 `scripts/demo-user-journey.sh check-qemu/check-vbox LAB` 注入，经 `[SHELL ... begin/end/ready]` 边界断言真实输出；不把命令回显当成功。负例包括未知短命令 `-12`、Linux 关闭拒绝 `-7`，以及 `poweroff > /short-guard.txt` 在打开目标前拒绝，随后确认目标不存在。原生 `hello > /short-output.txt` 与 Linux `micropython ... > /journey-python.txt` 均读回正确。

[编译日志](../tests/evidence/logs/short-commands-build.log)无 warning/error。产品旅程 PASS 不替代 VBox 全量 debug 或网络压力验收。

QEMU/KVM smp4、Linux/interp 开启、无 NIC 的[全量回归为 PASS (0 failures)](../tests/evidence/logs/short-commands-debug.log)，外部网络按原有语义 skip；[默认产品 ISO 已重新构建](../tests/evidence/logs/short-commands-product-iso.log)。

2026-10-19 cat 数据完整性修复：重定向原样复制到 EOF；外部 cat 也不再替换字节。QEMU/VBox 三次冷启动逐字节比较通过，详见 [v11 评审核对与环境矩阵](REVIEW_V11_FOLLOWUP.md)。旧矩阵和旧数据边界记录不代表当前cat实现。

发布前性能口径更新：约117ms已分解到前一连接同步TIME_WAIT等待及剩余请求阶段；历史hush比值含CPU0/AP放置变化，不作跨核扩展性结论。见 [HTTP/hush诊断与矩阵](RELEASE_PERF_DIAGNOSTICS.md)。
