# CapMix

Capability hybrid kernel. C11. x86_64 freestanding + hosted selftest.
Design doc: 混合选型清单（capability + L4 IPC + XNU split + EEVDF + COW FS）.

## Non-negotiables
- Skeleton is fixed: capability handles, FS/net in-kernel, drivers user-mode.
- Prefer posix_spawn; fork returns NOTSUP.
- Observation is capability-gated, not a global switch.
- Do not invent CPU registers / ABI; match arch/x86_64/*.

## After every change
1. Build what you touched.
2. Prefer `make test` (hosted) when possible.
3. Bare metal: follow QEMU serial path; success looks like:
   `selftest result: PASS (0 failures)`

## Critical paths (human review)
switch.S, syscall.S, paging CR3 load, IDT/IST, usermode iretq, lapic timer.

## Current status (approximate)
- Phase 0–3: solid (boot, IPC RTT, ring3, pressure+COW selftest green)
- Phase 4–7: mechanism demos; need real depth (drivers, net stack, crypto, VMX)
- Phase 8: tracing from day 1; expand coverage carefully



## Verify (mandatory after code changes)

```bash
make kernel 2>&1 | grep -iE "warn|error"
make iso
timeout 30 qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
  -cdrom capmix.iso -serial stdio -display none \
  -no-reboot -d cpu_reset 2>&1 | tee /tmp/capmix_run.log
