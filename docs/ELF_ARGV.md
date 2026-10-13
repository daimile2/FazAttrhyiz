# 静态 ELF：argv/envp 与最小 crt

## 创建接口

`cm_spawn_attr_t` 新增尾部字段 `const char *const *argv`、`envp`。
当 `entry==NULL` 时走已有静态 ET_EXEC 加载/exec 启动路径，将两个 NULL
终止向量复制到新用户栈，再发布 runnable task。数组、字符串都不借用
调用者内存；spawn 返回后调用者可复用/修改自己的缓冲。

```
const char *argv[] = {"/bin/argtest.elf", "hello", "world", NULL};
const char *envp[] = {"MODE=demo", NULL};
cm_spawn_attr_t attr = {
    .path = "/bin/argtest.elf", .entry = NULL,
    .qos = CM_QOS_USER_INTERACTIVE, .argv = argv, .envp = envp,
};
cm_posix_spawn(&attr, &pid);
```

- argv 为 NULL 或显式空向量时，默认 `{path,NULL}`，保证 **argc≥1**。
- 显式非空向量完整保留，包括空字符串；argv[0] 可以由调用者指定，
  不影响实际加载的 `.path`。
- envp 为 NULL 时，传入空环境；不自动继承另一个进程的环境。
- 最多 32 项 argv、32 项 envp；每字符串最多 255B + NUL，两组字符串
  合计最多 **4096B（包括 NUL）**。越界/未在界内终止为 NOSPC。
- 临时快照只分配一个 4 KiB 页，所有成功/失败路径释放；参数不增加
  常驻用户页数，仍使用已有 4 页用户栈。
- 失败不修改调用者 out_pid，不占用已发布 ELF/进程槽，并回收已加载镜像、
  页表和 commit。构建参数失败也不修改已有 inactive image 的栈布局。
- native `entry!=NULL` C-entry 仍是 `void(void)`，不使用这两个字段。

当前 spawn 属性及 argv/envp 是**可信内核/native PID1 C 接口**，不是新增
用户指针 syscall ABI；普通 ELF 的 syscall SPAWN 仍 NOTSUP，替换式
execve 仍未实现。本轮是已有 path→ELF exec 启动路径传参，不引入动态链接。
所有调用者须重编译新增的 C 结构，初始化时使用指定字段或零初始化。

## 初始栈布局

x86_64 little-endian，每个下列单元均为 8 字节。进入 `_start` 时 RSP
16 字节对齐；除 RSP 外不承诺通用寄存器提供进程参数。

```
低地址：初始 RSP
    argc
    argv[0] -> 用户栈中的 NUL 字符串
    ...
    argv[argc-1]
    NULL                   argv 终止
    envp[0] -> 用户栈中的 NUL 字符串
    ...
    NULL                   envp 终止
    0                      auxv AT_NULL
    0                      auxv value
    0～15 字节对齐填充
    argv/envp 字符串（完整复制）
高地址：CM_ELF_STACK_TOP = 0x800000
```

因此 argv 地址为 `RSP+8`，envp 地址为 `RSP+16+8*argc`。auxv 本轮只有
AT_NULL/0，没有 AT_PHDR、AT_ENTRY、AT_RANDOM 等完整运行时信息。
初始布局总占用最多 4096 + 69*8 + 15 = 4663B，4 页栈余下空间供应用使用。
指针都是用户虚址，不是 HHDM/kernel 地址。向量可跨不同 backing 页，
写入通过现有 image-backed copyout 完成，不能假设 backing 物理连续。
`cm_elf_set_args` 只允许加载/发布前的私有 inactive image，不能用于运行中
改写用户栈。KPTI/CR3/iretq 入口设计保持不变。

## 最小 crt

`user/crt0.S` 是可复用的静态启动入口：

1. 清 DF、清 RBP；从 RSP 取 argc，计算 argv/envp。
2. 按 SysV x86_64 调用约定置 RDI/RSI/RDX。
3. 保持 CALL 前 RSP 为 16 字节对齐，调用 `main(int,char **,char **)`。
4. main 返回后，把 int 返回值放进 RDI，调用 CapMix EXIT syscall 1。

`_start` 没有 return address，不能直接 RET。此 crt 不初始化 libc、environ
全局变量、constructors、TLS、栈 canary 或动态解释器；main 必须使用
CapMix syscall ABI，不能直接链接宿主 Linux libc 认为它自动可用。
测试 C 编译使用 freestanding、static/no-PIE、no red zone、general-regs-only，
避免依赖未交付的用户 SIMD/FPU/TLS 运行时。

生成链：`user/crt0.S` + `tests/argv_main.c` + `tests/hello_elf.ld` →
`build/argv.elf` → `build/argv_elf.inc` → VFS `/bin/argtest.elf`。
共享测试 linker script 把 C `.rodata*` 放入既有数据 PT_LOAD，旧汇编 fixture
地址布局不变；这不是完整只读数据/NX 改造。测试 main 打印每项 argv/envp，
记录 CPL/argc/envc 并返回 0。示例输出每行最多 127B，较长项仅截断显示，
内核参数复制仍保留完整字符串。

## 产品 shell

只扩展已有 run 的词分割，不改 PS/2/FB/命令调度：

```
run /bin/argtest.elf hello world
run /bin/argtest.elf
```

第一例 argc=3，argv[0]=/bin/argtest.elf，argv[1]=hello，argv[2]=world。
第二例 argc=1。shell 附带固定环境 `PATH=/bin`、`CAPMIX=product`。
首版只按空格/tab 分词，最多 32 项（包括路径）；无引号、转义、变量展开、
环境赋值、管道或重定向。native spawn 可以传入空项或含空格字符串，
简单 shell 暂不能表达这些项。init 只补装缺失的 argtest，不覆盖已有版本。

## 已执行验收

- `/tmp/argv-hosted.log`：默认 argc=1、空字符串、envp、调用者改写后的快照
  隔离、跨页向量、项数/字符串/总容量拒绝、spawn 失败回滚；全量 PASS。
- `/tmp/argv-debug.log`：真实 CPL=3 main 打印 argv[0]/hello/空项/world 和
  `CAPMIX_TEST=argv`、`MODE=debug`；exit=0，全量 PASS。
- `/tmp/argv-product.log`：AHCI root=disk，带参/无参 shell run 均 exit=0、
  reclaim=0，文件 ELF 仍能校验已有文件并 exit=0。
- `/tmp/argv-network.log`：4 CPU、双网卡＋双 NUMA，TCP/UDP 用户 ELF、
  DHCP、P1a、buddy soak 与其余 phase 保持全量 PASS。

关键字：`argv[0]=`、`argv[1]=hello`、`envp[0]=`、
`argv/envp crt main verified`、`argv ELF verified: CPL=3 argc=4`、
`argv selftest: PASS`、`selftest result: PASS (0 failures)`。

本轮文件：proc/elf_args.c、proc/elf.c、proc/spawn.c、proc/init.c、
include/capmix/proc.h、include/capmix/elf.h、user/crt0.S、user/shell.c 的 run
参数解析、tests/argv_main.c、tests/hello_elf.ld、boot/argv_test.c、
boot/kernel.c、Makefile 及文档。未新增网络/syscall 编号，未改 buddy/时钟、
DHCP/TCP 算法、FB 或 KPTI 核心。

多 ELF 并发及 `run ... &`、CPU 亲和、回收和容量参见 [ELF_CONCURRENCY.md](ELF_CONCURRENCY.md)。
