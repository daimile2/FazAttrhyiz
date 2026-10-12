# M3: virtio-blk and persistent COW root

The kernel discovers PCI vendor 1af4/device 1001 and uses the transitional
legacy I/O BAR transport. The queue and block request ABI follow the
[OASIS Virtio 1.2 specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html),
sections 2.7.2, 4.1.4.10, and 5.2. This first driver supports one device,
one split queue of 3..256 descriptors (power of two), and one synchronous
request at a time. It negotiates FLUSH, requires writable media, disables INTx,
and polls the used ring with release/acquire barriers. There is no interrupt
completion, modern-only PCI/MMIO transport, IOMMU, hotplug, or NVMe support.
Existing M2 console code and VMX/EPT/TRACE/NUMA cores are unchanged.

DMA uses owned physically contiguous buddy allocations: a 16 KiB queue region
and an 8 KiB request/bounce region. Each transfer uses at most 4096 data bytes;
larger block requests are split. User pointers and arbitrary kernel stacks are
never handed to hardware for DMA. Capacity/ranges, completion head/index/length
and device status are validated. Request polling has a 20,000,000-iteration
bound; reset polling has a 100,000-iteration bound (not a wall-clock timeout).
On timeout/invalid completion the driver resets, stops bus mastering and returns
an error. DMA pages are freed only after reset acknowledges; otherwise they are
quarantined and the device cannot be reinitialized this boot. This conservative
failure path may retain pages rather than risk DMA into recycled memory.

## Create a test disk and boot twice

Use a new disposable raw image, at least 8 MiB; the example uses 32 MiB:

```
truncate -s 32M capmix-root.img
make kernel
make iso
make -B test

timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -drive if=virtio,format=raw,file=capmix-root.img,cache=writeback \
  -cdrom capmix.iso -serial stdio -display none -no-reboot \
  2>&1 | tee /tmp/capmix_m3_first.log

# Reuse the SAME image; do not truncate or recreate it.
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -drive if=virtio,format=raw,file=capmix-root.img,cache=writeback \
  -cdrom capmix.iso -serial stdio -display none -no-reboot \
  2>&1 | tee /tmp/capmix_m3_second.log
```

Standard QEMU PC `-drive if=virtio` supplies the supported transitional device.
For an explicit attachment use `-drive if=none,id=root,format=raw,file=...`
with `-device virtio-blk-pci,drive=root,disable-modern=on`.
Modern-only device 1042, read-only disks, insufficient capacity, unsupported
queue sizes, or missing FLUSH produce a clear skip and use the existing ramdisk.
No virtio device and hosted execution also skip hardware without failing the
existing selftests. OVMF is optional; the M2 UEFI ISO/FB support remains usable.
The host `-display none` does not remove the virtual VGA; use GTK/VNC for logs.

## Volume and durability semantics

The on-disk format is the existing COWFS, not a new filesystem. The first 8 MiB
retain the fixed layout from io.h: sectors 0..63 raw, 64..8255 swap,
8256..16383 COWFS (1016 4 KiB blocks). Only the COWFS window is used on the
persistent device; capacity beyond the reserved 8 MiB is not used. There is
no partition table, installation procedure, or full-capacity volume sizing.

At boot, a valid COW volume is mounted as `/`. Auto-format is allowed only
when the entire reserved 8 MiB window is zero. An unknown/nonempty or damaged
volume is refused; it is not silently overwritten. Such a present-but-invalid
root reports an M3 failure, while scratch tests can still run. An I/O failure
never causes reads/writes on the selected virtio backend to silently use RAM.

COW commit flushes new data/metadata before writing the alternate superblock,
then flushes the superblock before accepting that generation in memory.
fsync/sync also flush when no new metadata is dirty. RAM flush remains a no-op
for online ramdisk; offline returns DEAD. Existing transaction batching is
unchanged: sync inside an open COW transaction does not prematurely commit it.
Durability relies on the QEMU backend/host honoring FLUSH and the existing
COWFS sector-write/checksum assumptions; this is not a new power-failure proof.

## Selftest isolation and observables

boot/m3_test.c creates a 4097-byte deterministic binary file `/m3-persist.bin`,
checks sync, drops the COW inode/indirect/page-cache state using cm_cow_mount(),
and reads the file again from actual virtio sectors. Before modifying a marker
on later boots it verifies the previous content. The marker's prefix is
`CapMix M3 persistent root v1`, followed by deterministic binary bytes.

The default boot still runs all earlier selftests. Their root is explicitly
switched to a fresh scratch ramdisk: phase3 modifies raw sector pointers and
assumes empty test paths; phase4 deliberately offlines its bound test device.
After those tests, with no open files/I/O pending, the controller restores `/`
to the virtio volume and verifies the marker again. These backend changes are
boot/test-controller operations, not general runtime mount/device management.
Ramdisk and virtio offline flags are independent. The console's final idle loop
has the persistent root selected. ELF/phase3 scratch fixtures are not copied to
the persistent root; this delivery persists the M3 file and any existing root
content rather than provisioning a production `/bin` installation.

Expected logs:

```
virtio-blk PCI
virtio-blk ready: ... FLUSH=1
cowfs: blank virtio-blk volume — formatting   # first boot only
m3 mount: '/' on virtio-blk
m3 persist first boot: creating marker
m3 persist: write -> sync/FLUSH -> remount -> read verified bytes=4097
m3 mount restored: '/' virtio-blk persist verified
m3 persist selftest: PASS (0 failures)
selftest result: PASS (0 failures)
```

The second boot must additionally print `m3 persist previous boot verified`.
Without a supported device: `virtio-blk skipped` and `m3 persist skipped`,
followed by the normal full PASS. Hosted validates the existing RAM/COW path
with the new flush hooks and explicitly skips real PCI/DMA.
