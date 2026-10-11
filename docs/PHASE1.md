# 阶段 1：VMO / VMAR 零拷贝 channel IPC

## 对象和 API

声明见 `include/capmix/vm.h`。本次新增内核 API，没有增加用户态 syscall 编号，
也没有改变阶段 0 的 paging、KPTI 或 ring3 入口。

- `cm_vmo_create(task, bytes, &hid)`：创建 resident VMO，大小向页边界取整；
  当前每对象最多 128 页（512 KiB）。创建时 charge commit，每页由 node0 buddy
  分配并清零。metadata 同样由 buddy 分配，避免依赖当前不能回收的 kmalloc。
- `cm_vmo_size(task, hid, &bytes)`：需要 INSPECT，返回取整后的大小。
- `cm_vmar_create(task, base, bytes, &hid)`：创建任务所有的独立完整页表和有界
  低半地址区间。base/bytes 页对齐，base >= 64 KiB，区间位于 47 位用户地址内。
- `cm_vmar_map(task, region_hid, vmo_hid, va, offset, bytes)`：将 VMO 页直接映射
  到该任务 VMAR；没有整缓冲区 memcpy，也没有接收端 backing-page 分配。
  范围/offset/length 必须页对齐并在界内。最多 32 个映射记录，重叠返回 EXIST。
  要求 VMAR MAP 和 VMO READ|WRITE|MAP；首版只支持共享读写映射。
  不支持硬件只读、EXEC、COW、惰性缺页、压缩/换出或页迁移。
- `cm_vmar_acquire_space()` 返回 root、CR3 和一个独占所有权的 lease token。
  token 保持 VMAR 活着，任何 lease 存在时 map 返回 BUSY。调用者在全部 CPU
  离开该地址空间、恢复原 CR3 后调用 `cm_vmar_release_space()`；不要复制 token
  后重复释放。lease 不自动切 CR3，不绑定现有用户线程调度器。
- `cm_vm_space_pointer()` 用于持有 lease 时获取 backing page 直接指针，单次
  连续访问不可跨页。hosted 测试使用该适配器，不声称 Linux 进程真的建立了
  给定虚址的用户映射。freestanding selftest 直接使用各任务虚址和实际 CR3。

VMAR 构造和映射调用现有 mapper。映射失败只在本模块的私有、未使用 VMAR 中
清除新叶子、回收空页表；销毁只回收自有低半页表与 root，保留共享内核高半页表。
本 API 没有把新 VMAR 安装到 KPTI shadow；用户线程接入留待后续。
现有 spawn / security selftest 的占位 VMO/VMAR 不受影响，也不能冒充 backed 对象。

## channel 传递语义

`cm_ipc_msg_t.handles[]` 在发送时是发送任务的句柄 ID，接收时是接收任务新分配的
句柄 ID，二者可以不同。单消息最多 4 个句柄。

成功 send 的含义是入队完成，并将发送句柄的对象引用、原权限一起 move 到队列。
发送任务原句柄失效；已经建立的映射继续持有 VMO 引用。如需保留本地句柄，先
使用 DUPLICATE 创建副本，再发送副本。TRANSFER 是必需权限，接收权限不会扩大。
消息中的句柄数量过大、重复 ID、非法 ID、无 TRANSFER、队列满均不移动任何句柄。
禁止向一个 channel 发送该 channel 自己的 endpoint，以免形成自持有环。
不同 channel 间的任意 capability 引用环没有自动垃圾回收。

recv 在同一个事务内预留全部接收句柄槽位。槽位不足返回 NOSPC，输出不变，队列
仍持有整条消息；释放槽位后可重试。成功 recv 将队列引用直接交给本地句柄表。
队列、句柄和引用变更由同一 capability guard 保护；guard 不跨越上下文切换。

send 要求 endpoint WRITE，recv 要求 READ。对端关闭后 send 返回 DEAD；recv 先
交付已有排队消息，再返回 DEAD。接收 endpoint 最后一个引用关闭时，丢弃队列
并释放其中的 capability。两端都销毁后静态 channel slot 可再次分配。

## 创建、映射与传递顺序

典型调用顺序（每个调用都应检查返回码）：

```c
cm_vmo_create(sender, 65536, &vmo);
cm_vmar_create(sender, 0x1000000, 0x200000, &sender_region);
cm_vmar_map(sender, sender_region, vmo, 0x1000000, 0, 65536);
/* 获取 sender space lease，在发送方映射中填充缓冲区，离开该 CR3。 */
cm_ipc_msg_t message = { .nhandles = 1, .handles = { vmo } };
cm_ipc_send(sender, sender_endpoint, &message);
cm_ipc_recv(receiver, receiver_endpoint, &message);
cm_vmar_create(receiver, 0x2000000, 0x200000, &receiver_region);
cm_vmar_map(receiver, receiver_region, message.handles[0], 0x2000000, 0, 65536);
/* 获取 receiver space lease，直接读取 0x2000000 的共享页。 */
```

持有 VMO 的引用来源是创建者/句柄/排队消息/映射；最后引用释放时销毁 VMO，
释放 backing pages、metadata 并归还 commit。VMAR lease 可以在关闭 region handle
后继续保持映射活着；最后 lease 释放后销毁 VMAR，并释放各映射的 VMO 引用。

## selftest 和结果

`boot/phase1_test.c` 从 `boot/kernel.c` 的 IPC selftest 后调用，检查：

- 两任务不同虚址映射同一 16 页 VMO（64 KiB），每页物理地址相同。
- freestanding 分别装载双方 CR3，以虚址直接写入/读取全部内容；checksum 相同。
- receiver handle ID 重绑定、权限不扩大、发送句柄失效、接收端修改可由发送端读到。
- 映射固定 VMO 生命周期，关闭 VMO/VMAR 句柄后 lease 仍然有效，最终页数和 commit
  回到基线。
- 无权限、越界、重叠、active VMAR、重复句柄、队列满、接收槽位不足时整体保留及重试。
- sender close 后交付排队 VMO，receiver close 后回收未交付 VMO。
- 真实耗尽 buddy，检查 metadata/backing 分配失败和部分页表分配失败完整回滚；
  释放内存后映射重试成功。

新日志关键字：

```text
vmo transfer ok
vmo zero-copy
vmo lifecycle ok
vmo transfer rollback ok
vmo queued lifetime ok
vmo allocation rollback ok
phase1 selftest: PASS (0 failures)
```

本机默认 hosted 与四核 KVM 日志实际包含以上信息，以及
`selftest result: PASS (0 failures)`。四核 KVM 的 transfer 行记录
`bytes=65536 sender_va=1000000 receiver_va=2000000 sender_h=3 receiver_h=4`，
校验行记录 `shared backing=16 pages checksum=8355840 shared-write ok`。
phase0、KPTI/ring3、IPC RTT 与 phase3 全量测试仍通过。
QEMU selftest 后 HLT，30 秒 timeout 的 124 状态是预期停止方式。

验收命令：

```sh
make test
make kernel 2>&1 | grep -iE 'warn|error'
make iso
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -serial stdio -display none \
  -no-reboot -d cpu_reset 2>&1 | tee /tmp/capmix_run.log
```
