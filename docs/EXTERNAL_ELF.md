# 宿主 GCC 编译外部静态 ELF，并拷入磁盘运行

## 一步构建与导入

前提：x86_64 宿主 GCC、make、Python3；已按 [磁盘安装](INSTALL_DISK.md) 创建 RAW
安装镜像，并至少启动一次到 shell、正常 poweroff，让 COW 根完成初始化。
关闭使用这个镜像的 QEMU/VMware；建议先备份。以下命令在仓库根执行：

```bash
make external-hello DISK_IMAGE=/path/capmix-system.img
```

它使用宿主 GCC 的 `-static` 构建 `build/external-hello.faz`，再将其写入磁盘 COW
根的 `/bin/external-hello.faz`。示例是**外部文件**，没有嵌入内核或开机 provisioning。
新路径必须不存在；已有路径拒绝覆盖。第一轮新安装盘尚未启动初始化时也拒绝导入。

重新启动同一个磁盘（可以不挂 ISO），在用户 shell 输入：

```text
run /bin/external-hello.faz host gcc argv
```

可观察到 `hello from external gcc static ELF`、
`argv[0]=/bin/external-hello.faz`、`argv[1]=host`、`argv[2]=gcc`、`argv[3]=argv`，
以及 `exit=0 reap=ok`。`run ... &` 沿用后台语义；示例打印后自行退出。
再次正常 poweroff/冷启动仍可运行该文件。

## 不经 Make 的 GCC 单命令

```bash
gcc -std=c11 -Wall -Wextra -O2 -static -nostdlib -no-pie \
  -ffreestanding -fno-builtin -fno-pie -fno-stack-protector \
  -mno-red-zone -mgeneral-regs-only -fno-asynchronous-unwind-tables \
  -Isdk/include -Wl,--build-id=none,-z,max-page-size=0x1000,-T,sdk/capmix.ld \
  sdk/crt0.S examples/hello.c -o /tmp/hello.elf
```

构建离线工具一次：`make build/cow-put`。导入任意新绝对路径：

```bash
python3 scripts/copy-to-disk.py /path/capmix-system.img /tmp/hello.elf /bin/myhello.elf
```

之后 shell `run /bin/myhello.elf first second`。开发自己的程序时只替换
`examples/hello.c`，保留 crt、链接脚本和上述选项；也可把 `sdk/` 复制到外部项目。
`readelf -h -l /tmp/hello.elf` 应显示 ELF64 / EXEC / x86-64 / PT_LOAD，不能有 INTERP。
`nm -u /tmp/hello.elf` 应为空；需要的辅助函数必须自己提供。

## 这不是 Linux 静态 glibc 兼容层

`gcc -static hello.c` 默认链接 Linux 的 crt/libc，并使用 Linux syscall、TLS 和
进程启动约定，因此不能直接作为 FazAttrhyiz 程序。这里的 `-nostdlib` 排除这些运行时；
`-static -no-pie` 生成无解释器的静态 ET_EXEC。没有 printf/malloc/errno/atexit，
没有 ld.so、共享库、动态重定位或完整 POSIX libc。请使用 SDK syscall wrapper。

`-mno-red-zone` 避免依赖栈下红区；`-mgeneral-regs-only` 避免当前用户上下文未提供的
SIMD/FPU 保存约定。不要自行启用 SSE/AVX、TLS、栈保护或 PIE。当前执行期可抢占、
支持多个固定 CPU 亲和 ELF；不是 Linux ABI 兼容执行环境。

## crt 与 argv ABI

`sdk/crt0.S` 与内核已有 `user/crt0.S` 使用相同入口约定：

- `_start` 的 RSP 为 16 字节对齐。`[rsp]` 是 64-bit argc；接着 argv 指针数组、NULL、
  envp 指针数组、NULL；字符串存在用户栈内。入口其他寄存器不是参数 ABI。
- crt 读取栈到 RDI/RSI/RDX，以 SysV x86_64 C 调用约定执行
  `int main(int argc, char **argv, char **envp)`。
- main 返回 int 后，crt 使用 FazAttrhyiz syscall **1 EXIT**，把返回值符号扩展到 RDI；
  不执行 libc 的析构/atexit。
- argc 至少 1；未传 argv 时加载器将镜像路径作为 argv[0]。shell run 传完整程序路径和
  空白分词参数；末尾独立 `&` 不传给程序。没有引号展开或重定向。
- argv 最多 32 项，envp 最多 32 项；单项最多 255B + NUL，两者字符串总计最多 4096B。
  shell 使用 256B 输入行；示例每条 argv 日志较长时截断显示，不改变原 argv。
- 产品 shell 当前给子程序 `PATH=/bin`、`CAPMIX=product`，不是完整继承任意宿主环境。

完整栈说明见 [ELF_ARGV.md](ELF_ARGV.md)。未提供 Linux auxv/TLS 初始化 ABI。

## 链接和映射约定

`sdk/capmix.ld` 入口 `_start`，代码起点 `0x400000`，数据/rodata 按 4KiB 对齐；
BSS 是 NOLOAD，并由内核清零。两个 PT_LOAD 标记分别为 R|X、R|W。
内核仅加载静态小端 ELF64、EM_X86_64、ET_EXEC，不支持 ET_DYN。

