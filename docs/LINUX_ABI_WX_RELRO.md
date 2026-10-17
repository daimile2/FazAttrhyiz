# Linux 动态加载：W^X 与 GNU_RELRO 子集

本轮在仓库自有 ld.so 和最小 initial-exec TLS 之上，为真实动态加载路径默认应用
硬件用户页权限。不是完整 PaX、完整 Linux mprotect，也不扩大 glibc/动态链接兼容范围。
Linux persona 和解释器开关仍默认关闭；一旦启用真实 ld.so，没有关闭此加固的绕过开关。

## 默认加固范围与兼容例外

范围为 `CM_ABI_LINUX` 且使用 `/lib/ld-capmix.so.1` 的镜像（包括其 fork 子镜像）：

| 阶段/区域 | 用户页权限 |
| --- | --- |
| 主程序、解释器的可执行 PT_LOAD | RX；首次进入用户态前即不可写 |
| DSO 装载/重定位阶段 | RW + NX，不可执行 |
| DSO 完成重定位后的代码 | RX，不可写 |
| 只读 PT_LOAD | R + NX |
| 数据、栈、brk、匿名 mmap、TLS/TCB | RW + NX |
| DSO span 中未属于 PT_LOAD 的填充页 | R + NX |
| GNU_RELRO 约定页 | 重定位后 R + NX |

没有为此动态路径保留长期 W+X 用户叶子页。临时文件缓冲同样是 RW+NX，用完释放。
内核通过 HHDM 写入尚未运行镜像的 backing，不赋予用户可写的代码别名。
内核/HHDM 自身映射权限不在本轮全局加固范围内。

**明确兼容例外**：原生 CapMix ELF、嵌入 ring3、Linux 静态 L0–L5/hush、L4a 交接探针
保持原有权限语义，数据页可能仍可执行；本轮不宣称全系统所有用户进程都已实现 W^X。
这些路径 syscall 10 仍走原有 ENOSYS 计数行为，未修改原生 ABI 或静态内存约定。
原生后续软件仍约定 `.cmx` 或 `/bin/cm/`，不是 Linux persona 的别名。

NX 由 CPUID.80000001H:EDX[20] 检测。已有每 CPU usermode 初始化在保留 EFER 其他位的
基础上启用 NXE（bit 11）；未修改 CR3/KPTI 策略、入口汇编、buddy、时钟或 TCP/RTO。
CPU 无 NX 时真实动态镜像加载返回 NOTSUP，并打印 `NX unavailable`，不伪称 W^X 生效。

## 有界 mprotect ABI

仅真实动态 Linux 镜像新增 Linux x86-64 syscall **10**：
`mprotect(addr, len, prot)`。地址须 4096 对齐，长度正数，末尾向上取整，范围须全部已映射。
保持既有 16MiB VA 窗口、8MiB payload 上限及严格 commit 语义，没有新增隐式分配。

- 支持 `PROT_READ=1`、`READ|WRITE=3`、`READ|EXEC=5`。
- 包含 WRITE|EXEC 返回 `-EPERM=-1`，日志 `linux W^X rejected`。
- PROT_NONE、其他保护组合、仍有多个 backing 引用的任意页返回 `-EOPNOTSUPP=-95`。
- 非对齐、空长度、越界返回 `-EINVAL=-22`；范围含空洞返回 `-ENOMEM=-12`。
- 先预检全范围，再在现有 backing 锁/禁本地中断的临界区更新页描述符、私有 backing
  writable 属性和 PTE W/NX，并执行本地 invlpg。失败不留下前半段已改权限的状态。
- KPTI full/shadow 继续共享同一低半叶子页表；既有 CR3 切换无 PCID/NOFLUSH 复用，
  无需改 KPTI 策略。本轮 Linux 仍绑定 BSP 单线程，不提供跨核同地址空间并发 mprotect。
- COW 拆分/写回及 clone 保留 NX；共享页权限变更直接拒绝。独占页去写后，后续 fork
  不为其预留写副本；恢复 RW 后又按原有规则 reserve。已有 no-overcommit 纪律不变。

W^X 不等于禁止 W→RX 或 RX→RW 的时序转换，首版允许独占页这样转换。
RELRO 也不是不可撤销密封：进程显式 mprotect 独占页恢复 RW 在本子集中仍可成功，
与“启动时把重定位目标设只读”是不同承诺。没有新增不可逆 seal 或 JIT 授权系统。

## GNU_RELRO 约定

在全部 DT_NEEDED 装载、RELA 和 TLS 模板复制完成后，ld.so 应用各 PT_LOAD 最终权限，
然后处理主程序、全部 DSO 及解释器的 `PT_GNU_RELRO`：

