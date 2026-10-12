# M4: virtio-net, static IPv4, off-loopback TX/RX

The kernel supports the first PCI vendor 1af4/device 1000 NIC with a usable
legacy I/O BAR (revision 0, MSI-X disabled). Modern-only device 1041 is skipped.
The implementation follows the [OASIS Virtio specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html),
legacy PCI/split-ring and network framing sections. It uses RX queue 0 and TX
queue 1, each 2..256 descriptors, power of two; eight RX buffers maximum and
one TX in flight. MAC and optional STATUS are the only negotiated features.
If STATUS is absent the link is assumed up and that limitation is logged via
`status_feature=0`. With STATUS, link state comes from device configuration.

Header and Ethernet data use separate descriptors: the legacy net header is
10 bytes because VERSION_1/MRG_RXBUF are not negotiated. No checksum/GSO/TSO
or mergeable-buffer offload, packed rings, multiqueue or interrupt completion
is implemented. PCI INTx is disabled and queues request no interrupts.

Each queue uses a 16 KiB buddy allocation. RX buffers and TX bounce buffer are
individual owned 4 KiB pages; arbitrary user/kernel stack addresses never go
to hardware. Polling uses release/acquire barriers and checks RX completion
head, length and backlog; TX completion head/index are checked. Invalid DMA
completion or TX timeout resets the device and stops bus mastering. Pages are
freed only after reset acknowledgement; otherwise they are quarantined and
reinitialization is refused this boot. TX polling has a 20,000,000-iteration
bound, reset 100,000 iterations; these are iteration limits, not wall clocks.

## Static network and minimal protocol

`vnet0` is configured as 10.0.2.15/24, neighbor/gateway 10.0.2.2. This is the
standard QEMU user-network subnet. No DHCP or general route table exists.
A single neighbor is learned through validated ARP. Minimal IPv4/ICMP supports
an echo probe/reply for this peer, including Ethernet addresses, total length,
fragment rejection, IP/ICMP checksums, echo id/sequence and exact payload checks.
ARP and IP ingress are first passed through the existing XDP -> nft classifier;
only PASS proceeds to the protocol. There is no hardware implementation of
XDP_TX/REDIRECT or new outbound filter path in this milestone.

The selftest is driven by bounded BSP polling: at most three ARP requests and
three echo requests, each with at most 2,000,000 RX polls. Link-down or an
unreachable configured peer yields an explicit test failure, not a hang or a
fake successful RX. No supported NIC yields skip+PASS. A missing MAC feature,
unsupported queue, missing HHDM, or unsupported transport also skips with a
reason. Hosted skips PCI/DMA but runs codec validation (including checksummed
wrong payload, corruption, truncation and fragments).

The raw kernel RX/TX APIs remain available; no background RX thread or socket
interface for this NIC is added. ARP/ICMP service runs during selftest polling.
The existing capability UDP syscall/API stays lo-only, with its original
256-byte, nonblocking semantics. No TCP, DHCP, user NIC runtime, general ICMP,
IPv4 options, fragmentation/reassembly, VLAN or Internet connectivity is claimed.

## QEMU validation and M3 persistence

Use a NEW disposable image for the first invocation; do not truncate an existing
M3 image. The second invocation must reuse the same disk unchanged:

```
# Only when the image does not already exist:
truncate -s 32M /tmp/capmix-data.img
make kernel
make iso
make -B test

timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso \
  -drive file=/tmp/capmix-data.img,if=virtio,format=raw \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -serial stdio -display none -no-reboot \
  2>&1 | tee /tmp/capmix_m4_first.log

# Repeat the same QEMU command without recreating/truncating the disk.
```

If firmware/machine configuration selects modern-only transport, explicitly
use `-device virtio-net-pci,netdev=n0,disable-modern=on`. This milestone supports
legacy/transitional PCI only. Do not change the user netdev subnet/host address
without also updating this milestone's fixed IP/peer configuration.

[QEMU's network documentation](https://www.qemu.org/docs/master/system/devices/net.html)
uses ping to 10.0.2.2 to check user networking. This test targets that local
virtual router, not an Internet host and not a fabricated kernel loopback.
Packet evidence can be captured with:

```
-object filter-dump,id=capture,netdev=n0,file=/tmp/capmix-net.pcap
```

The capture should contain an ARP request/reply and IPv4 ICMP type 8/type 0,
both carrying `CapMix virtio-net off-lo payload!` (33 bytes). TX/RX counters
increase only after TX completion and real ingress respectively. The ingress
counter includes frames later dropped by policy/validation.

Without a NIC, omit virtio-net and use `-nic none`: `virtio-net skipped` and
`m4 virtio-net skipped` appear with full PASS. M3 still mounts/verifies the disk
and runs its legacy destructive tests on scratch RAM. M2's framebuffer/keyboard,
blk/COW persistence, VMX/EPT/NUMA and TRACE cores are unchanged.

Expected successful logs:

```
virtio-net PCI
virtio-net ready: ... link=up ...
virtio-net ip: vnet0=10.0.2.15/24 gateway=10.0.2.2
m4 codec verified
virtio-net ARP rx: 10.0.2.2 neighbor verified
virtio-net tx/rx payload verified: ... bytes=33 tx_delta=2 rx_delta=2 (off-lo)
m4 virtio-net selftest: PASS (0 failures)
m3 mount restored: '/' virtio-blk persist verified
selftest result: PASS (0 failures)
```

A second boot also prints `m3 persist previous boot verified`. More ARP traffic
or retries can produce counters above 2; only monotonic increases and matching
payload/echo fields are required. No production TCP/IP stack is implied.
