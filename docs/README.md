# CapMix 文档导航

当前能力以 [内核总览](KERNEL_OVERVIEW.zh-CN.md)、源码和后续功能文档为准。
第一次使用先读 [VirtualBox 测试手册](VIRTUALBOX_TESTING.zh-CN.md)；开发用户程序先读
[外部静态 ELF/SDK](EXTERNAL_ELF.md) 与 [示例](EXAMPLES.md)。

## 当前使用与接口

| 主题 | 文档 |
| --- | --- |
| 系统设计、接口、容量与状态 | [内核总览](KERNEL_OVERVIEW.zh-CN.md) |
| VBox 配置、产品/自测、验收清单 | [VirtualBox 测试手册](VIRTUALBOX_TESTING.zh-CN.md) |
| 磁盘安装与无 ISO 冷启动 | [INSTALL_DISK](INSTALL_DISK.md) |
| 用户态 init/shell、管理能力 | [USER_INIT_SHELL](USER_INIT_SHELL.md) |
| shell重定向、标准I/O、QEMU/VBox矩阵 | [SHELL](SHELL.md) |
| kill、关机等待与应急降级 | [KILL_SHUTDOWN](KILL_SHUTDOWN.md) |
| GCC SDK、crt、离线导入 | [EXTERNAL_ELF](EXTERNAL_ELF.md) |
| hello/nc/cat/ls/http-get | [EXAMPLES](EXAMPLES.md) |
| argv/envp 与栈布局 | [ELF_ARGV](ELF_ARGV.md) |
| ELF 并发、亲和、run& 与回收 | [ELF_CONCURRENCY](ELF_CONCURRENCY.md) |
| 用户文件 syscall/copyin/out | [P1A_ELF_FILES](P1A_ELF_FILES.md) |
| NIC UDP 与后台入队 | [P1B_UDP_NIC](P1B_UDP_NIC.md) |
| DHCP 与静态回退 | [P1C_DHCP](P1C_DHCP.md) |
| 最小主动 TCP | [P1D_TCP](P1D_TCP.md) |
| 用户 TCP/UDP ABI | [USER_NETWORK_ABI](USER_NETWORK_ABI.md) |
| SYN/数据/FIN 重传 | [TCP_RTO](TCP_RTO.md) |
| e1000/virtio-net RX IRQ | [NET_RX_IRQ](NET_RX_IRQ.md) |
| buddy SMP 安全与 soak | [P0A_BUDDY_SMP](P0A_BUDDY_SMP.md) |
| 无 TSC-deadline 时钟标定 | [P0B_TIMER](P0B_TIMER.md) |
| SRAT/SLIT 与节点分配 | [NUMA](NUMA.md) |
| 最小 EPT guest 隔离 | [EPT](EPT.md) |

## 历史与整理规则

原 `PHASE*.md` 与 `M2`–`M8` 文档统一移到 [history/](history/README.md)。
这些记录包含独有的接口、硬件 bring-up、负例和验收证据，因此本次不删除正文。
历史中的“未实现”“首版限制”是当时快照，不能覆盖后续续作的实际状态。
引用路径已同步更新；新的操作指南放顶层，阶段实现记录放 history。

本次为文档整理，不表示重新完成各平台全量验收；手册中的操作步骤与已实测证据分开描述。

- [用户态TCP/HTTP服务端与端口转发](HTTPD.md)

- [Linux ABI L0：双轨、开关、黄金ELF与边界](LINUX_ABI_L0.md)

- [Linux ABI L1：定制 musl BusyBox 文件子集与有界 BSS](LINUX_ABI_L1.md)

- [Linux ABI L2：驻留 COW fork、brk、替换式 execve 与记账](LINUX_ABI_L2.md)

- [Linux ABI L3：有界 IPv4 socket、能力生命周期与固定 ELF](LINUX_ABI_L3.md)
