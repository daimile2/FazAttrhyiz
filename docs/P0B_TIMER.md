# P0b：LAPIC 1ms tick 标定

原问题：BSP 无 deadline 时用固定 initial_count=0x10000、假定 APIC bus
1GHz；AP 无 deadline 时不 arm，导致 AP 没有周期 tick。buddy 锁不修改。

TSC 参考频率先使用 CPUID.15 的有效 crystal/ratio；没有有效信息则 BSP
独占 PIT channel2 mode0，用 OUT2 terminal count 测量三个约 50ms 窗口，
取中位数。CPUID.16 nominal MHz 仅作为 PIT 失败后的显式 fallback，不是
首选 TSC 频率。没有假定 1GHz 的路径。

deadline 可用则保留原有 TSC-deadline 入口/ISR rearm。没有 deadline 时，
BSP 将 APIC 设为 masked one-shot、DCR=3（divide16），测五个约25ms 的
CCR/TSC 差值，至少三个有效样本取中位数。`calib_hz` 是分频后 countdown
频率；initial_count=round(calib_hz/1000)，不是未经测量的 bus 频率。
失败明确 panic，不能假装有已标定的调度时钟。

BSP/AP 都调用同一 configure_timer，AP 使用 BSP 发布的不可变参数，不
重新写 PIT。BSP 在启用 IRQ 时启动，AP 在各自初始化时启动。周期模式
由硬件 reload，ISR 不重新写 initial_count，避免周期额外漂移。
日志含 `tsc-deadline`、`calib_hz`、`initial_count`，每 AP 都有 armed 日志。

新增 debug 自测逐 CPU 收集128次 IRQ，首尾间有127个间隔。采样窗口内
不打印日志，结束后输出 elapsed_tsc、mean_ns、target_ns、error_permille；
每核平均误差严格小于5%才 PASS，三秒未完成则失败。误差以标定 TSC 为
时间参考，描述稳态平均 IRQ delivery；不保证每次IRQ jitter<5%，也不是
hrtimer、深 C-state/变频电源管理或独立 UTC 时间认证。早期 tick 日志可能
受日志/启动屏蔽 IRQ 影响，不能拿单个启动间隔代替稳态频率。

验收镜像：

```bash
make kernel
make iso LIMINE_CONFIG=boot/limine-selftest.conf
# 保留其他 host 功能，仅隐藏 deadline：
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 \
  -cpu host,-tsc-deadline -smp 4 -cdrom capmix.iso \
  -serial stdio -display none -no-reboot
# 再用 -cpu host 回归 deadline 路径。
make test
make iso                          # 最终恢复产品路径
```

VirtualBox 4 CPU、512MiB、serial file、selftest ISO：核对 CPUID 无 deadline、
BSP与三个AP相同的calib_hz/count、四个 `timer calib cpu=... PASS`、
`elf user preempt verified`、buddy soak、phase3 PASSED 和全量 PASS。
已有实测：VBox calib_hz=62492598（divide16）、count=62493；四核平均
999527/999882/999905/999879ns，最大平均误差0.0473%。QEMU无deadline四核
999932–999933ns；host deadline约1000988–1001429ns，均全量PASS。
这些是本次日志记录，不是所有宿主负载下的延迟保证。

寄存器/周期语义依据 [Intel SDM Volume 3A](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf)。
