# 阶段 2：静态 ELF64 加载与 path-based posix_spawn

## 使用方式与支持边界

保留原 `entry != NULL` 的 C 入口 spawn。新增 ELF 分支：

```c
cm_spawn_attr_t attr = {
    .path = "/bin/hello.elf",
    .entry = NULL,
    .qos = CM_QOS_USER_INTERACTIVE,
};
cm_id_t pid;
cm_error_t err = cm_posix_spawn(&attr, &pid);
/* err==CM_OK 后，由当前启动 CPU 的调度循环运行该任务。 */
```

本轮实现的是创建新任务的 exec 类路径，不是替换调用进程的 `execve()`。
`CM_SYS_EXECVE` 仍返回 NOTSUP。fork 继续 NOTSUP。

首版支持 little-endian x86_64 静态 ET_EXEC，最多 16 个 program headers、128 个
映射页（含 4 页栈）。PT_LOAD 位于 0x400000..0x7fc000；栈在
0x7fc000..0x800000，初始 RSP=0x7fffe0，argc=0、argv/envp 终止项为 NULL。
未提供参数字符串、auxv、动态链接、PT_INTERP、PT_DYNAMIC、TLS 或 ET_DYN。
跨段共享同一虚拟页、W+X 段及入口位于非可执行/非文件内容处会被拒绝。

runner 仍限启动 CPU（CPU0），最多一个未回收 ELF 进程；EEVDF 的候选选择
对该 ELF 明确实施 CPU0 affinity，包含尚未进入、阻塞及退出清理阶段。
用户态 IF=1，LAPIC tick 现在会实际抢占 ELF，并由同一 EEVDF 路径选择下一线程。
ELF 可与普通内核线程交替运行；尚不支持多 ELF 并发或用户线程跨 CPU 迁移。
第二次 spawn 在前一个镜像回收前返回 BUSY；在其他 CPU 调用 spawn 返回 INVAL。

ELF runner 把自己的 full CR3 绑定到现有 context slot。抢占的 timer frame 仍由
现有汇编保存在入口栈上，恢复时使用该 full CR3；timer 返回前重新设置现有
shadow-mapped TSS.RSP0，随后由原有入口汇编恢复进入前的 CR3 并 IRET。
当前 CPU 只有一个 ELF 使用入口窗口，普通内核线程使用各自栈，不覆盖该 frame。
恢复也清除普通 C-entry 进程 EXIT 可能留下的全局 leave-user 标志。
没有修改 VMX/EPT、TRACE 环、页表映射或 KPTI 的入口汇编。
ELF 文件必须使用 CapMix 的原生 syscall ABI，不能直接运行 Linux 用户程序。
首版 ELF syscall 允许 NOP、CLOCK、stdout/stderr WRITE、EXIT；FORK 返回 NOTSUP，
其他调用也返回 NOTSUP，避免未经 copyin/out 校验的旧 syscall 接口暴露给 ELF。
WRITE 检查整个缓冲区都在可读的已加载用户页内；高半内核地址、缺页、越界和
地址溢出都返回 INVAL。旧 C 入口和内嵌 ring3 测试的 ABI 行为保持原样。

## 加载与回收

`proc/elf.c` 使用已有 VFS `stat/open/pread/close` 读取 COWFS 文件，加载期间持有
VFS 的递归锁以保持文件内容一致，不修改 fs/cowfs 或 VFS 实现。

校验 ELF magic、class、endianness、machine、version、header sizes、phdr 表范围、
PT_LOAD 文件/内存大小、虚址范围、对齐、页重叠、入口和资源上限。所有加法先做
减法形式的界限检查，防止 wrap。逐页分配并清零 backing，再按文件偏移读取
PT_LOAD 的 filesz 字节，保留 BSS 及页内空隙为零。

镜像使用单独拥有的用户页表和 backing pages，不改变阶段 1 的 VMO/VMAR 接口。
非 PF_W 段清除私有叶子的 W 位；拒绝 W+X 声明。**尚未启用数据页/栈 NX**，
因此不声称实现完整硬件 W^X。首版也不提供用户故障隔离/恢复或执行时间限制。

KPTI 的原入口汇编、CR3 切换和窗口策略未修改。ELF 使用同样的 `.entry.text` /
`.entry.data` 窗口与共享低半子树。加载器先持有 shadow root 再映射窗口，确保
窗口构建失败时能回收部分页表；不更改原 `cm_space_shadow()` 实现。