- 每对象最多一个非空区域，filesz <= memsz，VA 加法不溢出。
- 声明区域必须完整位于同一个可读可写 PT_LOAD 内；禁止越界或覆盖可执行段。
- 起点依 GNU 页约定向下取整；**结束地址必须为 4096 整数边界**。
  首版不接受未对齐尾页，以免静默漏保护或误封相邻可写数据。
- 链接器应把待保护内容与后续 `.data/.bss` 分开到合适页边界。
  例如 GNU ld `-z relro -z now -z max-page-size=0x1000` 的当前黄金布局可用。
- 立即绑定，保护后不执行 lazy PLT 写回。重复/畸形/冲突声明 exit 126，输出
  `ldso: reject RELRO bounds/alignment`，沿既有路径释放库、TLS 和临时缓冲。
- **无 RELRO 的旧主程序/DSO允许运行**：仍应用 W^X，摘要明确显示 `no-relro=N`。
  这不是伪造 RELRO，也不要求重编所有旧程序。需要文本重定位的旧文件仍按既有策略拒绝。
- 请求 executable GNU_STACK 的动态对象拒绝；动态栈始终 NX。

## 旧解释器与磁盘升级

主程序/库没有 RELRO 与“旧 ld.so 不理解新的 NX staging”必须区分。
内核要求真实解释器包含 PT_NOTE：owner `CapMixWX\0`（namesz=9）、type=1、
4 字节 descriptor=1。它只是加载协议版本，不是签名或来源认证。
没有/重复/版本不匹配的标记，在进入用户态前返回 NOTSUP，打印：

`linux hardening rejected: stale ld.so (CapMixWX v1 required); update interpreter`

这避免旧加载器直接跳入 RW+NX 库导致未隔离页故障。供应函数仍只补充缺失文件，
不覆盖用户已有根文件。升级实验盘应停机并显式更新 `/lib/ld-capmix.so.1`，或创建新测试盘；
仅更新启动内核不会替换已存在的解释器。卸载 Linux 的数据保留/删除策略不变。
解释器带标记也不代表可任意信任：本内核目前并未提供通用恶意 ELF 故障隔离保证。

## 自测证据与限制

`boot/linux_ldso_test.c` 增加：

1. W+X mprotect 返回 EPERM；不对齐/不支持保护/含空洞的范围失败且原权限不变。
2. 独占 RW→R/RX，copyout 返回拒绝；共享 backing 权限变更拒绝；fork/COW 写分裂后仍有 NX。
   销毁后 free pages+cache、commit 回到基线，覆盖 reserve 纪律。
3. 成功动态及 TLS 程序退出后、镜像尚未销毁时，遍历用户页 PTE，检查没有 W+X、
   非代码均 NX；逐区域检查主程序、解释器及 DSO 的 RELRO W=0/NX=1，copyout 拒绝代码/RELRO。
4. 篡改库 PT_LOAD 为 W+X：exit 126；RELRO 尾地址改为未对齐：exit 126；
   删除库 GNU_RELRO：成功 exit 0、`RELRO=2 no-relro=1`；所有路径核对回收。
5. 篡改解释器协议标记：内核加载拒绝，页/commit 回滚；原动态/TLS 黄金仍 exit 0。

**没有实际执行故意非法 store 或 NX jump**：现有非 COW 用户 #PF 仍进 fatal reporter，
本轮未改异常/IST/iret 汇编或引入通用进程故障终止。权限证据是 CPL3 正常执行 + PTE 检查 +
copyout 拒绝 + 恶意映射请求拒绝，不能写成“恶意 ELF 页故障已隔离且内核总能存活”。

关键日志：

```text
linux W^X mechanism verified: RWX=EPERM ... COW NX retained
linux W^X loader protocol verified: stale interpreter rejected rollback=ok
ldso: W^X active objects=3 RELRO=3 no-relro=0
linux W^X/RELRO PTE verified: ... RX nonwritable data/stack NX copyout=denied
linux ldso wx-load: CPL3 exit=126 ... rollback/reclaim=ok
linux ldso no-relro: CPL3 exit=0 ... rollback/reclaim=ok
selftest result: PASS (0 failures)
```

## 复现与运行

现有六份固定 ELF/DSO 仍位于 `tests/fixtures/ldso/`。仅解释器内容/哈希更新，
主程序、普通库和 TLS 黄金未改；SHA-256 以该目录 `SHA256SUMS` 为准，正常构建核验。
源码为仓库原创；使用 GCC13.3.0/binutils2.42、固定 SOURCE_DATE_EPOCH/TZ 复现。
构建审计增加 CapMixWX v1 note 检查，继续检查真实重定位及无解释器自重定位/TLS。

