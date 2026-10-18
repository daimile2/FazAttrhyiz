# 原生 / Linux fd 表：64 槽

2026-10-18 将共用 `CM_FD_MAX` 从32扩到 **64**，有效索引0..63。
Linux persona 默认仍关闭；关闭时原生也使用64槽，不删除/替换原生 `/bin`。
没有修改 KPTI、buddy 锁、时钟、TCP/RTO、安装盘布局或调度纪律。

## 实现与边界

- 上限定义在 `include/capmix/fd_limits.h`（无C类型，汇编黄金探针也可包含），`proc.h` 引入它。
- `fd_store` 是64个int，`linux_console_store`、`linux_cloexec_store` 各64个uint8。
  分配、查找、退出关闭、fork继承、exec CLOEXEC、pipe/socket dup 与fcntl边界均使用同一常量。
- `stdio_mask` **只描述fd0/1/2**，不是全fd位图；保留原宽度。各位移都受fd<3或短路分支保护，
  fd32..63不对它做位移。没有需要由32位扩成64位的通用fd bitmap。
- 仍分配最小可用fd。stdio保持占用时，普通文件等最多再占61槽；关闭后复用。
  Linux `dup2` 越界仍EBADF；F_DUPFD起点越界仍EINVAL，满表仍EMFILE。
  内部原生 `cm_native_dup2` 补充目标索引检查，避免可信调用误用造成越界。
- fd是capability handle的适配索引，读写权限、DUPLICATE与TRANSFER策略不变。
  Linux FILE/pipe/socket共用这个fd表；原生网络API直接返回capability，不因本轮扩大后端连接容量。
- 全局VFS同时打开文件池仍64、每task句柄表仍256、TCP后端仍32槽，其他池也未扩大。
  64不是保证每进程同时新开64个独立FILE对象；其他进程或后端可先耗尽共享资源。
- 不新增RLIMIT/动态调额、select/poll全集、线程组fd创建能力或完整POSIX承诺。

## 错误语义与内存

| 情形 | 行为 |
|---|---|
| 原生fd表满 | 原有CM_ERR_NOSPC=-6；临时FILE/handle回滚 |
| Linux fd表满 | 原有-EMFILE=-24；不消耗后端对象 |
| 后端TCP槽先满但fd仍有空位 | 原有-ENOBUFS=-105，与EMFILE区分 |
| 关闭一个高fd再open | 复用最小空位；Linux console/CLOEXEC标签重置 |

x86-64实测 `sizeof(cm_proc_t)`：**720 → 912B**。
新增32槽 × (4B fd + 1B console + 1B CLOEXEC) = **192B/进程记录**。
固定32个proc记录（包含线程记录）使 `g_procs` 的元数据增加 **6144B（6KiB）BSS**。
线程组仍通过指针共享leader的fd表，但成员记录本身也含预留数组；不按活跃线程再动态分配一个表。
这是静态驻留元数据，不是每次open额外预占64页；实际FILE/handle/页/COW commit记账沿用原有规则。
最终内核页对齐造成的镜像页变化与6KiB元数据增量不是同一指标，不将后者伪称精确页数。
初始化/查找/关闭/fork/exec扫描上界32→64；满表时需复制/释放更多句柄，未承诺零性能成本。
上一轮USER_JOURNEY性能表保留FD32采样版本；本轮短产品回归不是同负载重新测性能。

## 测试影响与复现

`boot/fd_table_test.c` 在Linux开/关均测原生，开启时再测Linux：

1. 一个FILE经重复句柄填满fd3..63，避免先碰到VFS全局池；检查满表错误与临时资源回滚。
2. fd63真实读写；Linux高位CLOEXEC设置、fork继承与子进程关闭不破坏父进程。
3. 关闭47和63后分别open复用；检查高位标签重置、F_DUPFD与dup2越界。
4. 关闭全部再open得到3；退出后VFS打开数、页数和commit回到基线。

现有 `tests/linux_threads.S` 满表循环及暂存数组原写死32；现在共用上限头，数组64×8B。
仍要求最终EMFILE，并将最低占用断言从20提高到52；没有删除失败断言或开启线程组fork/exec。
黄金ELF重新生成并钉SHA-256：

```text
b1e902f8c31ac15bd0821ae34477a6c05ae28378bd7d96918bd559993730932b
```

```sh
make linux-threads-verify
make test
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
# 运行现有QEMU serial验收命令；无NIC时加-nic none，wire项明确skip。
python3 tests/check_fd_table.py /path/to/serial.log
# hosted 开启、interp关闭：
python3 tests/check_fd_table.py --hosted --interp-off /path/to/hosted.log
# Linux关闭配置日志加 --disabled；hosted日志另加 --hosted。
```