`usermode.c` 通过已有 `cm_enter_user()` 进入 ELF entry，设置当前 CPU 的 full /
shadow 元数据；ELF exit 分支恢复完整内核根，切回原调度器。原内嵌 hello 的
`cm_land_user()` 收尾路径保留。sys_exit 记录退出码，镜像 commit 延迟到 off-CPU
回收，防止释放正在使用的页表或 syscall 栈。

调度器确认线程退出并回收线程槽位后，调用 `cm_proc_reap_elf(pid)` 回收镜像、
backing pages、页表和 commit。未退出/仍拥有原线程槽位时返回 BUSY。
成功回收保留 proc/task 的退出记录，镜像指针清空，第二个 ELF 才可启动。

## 测试 ELF 如何生成及进入镜像

源码为 `tests/hello_elf.S`，布局为 `tests/hello_elf.ld`。Makefile 自动执行：

1. 编译汇编为 `build/hello-elf.o`。
2. 静态链接为真正的 ELF64 ET_EXEC：`build/hello.elf`。
3. `scripts/embed-elf.py` 将 ELF 的完整字节生成 `build/hello_elf.inc`。
4. `boot/phase2_test.c` 引入该数组，随内核一起放入 ISO 的 `capmix.elf`。
5. 开机 selftest 经 VFS 创建 `/bin`，写入并 fsync `/bin/hello.elf`；加载器再
   按该路径读取文件。ISO 没有额外的根文件系统挂载协议；运行时文件位于既有
   COWFS 中，测试不从裸数组直接绕过 VFS 加载。

直接生成/查看：

```sh
make build/hello.elf
readelf -h -l build/hello.elf
```

它有两个 PT_LOAD：RX code at 0x400000，RW data+BSS at 0x401000。
用户代码读 CS 并在 BSS 记录 CPL，检查 BSS 初始为零，确认无效 WRITE buffer
返回 INVAL、fork 返回 NOTSUP，随后记录 IF=1，运行跨越多个 tick 的循环，校验用户寄存器/栈保存，再通过 native WRITE=2 打印 `hello from elf`，
通过 EXIT=1 退出 0。任何检查失败则 exit 1。

## 验收日志与实际验证

新关键字：

```text
elf fixture
elf reject ok
elf load
elf rollback ok
exec spawn
exec ring3
user[10]: hello from elf
exec exit: pid=10 code=0
elf preempt: pid=10 preempts=N run_ticks=M cpu=0
elf user preempt verified: CPL=3 IF=1 ... competitor_runs=K registers/stack ok exit=0
elf exec ok
exec reap
elf lifecycle ok
phase2 selftest: PASS (0 failures)
```

PID 是运行时分配的，检查字符串时不应固定为 10。

hosted 验证 ELF 校验、段权限、BSS、地址边界、commit 拒绝回滚、串行 spawn 和
镜像回收；明确跳过 ring3 执行。freestanding 真实执行 ELF，校验 CPL=3、exit 0、
一次 WRITE/15 字节，以及最终页数/commit 回到基线。
新增同 QoS 内核竞争线程：只有在 ELF 被真实抢占且尚未退出时才增加
competitor_runs；测试要求 elf_preempts>0、elf_run_ticks>1、competitor_runs>0。
最多执行 512 次调度步，失败时终止并回收两个测试线程及镜像。

默认 hosted、四核 KVM 日志实际出现 `phase2 selftest: PASS (0 failures)` 与
`selftest result: PASS (0 failures)`。四核日志实际包含 `hello from elf`、CPL=3、
exit 0；phase0/phase1 与 `p3: phase 3 selftest PASSED (0 failure(s))` 保持通过。
另验证 hosted 五级 walker 和关闭 KPTI 变体。

```sh
make test
make kernel 2>&1 | grep -iE 'warn|error'
make iso
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -serial stdio -display none \
  -no-reboot -d cpu_reset 2>&1 | tee /tmp/capmix_run.log
```

内核 selftest 后 HLT，QEMU 的 timeout 124 状态是预期停止；以串口 PASS 为验收。
涉及用户态抢占恢复的 `usermode.c` 与 EEVDF 路径应人工审查。
`paging.c`、`syscall.S`、`switch.S`、IDT/IST、LAPIC timer 与 VMO 实现未修改。
真实 ELF 抢占验收使用 KVM；hosted 继续明确跳过 ring3 执行，验证加载、拒绝与回收。
