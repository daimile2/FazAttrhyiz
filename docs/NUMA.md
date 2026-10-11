# NUMA topology and allocation balancing

Topology is discovered after the initial kernel CR3 load, before AP startup.
SRAT type 0 (LAPIC), type 2 (x2APIC), and type 1 (memory) provide affinities;
SLIT supplies distances, with 10 local/20 remote defaults if absent or invalid.
RSDP/root/SDT checksums, table and entry lengths, arithmetic overflow, duplicate
CPU IDs and overlapping memory ranges are checked. Unknown SRAT records are
skipped. Sparse proximity domains map to dense internal node IDs. The domain
containing the existing primary arena is node0; its arena stays unchanged.
Absent/invalid topology or a primary arena crossing affinity boundaries falls
back to one node0, with explicit INF logs.

Limine usable memory is snapshotted before its page tables are replaced. Only
usable RAM intersecting enabled SRAT memory ranges may join the allocator.
The largest intersection for each remote node contributes at most 64 MiB.
Ranges are mapped through the existing kernel HHDM mapping interface before
buddy registration. No CR3 transition or KPTI layout changes are made.
Bounds: 8 nodes, 64 affinity ranges, 64 CPU affinity records, 128 cached usable
pieces. Missing or excessive topology falls back safely. Memoryless nodes are
reported; they cannot satisfy strict allocations.

Buddy keeps free lists, counts and coalescing boundaries per arena under the
existing global lock. Coalescing uses absolute block alignment (matching initial
splitting) and never crosses arena/node boundaries. Global free/total pages and
commit backing include added RAM; extending backing preserves existing charge
and swap backing. Existing pressure watermarks retain their initial-pool values.

Ordinary `cm_page_alloc(order)` rotates across nodes, skipping nodes that cannot
satisfy the requested order. If a selected node is exhausted or memoryless,
it tries all remaining nodes. Thus existing callers automatically spread new
allocations across real node memory. `cm_page_alloc_node(node, order)` is strict:
it never returns another node's memory. `cm_page_node(ptr)` identifies ownership;
`cm_numa_stats` reports actual arena counters, including zero for memoryless
nodes. Binding remains explicit task metadata used with the node API. It does
not migrate existing mappings or silently constrain ordinary allocations.
Single-node selection and strict node0 behavior are retained.

The balancing algorithm spreads new allocations; there is no page migration,
background rebalancing daemon, memory hotplug or NUMA scheduler migration.

Boot tests verify CPU masks, local distances, strict allocation, actual multiple
node ownership, payload writes, per-node free/block accounting, and complete
recovery. With remote memory, one node is exhausted to verify strict failure and
automatic remote fallback. Phase0 invalid-node checks use the discovered count.
For multi-node pressure tests, actor kernel stacks are prepared before the RAM
balloon, avoiding high-order allocation failures from fragmented free pages.
Anonymous payload allocation, L1-L4 pressure, checksums, kill ordering and
teardown assertions stay intact; single-node pressure setup is unchanged.

Mitigations log actual CR4 SMEP/SMAP, and KPTI verified by real BSP user shadow
CR3 entries. Bits: KPTI=0x01, SMEP=0x02, SMAP=0x04. Early boot explicitly says
KPTI awaits verification; after ELF/ring3 use it reports the verified bit.
Hosted never claims hardware KPTI. KASLR is `unimplemented`, `kaslr_slide=0`.
No SMEP/SMAP enabling or random relocation is added.

Checks:
```
make numa-test
make test
make kernel 2>&1 | grep -iE 'warn|error'
make iso
```
Use the usual KVM command for single-node fallback. For two actual nodes and a
nondefault SLIT distance:
```
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -object memory-backend-ram,id=mem0,size=256M \
  -object memory-backend-ram,id=mem1,size=256M \
  -numa node,nodeid=0,cpus=0-1,memdev=mem0 \
  -numa node,nodeid=1,cpus=2-3,memdev=mem1 \
  -numa dist,src=0,dst=1,val=21 \
  -cdrom capmix.iso -serial stdio -display none -no-reboot -d cpu_reset \
  > /tmp/capmix_numa2.log 2>&1
```
Look for `SRAT detected 2 nodes`, `SLIT distances validated`,
`distance node0->node1=21`, `visited_mask=3`,
`exhaustion fallback/recovery verified`, `numa selftest: PASS`,
`mitigations=1 SMEP=0 SMAP=0 KPTI=1`, and the full selftest PASS.
Actual SMEP/SMAP values depend on CR4; zero is not a probe failure.
The timeout exit after a completed, halted selftest is expected.

Parser layout follows the [ACPI specification, SRAT/SLIT](https://uefi.org/specs/ACPI/6.6/05_ACPI_Software_Programming_Model.html).
`make numa-test` independently covers sparse domains, LAPIC/x2APIC IDs,
primary-domain reorder, disabled affinity, truncated/corrupt records,
overlap and address overflow. It never modifies a running kernel's topology.