```sh
scripts/build-linux-ldso.sh /tmp/cm-wx-build
sha256sum -c tests/fixtures/ldso/SHA256SUMS
cmp /tmp/cm-wx-build/ld-capmix.so.1 tests/fixtures/ldso/ld-capmix.so.1
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
# 使用既有 QEMU SMP4 串口命令；wire 回归另启宿主 echo/HTTP 服务。
python3 tests/check_linux_wx.py --wire /tmp/capmix_run.log
```

`--disabled`、`--interp-off`、`--hosted` 适配其他开关配置；递归检查 TLS、hush、L0–L5。
hosted 仅运行权限/COW/记账模型测试，不能证明 CPL3 或硬件 NX 执行。

## 环境矩阵

2026-10-17，Ubuntu 宿主，QEMU/KVM 与 VBox 7.2.20；仅记录实际结果。

| 环境 | 结果 | 日志 |
| --- | --- | --- |
| 两个干净构建目录，GCC13.3/binutils2.42 | 六份黄金逐字节一致、SHA256/ELF 审计通过 | `/tmp/wx-fixture-build.log`、`/tmp/wx-reproduce.log` |
| hosted：Linux关、Linux开/interp关、双开 | 三份全量 `PASS (0 failures)`；双开执行权限/COW/旧加载器拒绝模型测试，CPL3 跳过 | `/tmp/wx-hosted-{off,static,on}.log` |
| KVM SMP4，双开，e1000 | 全量 `PASS (0 failures)`，PTE/权限/RELRO/TLS 正负例及实际网络链通过 | `/tmp/wx-on-e1000-matrix.log` |
| KVM SMP4，双 NUMA，virtio-net+e1000 | 全量 `PASS (0 failures)`，相同安全/静态/原生回归通过 | `/tmp/wx-on-dual-vnet-matrix.log` |
| KVM SMP4，双开，无网卡 | 全量 `PASS (0 failures)`，网络按既有策略 skip | `/tmp/wx-on-no-nic-matrix.log` |
| KVM SMP4，Linux开/interp关 | 全量 `PASS (0 failures)`，动态拒绝，静态/原生及网络回归通过 | `/tmp/wx-interp-off-matrix.log` |
| KVM SMP4，Linux运行时关 | 全量 `PASS (0 failures)` | `/tmp/wx-linux-off-matrix.log` |
| KVM SMP4，编译 Linux关，双节点/两网卡 | 全量 `PASS (0 failures)` | `/tmp/wx-compiled-off-matrix.log` |
| 无 ISO 产品盘，virtio-blk/e1000 | 动态/TLS 前台后台 exit=0，hush、L0/L5、原生文件重定向、nc/http-get/httpd、关机通过 | `/tmp/wx-final-product-first.log`、`/tmp/wx-final-httpd-client.log` |
| 同产品盘无 ISO 冷启动 | 动态/TLS exit=0、持久文件 cat、hush、关机通过 | `/tmp/wx-final-product-second.log` |
| 默认产品 ISO，Linux关闭 | 用户 shell、原生 hello exit=0、ACPI 关机通过 | `/tmp/wx-default-product.log` |
| VBox EFI64/PIIX3/AHCI/SMP4，无 ISO 首次与第二次启动 | 两次动态/TLS 明确打印 W^X/RELRO 且 exit=0；前后台、L0、hush、原生文件持久读回、关机通过 | `/tmp/wx-vbox-first.log`、`/tmp/wx-vbox-second.log` |

QEMU 六份全量日志均由 `tests/check_linux_wx.py` 检查，启用网络的 Linux 配置额外使用
`--wire` 强制实际 UDP/TCP 回归。phase0–9、NUMA、TRACE 等随全量通过。
VBox 仅为产品验收，其 e1000 显示 down，不宣称 VBox 网络或全量 debug PASS。
测试使用单独 `CapMix-WX-RELRO-20261017` 虚拟机与 `/tmp/wx-product.vdi`，未修改旧 VM。
无 NX CPU 的拒绝策略有实现，本轮没有在禁 NX 的 CPU 模型下执行完整验收。

## 文件清单

- `arch/x86_64/usermode.c`、`include/capmix/arch.h`：NX 探测及每 CPU NXE 初始化。
- `proc/elf.c`、`include/capmix/elf.h`、`abi/linux.c`：动态页权限、有限 mprotect、NX/COW 保留、旧加载器拒绝。
- `user/linux/ldso/{ldso.c,start.S}`：最终 PT_LOAD/RELRO 权限及协议 note。
- `boot/linux_ldso_test.c`、`tests/check_linux_{ldso,wx}.py`：安全正负例、PTE/记账和回归链。
- `scripts/check-ldso-fixtures.py`、`tests/fixtures/ldso/{ld-capmix.so.1,SHA256SUMS,README.md}`：新固定解释器/审计。
- 本文、TLS/ld.so/总览及导航：当前范围、兼容例外与环境证据。
