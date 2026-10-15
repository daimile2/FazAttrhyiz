# 历史阶段与产品里程碑

保留原实现过程、边界和当时的测试证据；当前状态请先看 [总览](../KERNEL_OVERVIEW.zh-CN.md)，
操作见 [VirtualBox测试](../VIRTUALBOX_TESTING.zh-CN.md)。

| 记录 | 原主题 |
| --- | --- |

| [M2_CONSOLE](M2_CONSOLE.md) | M2 Guest console |

| [M3_PERSIST](M3_PERSIST.md) | M3: virtio-blk and persistent COW root |

| [M4_VIRTIO_NET](M4_VIRTIO_NET.md) | M4: virtio-net, static IPv4, off-loopback TX/RX |

| [M5_INIT_SHELL](M5_INIT_SHELL.md) | M5: PID1 and the product shell |

| [M6_PRODUCT_BOOT](M6_PRODUCT_BOOT.md) | M6：产品启动与 VMware UEFI |

| [M7_AHCI](M7_AHCI.md) | M7：AHCI / SATA 最小存储 |

| [M8_E1000](M8_E1000.md) | M8：e1000 / e1000e 最小网络 |

| [PHASE0](PHASE0.md) | 阶段 0：页表 folding、KPTI 与 NUMA 桩 |

| [PHASE1](PHASE1.md) | 阶段 1：VMO / VMAR 零拷贝 channel IPC |

| [PHASE2](PHASE2.md) | 阶段 2：静态 ELF64 加载与 path-based posix_spawn |

| [PHASE4](PHASE4.md) | 阶段 4：IRQ capability 通道与用户驱动接收 |

| [PHASE5](PHASE5.md) | 阶段 5：lo IPv4 UDP 与用户可见 datagram 收发 |

| [PHASE6](PHASE6.md) | Phase 6: SHA-256 content verification |

| [PHASE7](PHASE7.md) | Phase 7: hardware virtualization minimum initialization |

| [PHASE8](PHASE8.md) | Phase 8: static tracepoints and capability reads |

| [PHASE9](PHASE9.md) | Phase 9: one VMLAUNCH and a VMCALL exit |
