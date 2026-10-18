# Linux 只读 PRIVATE 文件映射最小集

实现日期：2026-10-18。仅 Linux persona，默认 `capmix.linux=0` 不变。
这是预先拷贝到驻留页的只读文件快照，不是通用 Linux 文件 pager，也不新增原生 mmap ABI。

## 接口与限制

Linux x86-64 syscall 9，寄存器顺序仍为 `addr,len,prot,flags,fd,offset`。
支持 `prot=PROT_READ(1)`、`flags=MAP_PRIVATE(2)`，fd 必须是可读 FILE capability。
文件须为普通文件；目录返回 `-ENODEV(-19)`，无效/不可读/非FILE fd 返回 `-EBADF(-9)`。
可写或可执行文件映射、MAP_SHARED、MAP_FIXED及其他flags/prot组合返回 `-EOPNOTSUPP(-95)`。
匿名 PRIVATE RW 原有 `prot=3,flags=0x22` 语义保持不变。

- offset 必须4096字节对齐，len非0。offset在EOF或更后，或请求覆盖EOF所在尾页之后的整页，返回 `-EINVAL(-22)`。
- len向上取整4096。尾页文件EOF之后补零；len未对齐时同页其余有效文件字节也被读取。
- 地址hint/first-fit和 munmap 部分释放规则沿用匿名arena；没有固定地址覆盖。
- 文件读取使用pread，不改变fd seek位置。读取失败/短读返回 `-EIO(-5)` 并回滚映射及commit。
- 在VFS锁内取得内容快照。后续文件写入/截断不会更新映射；关闭fd不使映射失效。
- 页描述符和硬件页表均只读，copyout写入被拒绝。动态镜像的mprotect也不能把文件快照升级到可写/可执行。
  普通静态ELF既有NX兼容策略未变；不在此宣称所有静态只读数据页都硬件不可执行。
- 多线程组地址空间变更仍拒绝；不新增线程组并发mmap支持。

## 配额、fork 与 no-overcommit

窗口仍为 `0x400000..0x1400000`（16MiB），mmap arena 为 `0x800000..0x1400000`（12MiB）。
每镜像所有image/stack/brk/mmap有效页合计最多2048页（8MiB）。不是8MiB额外文件缓存。
映射前逐页charge并实际分配；数据页、PT、描述符或commit不足返回ENOMEM，回滚已获得资源。
快照不保留文件句柄、不依赖文件page cache，也不延迟到缺页才分配。

fork复用原有引用backing：文件快照只读共享，**不为这些只读页另收可写副本reserve**。
其他可写页仍按既有COW规则预占副本并收费，未改变no-overcommit纪律。
父/子可分别munmap；最后一个引用释放时返还数据页和commit。
本轮无可写文件COW；请求可写直接拒绝。resident-only限制不变，COW页仍不压缩/换出。
动态ld.so黄金继续使用既有匿名缓冲加载/重定位路径，没有改成通用文件mmap加载器。

## 探针与复现

`tests/linux_l5.S` 同时保留原1MiB匿名COW用例，并增加文件探针。
`make kernel` 按Makefile以固定链接脚本 `tests/linux_hello.ld` 构建并由 `scripts/mark-linux.py` 标记Linux ELF，
嵌入 `/bin/linux-mmap.elf`。产品Linux包补缺文件的既有策略不变：已有同名旧ELF不会被静默覆盖，
需由用户明确更新安装包/文件后运行本轮探针。

本轮构建 `build/linux-mmap.elf` SHA-256：
`166946117296648927c97ecd57c3afc19422bfd3ab2795aa42873c15833101c0`。
源码 SHA-256：`c62f5158e2589aa8ed139187ff7b2372717595f503020533be8be23b77bad443`。

探针会创建/覆盖专用 `/data/mmap-private.bin`（4096个A、904个B），请勿在该路径存放用户数据。
开启Linux后可 `run /bin/linux-mmap.elf`；需现有可写 `/data`。
检查正例和可写/shared/坏fd/非对齐offset负例、关闭fd后fork读取、munmap及exit=0。
机制测试另核对commit增加、拒绝过EOF整页、部分偏移、写入拒绝、mprotect升级拒绝、
fork仅为可写页收费、父先munmap子仍可读、最终页/commit基线恢复。

关键日志：

- `linux file mmap mechanism: readonly snapshot ... commit-limit verified`
- `linux file mmap reclaim: pages/commit baseline restored`
- `linux file mmap user: open/map/read ... rejected ok`
- `linux L5 selftest: PASS (0 failures)`

构建/检查命令：

```sh
make test
make kernel
make iso LIMINE_CONFIG=boot/limine-linux-l4-selftest.conf
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 -nic none \
  -cdrom capmix.iso -serial stdio -display none -no-reboot > /tmp/filemmap.log 2>&1
python3 tests/check_fd_table.py /tmp/filemmap.log
```

Linux开启矩阵的检查链覆盖L0、BusyBox-L1、L2/L3、hush、动态ld.so/TLS/W^X黄金，
以及线程复用/迁移；关闭矩阵保留原生路径。无网卡wire测试明确skip，不代表真实网络回归PASS。

## 验收矩阵（2026-10-18）

| 环境 | 结果 | 仓库证据 |
|---|---|---|
| hosted Linux开/关 | 全量0 failures；机制实际执行，ring3按hosted规则跳过 | [开](../tests/evidence/logs/filemmap-hosted-on.log)、[关](../tests/evidence/logs/filemmap-hosted-off.log)、[检查](../tests/evidence/logs/filemmap-hosted-on-check.log) |
| QEMU KVM SMP4 512MiB，Linux+interp开，无网卡 | 全量0 failures；真实文件映射探针exit=0；hush/静态/动态黄金通过 | [串口](../tests/evidence/logs/filemmap-qemu-on.log)、[检查](../tests/evidence/logs/filemmap-qemu-on-check.log) |
| QEMU KVM SMP4 Linux关，无网卡 | 全量0 failures；Linux用例明确skip，原生回归保留 | [串口](../tests/evidence/logs/filemmap-qemu-off.log)、[检查](../tests/evidence/logs/filemmap-qemu-off-check.log) |
| VBox 7.2.20 UEFI/PIIX3/AHCI DVD，SMP4 512MiB，Linux+interp开，无网卡 | 全量0 failures；真实探针和动态/静态回归通过 | [串口](../tests/evidence/logs/filemmap-vbox-on.log)、[检查](../tests/evidence/logs/filemmap-vbox-check.log) |

[内核构建](../tests/evidence/logs/filemmap-kernel.log)、[开启配置ISO](../tests/evidence/logs/filemmap-iso-on.log)、
[探针哈希](../tests/evidence/logs/filemmap-fixture-sha256.log)。本轮未测双NUMA或有网卡端到端旅程。
探针添加5000B内容时首版放入文本段，超过黄金链接脚本的0x401000数据段起点，链接器拒绝：
[原始失败](../tests/evidence/logs/filemmap-fixture-build-rejected.log)。已移入数据段再重建并跑上述矩阵，未忽略失败。

默认产品ISO保持Linux关闭，另验到达用户shell提示符：[产品串口](../tests/evidence/logs/filemmap-product.log)、[ISO构建](../tests/evidence/logs/filemmap-product-iso.log)。空闲30秒timeout=124，非全量selftest。