检查器同时运行既有SIGCHLD/hush/MicroPython/线程/动态黄金链。
L2宿主检查器现按黄金程序启动PID→fork→同一子进程exec→wait状态42关联，
不再假设全日志第一次fork一定是L2黄金程序；其他负例和ENOSYS断言保留。
日志 `fd64 metadata ... delta=192 bytes/slot` 中slot指**proc记录槽**，不是单个fd的字节数。

## 2026-10-18 环境矩阵

下列Linux开/关指 `LINUX_ABI=1` 内核的运行配置 `capmix.linux=1/0`，不改变默认产品配置。

| 环境 | 本轮结果与证据 |
|---|---|
| hosted Linux开、interp关 | 全量 `PASS (0 failures)`；[日志](../tests/evidence/logs/fd64-hosted-on.log)、[完整检查](../tests/evidence/logs/fd64-hosted-on-check.log)，硬件ELF执行明确skip |
| hosted Linux关 | 全量 `PASS (0 failures)`；[日志](../tests/evidence/logs/fd64-hosted-off.log)、[检查](../tests/evidence/logs/fd64-hosted-off-check.log)，原生fd64已测、Linux部分明确skip |
| QEMU KVM/host，512MiB，SMP4，Linux+interp开，无NIC | 全量0 failures；[日志](../tests/evidence/logs/fd64-qemu-on.log)、[黄金链检查](../tests/evidence/logs/fd64-qemu-on-check.log)，hush/MicroPython/线程等通过 |
| QEMU KVM/host，SMP4，Linux关，无NIC | 全量0 failures；[日志](../tests/evidence/logs/fd64-qemu-off.log)、[检查](../tests/evidence/logs/fd64-qemu-off-check.log) |
| VBox7.2.20 UEFI，512MiB，SMP4，Linux+interp开，无NIC | 全量0 failures；[日志](../tests/evidence/logs/fd64-vbox-on.log)、[最终检查](../tests/evidence/logs/fd64-vbox-on-check.log) |
| QEMU SMP4，AHCI/e1000，Linux开，产品盘无ISO | [产品结果](../tests/evidence/logs/fd64-product-check.log)、[串口](../tests/evidence/logs/fd64-product-serial.log)；原生httpd 8次预热+2次请求均200/25B/EOF，hush预热+测量各6000B逐字节通过；不是64连接或性能基线 |
| 构建与固定探针 | [kernel](../tests/evidence/logs/fd64-kernel.log)、[ISO](../tests/evidence/logs/fd64-iso.log)、[探针逐字节重建/哈希](../tests/evidence/logs/fd64-fixture-verify.log)；无新增freestanding warning/error |
| 默认产品ISO，Linux默认关 | [shell烟测](../tests/evidence/logs/fd64-product-boot.log)，提示符后闲置超时124，不冒充全量selftest |

无NIC的全量运行不冒充网络wire验收；httpd实际收发由产品盘那行覆盖。
本轮未单独重跑 `LINUX_ABI=0` 编译矩阵；上述关闭是运行开关验证。

### 保留失败与关闭方式

- [首轮3项线程失败](../tests/evidence/logs/fd64-first-thread-failure.log)：旧固定ELF的32项暂存数组/循环边界
  与新内核64槽不一致，触发 `linux threads failure ... rax=27`，全量确为FAIL (3 failures)。
  修正为共享上限、重建固定ELF及哈希后重跑通过，未降低EMFILE或线程隔离要求。
- 新fd测试在L2黄金之前增加了一个合法fork，旧宿主检查器错误地挑中日志中第一次fork：
  [QEMU旧检查失败](../tests/evidence/logs/fd64-first-checker-failure.log)、
  [VBox旧检查失败](../tests/evidence/logs/fd64-vbox-first-checker.log)。
  两份guest最终日志本身是0 failures。检查器改成关联黄金程序的PID/exec/wait42后，
  对同一保留日志重新只读校验通过，没有声称重跑了内核或将检查失败当作PASS。
  [失败基线仍被拒绝](../tests/evidence/logs/fd64-baseline-rejected.log)。

本轮修改文件：fd_limits/proc头、原生dup2边界、boot fd表自测与kernel注册、L3容量注释、
线程固定探针源码/ELF/sha256、L2与FD64宿主检查器、Makefile及相关容量文档/证据索引。
