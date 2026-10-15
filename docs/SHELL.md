# 用户 shell：标准输入/输出重定向

更新：2026-10-15。本轮交付前台 `>`、`>>`、`<` 与输入输出组合，适用于内建
`echo/ls/cat` 和 `run /absolute/program.elf ...`。仍是用户态shell，使用现有VFS/FILE
capability；未改KPTI、buddy锁、时钟、TCP RTO或IRQ核心。

## 用法

```text
echo first > /note.txt
echo second >> /note.txt
ls /bin > /listing.txt
cat < /note.txt > /copy.txt
run /bin/cat.elf < /copy.txt > /elf-copy.txt
run /bin/ls.elf /bin > /elf-list.txt
run /bin/http-get.elf 10.0.2.2 8000 /hello.txt > /response.txt
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
不支持命令或管道语法在打开文件前拒绝。`run`程序路径须绝对路径。
但文件成功打开后，程序不存在/ELF加载失败/执行报错**不会恢复被截断的旧内容**；
已有部分写入也可能保留。这不是事务式重定向或原子文件替换。

输出命令完成后shell对仍持有的输出FILE执行FSYNC，再关闭自己的描述符，记录：

```text
shell redirect committed: status=0
```

底层写入/FSYNC/关闭或子进程执行失败会报告 `shell redirect failed` / `shell error`。
子进程自己的退出码仍在 `shell child done: ... exit=N reap=ok` 中可见；重定向run的
非零子退出被shell报告为IO错误，不当作成功提交。错误不保证撤销已有磁盘副作用。

内建cat沿用最多4096B文本显示边界；较大的流式文件请用 `/bin/cat.elf`，上限1MiB。
两者替换非文本字节，不是二进制cp。ELF cat省略路径时读取fd0，指定路径时仍打开该文件。
SDK `cm_print/cm_output`写fd1，`cm_report`计数/错误摘要改用fd2，避免把
`ls entries`、`cat bytes read`等诊断计数混入输出文件。不是所有示例都把所有诊断移至stderr。

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

- `run -i /bin/nc.elf ...`沿用CONSOLE授权与PS/2交互，正常收发、`/quit`退出。
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
run /bin/cat.elf < /copy.txt > /elf-copy.txt
run /bin/ls.elf /bin > /listing.txt
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
| hosted | Linux/C11 | PASS，全量0 failures | `/tmp/redir-hosted-final.log` |
| QEMU debug | KVM、512MiB、4 CPU | PASS，全量0 failures | `/tmp/redir-qemu-debug-final.log` |
| QEMU产品/冷启动 | AHCI安装盘，无ISO、e1000、4 CPU | PASS，冷启动内容断言通过；nc20B/HTTP/kill回归通过 | `/tmp/redir-qemu-final1.log`、`/tmp/redir-qemu-final2.log` |
| VBox产品/冷启动 | 7.2.20、EFI64、PIIX3、512MiB、4 CPU、AHCI、82540EM | PASS，重定向写盘+冷启动内容断言通过 | `/tmp/redir-vbox-final1.log`、`/tmp/redir-vbox-final2.log` |

宿主为Linux 7.0.0-38-generic、Intel i5-13500H；QEMU 8.2.2/KVM，VBox 7.2.20r175154。
VBox启动时e1000报告link down，网络未作专项验收、不记PASS；本轮不修改其驱动核心。
QEMU已验证nc20B echo、HTTP重定向、kill137/reap/ACPI关机。VBox未跑全量debug，
其PASS仅指表内产品重定向/持久化测试，不代替hosted/QEMU的全量PASS。
VBox和KVM/QEMU测试应串行运行；本机重叠启动曾报KVM entry failed，不能记为内核回归PASS。
`/tmp`文件是当前工作区证据，不是长期归档。新建VBox测试VM为
`CapMix-redirect-20261015`，未修改用户原有`capmix` VM；测试盘/配置保留供检查。

冷启动日志的逐字节断言脚本已保留，可复查当前证据：

```bash
python3 tests/check_shell_redirection.py qemu /tmp/redir-qemu-final2.log
python3 tests/check_shell_redirection.py vbox /tmp/redir-vbox-final2.log
```

对应输出 `shell redirection coldboot: PASS`；检查覆盖/追加最终字节、ELF复制、
目录无诊断污染、拒绝操作后guard内容、已有盘挂载与ACPI关机。QEMU另查HTTP正文，
不因仅出现键盘命令回显就判定成功。串口中init偶尔在提示符后插入日志，脚本匹配
完整命令而不要求prompt和命令相邻。断言结果保存在`/tmp/redir-{qemu,vbox}-assertions.log`。