用户映射窗口 `[0x400000,0x800000)`；栈占最后 4 页，因此镜像 PT_LOAD 必须止于
`0x7fc000` 之前。最多 16 个 program header、128 个 backing 页（含 4 个栈页），
段大小/页数超限会被加载器拒绝。没有完整 NX/W^X 保证，不能将 ELF flags 当安全隔离
承诺。导入工具只复制文件，不提前证明其一定符合加载器所有条件；最终 run 会校验。

## syscall 最小约定

`sdk/include/capmix.h` 自包含，不依赖仓库内核头文件。

| 项目 | 定义 |
| --- | --- |
| 指令 | SYSCALL；RAX 为 FazAttrhyiz syscall 号 |
| 参数 | RDI、RSI、RDX、R10；第四参数不是 RCX |
| 返回 | RAX，负值为 FazAttrhyiz 错误码；不转换 errno |
| 编译器 clobber | RCX、R11、memory、cc |
| 1 EXIT | exit_code,0,0,0；crt 负责调用 |
| 2 WRITE | fd,用户地址,字节数,0 |

SDK `cm_print()` 使用 fd1 的现有诊断输出路径，分块每次最多 127B；输出同步到
serial/FB 内核日志，可能出现 `user[pid]:` 前缀。这是诊断流，不是 console capability，
普通 ELF 不获 shell 的管理权限。诊断 WRITE 成功返回请求字节数；SDK cm_print 将成功归一为 0，
失败保留负错误。fd2 为对应诊断输出。文件 fd 的 WRITE 则使用已有
fdio/FILE capability 检查与 copyin，语义见 [P1A 文件 ABI](P1A_ELF_FILES.md)。
网络、其他 syscall 仍以 `include/capmix/syscall.h` 与对应 ABI 文档为准，本小 SDK
未包装完整系统调用集。

## 离线导入安全边界

- 仅支持已初始化的 **FazAttrhyiz 安装 RAW 镜像**，沿用固定 MBR root/ESP 布局解析；
  不支持 VMDK/QCOW2、旧未分区数据盘、宿主真实块设备或未知盘。转换/备份需自行完成。
- 拒绝 symlink/hardlink/device、相对/含 `..` 的目标路径、已存在目标文件和缺失父目录。
  不自动建目录、覆盖文件或格式化根。宿主源文件须普通文件、非空、最多 1MiB；这是
  导入工具的文件上限，不代表 ELF 可映射满 1MiB。
- 同时取得 flock 与 POSIX 排他锁，拒绝被 QEMU 锁定的镜像。VMware 或其他工具未必
  使用相同锁，所以必须**关 VM 后导入**，不要绕过锁并发编辑镜像。
- Python 在镜像同目录制作完整 staging 副本，C helper 使用原 `fs/cowfs.c`、
  `fs/vfs.c`，仅允许读写 COW LBA 范围；不写 MBR、ESP、raw/swap。只 mount，绝不 mkfs。
- 写入/fsync 后重新 mount 并逐字节读回比较；成功才复验原镜像身份并原子 replace、
  fsync 所在目录。任何校验或 helper 失败丢弃 staging，原盘不变。需要额外一份镜像大小
  的宿主空闲空间；发布会替换宿主文件 inode，原路径和文件 mode 保留。
- `build/cow-put` 是 staging 内部适配器；正常入口是 Python 脚本，不能绕过 staging
  把它直接用于生产磁盘。宿主缓存/日志/页分配 shim 只在此工具中，未改内核核心。

## 验证

```bash
make build/cow-put build/external-hello.faz
python3 tests/test_copy_to_disk.py
# 可选已有、已关闭的初始化安装盘作只读 fixture；测试在临时副本写入：
CAPMIX_TEST_DISK=/path/capmix-system.img python3 tests/test_copy_to_disk.py
```

测试覆盖未知/空盘不格式化、非法路径、设备、symlink/hardlink、POSIX 锁拒绝。
另有跨进程测试确认原镜像 POSIX 锁在 staging 完成、调用 helper 时仍保持。
带 fixture 时还验证已有路径不覆盖，以及整个非 COW 区域逐字节未变。
真实验收使用上述 shell run，检查 argv 和 exit=0；hosted 自测不能替代 ring3 运行。

更多可安装的 `nc`、`cat`、`ls` 示例见 [EXAMPLES.md](EXAMPLES.md)。

标准I/O续作：未绑定fd1/fd2时上述诊断流语义保留；绑定fd1后WRITE走FILE，
SDK分块处理短写，`cm_report`摘要走fd2。SPAWN扩展和前台重定向见 [SHELL](SHELL.md)。

服务端扩展提供listen/accept syscall40/41与SDK包装，完整ABI及最小httpd用法见 [HTTPD.md](HTTPD.md)。

Linux静态ELF是独立可关闭persona，不能套用原生crt/syscall号；L0识别与构建见 [LINUX_ABI_L0.md](LINUX_ABI_L0.md)。新原生软件建议 `.faz` 或 `/bin/cm/`，已有 `.elf` 不强制改名。

Linux 文件兼容轨的固定 BusyBox echo/cat/ls 构建、64KiB BSS 限制及来源见
[LINUX_ABI_L1](LINUX_ABI_L1.md)。这是定制子集，不表示任意宿主 musl 静态程序可用；
原生 SDK、`.faz` 或 `/bin/cm/` 约定继续保留。
