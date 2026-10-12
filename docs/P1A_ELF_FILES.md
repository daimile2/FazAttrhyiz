# P1a：静态 ELF 文件 syscall

普通 CapMix x86_64 ET_EXEC 现在可以调用 `open/read/write/close/mkdir/unlink/fsync`。
这不是 Linux syscall ABI：RAX 为编号，RDI/RSI/RDX/R10 为参数，返回负的
CapMix 错误码；RCX/R11 被 SYSCALL 破坏。fork 仍返回 NOTSUP。

| 编号 | 调用与参数 | 返回 |
|---|---|---|
| 11 | open(path, flags) | fd（通常从 3 开始） |
| 12 | read(fd, buffer, bytes) | 实际读取长度，EOF 为 0 |
| 2 | write(fd, buffer, bytes) | 实际文件写入长度 |
| 13 | close(fd) | 0 |
| 14 | mkdir(path) | 0，已有目录为 EXIST |
| 15 | unlink(path) | 0 |
| 16 | fsync(fd) | 0 |

open 使用 `include/capmix/fs.h` 的 CM_O_*，不是宿主 libc 的全部标志。
RDONLY=0、WRONLY=1、RDWR=2、CREAT=0x40、TRUNC=0x200、APPEND=0x400。
非法访问模式或未知标志拒绝。fd 经当前进程的 fdio 表查到当前 task 的
FILE handle，read/write/fsync 分别检查 READ/WRITE/WRITE right；不能借用
其他 task 的 fd。进程退出关闭文件 fd。标准输出仍为既有日志适配，最多
显示 127 字节但返回请求长度，尚非完整 POSIX TTY。

## 用户指针与窗口

`proc/uaccess.c` 对普通 ELF 使用加载器记录的实际页 backing。读用户数据
要求 PF_R，写入用户缓冲要求 PF_R|PF_W。拒绝 NULL（非零长度）、整数
溢出、内核地址、未映射页、只读目的页。整个 I/O 缓冲先验证，再进行任何
文件读写；无效 read 不推进文件位置，无效 write 不修改文件。
路径先 copyinstr 到 128 字节内核数组，必须包含非空 NUL 终止字符串；
遇到未映射页或 128 字节仍未终止则 INVAL。合法终止符紧贴页末可接受。

每次 read/write 上限 **1 MiB（含）**；超过返回 INVAL，既不截断，也不自动
扩展旧的 1 MiB 嵌入 ring3 窗口。普通 ELF 校验使用自身映射，而不是该旧窗口：
当前加载器允许 0x400000–0x800000、最多 128 个驻留页（含栈），因此单个
连续合法缓冲还受实际页数限制。更大的文件可用多次调用流式处理。
零长度不访问缓冲，但文件 fd/权限仍检查。文件 I/O 使用一个 4 KiB buddy
临时页分块 copyin/out，不在内核栈放大缓冲；短读写返回实际完成量，后续
块发生错误时优先返回已完成量。镜像在进程 off-CPU reap 前固定，首版不
提供并发用户 unmap，因此本路径无用户映射撤销竞态。
可信 native C-entry 保留原有内核指针语义；未开放的 spawn/channel 等调用
仍被 ELF 白名单拒绝。此次没有改动 VMX/EPT、buddy 锁、时钟或硬件驱动。

## 生成、安装与运行

`tests/file_elf.S` 经 `tests/hello_elf.ld` 生成 `build/file.elf`，再由
`scripts/embed-elf.py` 生成 `build/file_elf.inc`。产品 init 独立补装缺失的
`/bin/filetest.elf`，已有 hello 或 filetest 不覆盖；更新已有测试程序时需要
自行更换该文件。本轮 root 可以是 RAM、virtio-blk 或 AHCI，无特别文件后端。

进入产品 shell：

```
run /bin/filetest.elf
cat /p1a/user.txt
poweroff
```

测试 ELF 验证 CPL=3、mkdir、负例指针、readonly FILE 写拒绝、4097 字节跨页
分块读写、fsync、close、unlink 与文件不存在。它先读取已有
`/p1a/user.txt` 校验内容，再写入并 fsync `CapMix P1a ELF file persistence`。
重新启动同一个磁盘后再 run，可看到 `p1a previous boot payload verified`。
没有该日志表示首次运行；内容损坏会 exit 1，不会先覆盖后掩盖损坏。

## 验收证据

- `/tmp/p1a-hosted.log`：`selftest result: PASS (0 failures)`；实际用户态执行 skipped。
- `/tmp/p1a-debug.log`：4 CPU，无盘 debug 全量 `PASS (0 failures)`。
- `/tmp/p1a-debug-ahci.log`：4 CPU，AHCI debug 全量 `PASS (0 failures)`。
- `/tmp/p1a-product-boot1.log` 与 `/tmp/p1a-product-boot2.log`：同一
  `/tmp/capmix-p1a-sata.img` 两次 AHCI 产品启动，root=disk、ELF exit=0；
  第二次含 `p1a previous boot payload verified`，cat 内容一致。

关键字：`p1a file syscalls verified`、`invalid-pointer rights chunk=4097`、
`p1a ELF file readback verified`、`p1a selftest: PASS`、
`p1a previous boot payload verified`。产品启动不执行全量 selftest，不能将
产品 shell 的 exit=0 当作全量 PASS；debug 使用 `boot/limine-selftest.conf`。
