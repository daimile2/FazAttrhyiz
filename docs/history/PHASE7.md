# Phase 7: hardware virtualization minimum initialization

> 历史阶段记录：保留当时实现与验收边界；后续功能可能已扩展。当前状态见
> [内核总览](../KERNEL_OVERVIEW.zh-CN.md)，操作测试见 [VirtualBox手册](../VIRTUALBOX_TESTING.zh-CN.md)。

`cm_virt_hw_probe()` (declared via `capmix/virt.h`) detects CPUID VMX and SVM.
Hosted builds only detect and return `CM_VIRT_HW_HOSTED`. Freestanding probes
are synchronous, BSP-only, with local interrupts disabled throughout; they do
not initialize other CPUs or run a guest. Existing `cm_vcpu_run()` remains a
synthetic accounting demo and VFIO attach remains bookkeeping.

Intel: check IA32_FEATURE_CONTROL lock/outside-SMX enable; leave unlocked or
disabled firmware policy untouched. Check IA32_VMX_BASIC region size, WB type,
physical-address restriction, CR0/CR4 fixed masks. Allocate a zeroed aligned
buddy page and write the revision id. Add required CR0 fixed bits (never clear
existing bits), set only CR4.VMXE, execute VMXON followed by VMXOFF, restore
CR0/CR4 and release the page. The region is ordinary kernel RAM through the
existing direct mapping; no new cache-management scheme is introduced.

AMD: check VM_CR.SVMDIS and existing EFER.SVME ownership. Save EFER and
VM_HSAVE_PA; allocate/zero an aligned host-save page, install VM_HSAVE_PA,
set EFER.SVME and verify it reads back. Restore both MSRs and verify their
values before freeing the page. No VMRUN, VMCB or NPT is implemented.

The probe copies the current IDT into a private BSP table, changes only its
#UD/#GP gates, temporarily loads it and restores the original IDTR. APs retain
the original shared table. Assembly preserves all general registers and uses
the existing KPTI entry macros unchanged. Recovery accepts only CPL0/BSP,
zero error code and an exact instruction RIP in the static fixup table.
Unmatched faults retain the normal report-and-halt path. An intentional UD2
and invalid RDMSR verify both recovery paths before initialization.

Results contain status, stage, exception vector, VMX CF/ZF status and rollback
state. No support, hosted privileges, firmware restrictions or refused
initialization yield an explicit skip; kernel selftest does not fail for those
environment restrictions. Recovery-check or rollback failures do fail the test.
A region still potentially owned by hardware is retained on rollback failure.
The API must not be called again following such a rollback failure.

`boot/phase7_test.c` checks an injected abort after region preparation and a
normal initialization attempt, buddy free-page count and CR4 restoration.
The implementation additionally verifies restored CR0/CR4 and AMD MSRs.
Logs include:
- `phase7: hardware detect vmx=... svm=... recovery_tested=...`
- `prepare abort rollback verified`
- `VMXON/VMXOFF minimum init ok` or `SVM EFER.SVME/VM_HSAVE_PA minimum init ok`
- `status=... reason=... stage=... fault_vector=... instruction_status=... rollback=ok`
- `hardware init skipped: ...` when unavailable
- `guest launch skipped: VMCS/VMCB guest state and EPT/NPT not implemented`
- `phase7 selftest: PASS (0 failures)`

Status values (enum in `virt_hw.h`): 0 initialized, 1 no support, 2 hosted,
3 firmware/ownership policy, 4 unsupported controls/region/recovery check,
5 allocation failure, 6 instruction/MSR exception, 7 VMX instruction refusal,
8 injected abort, 9 rollback failure. VMX instruction status: 1 CF, 2 ZF,
0xffffffff recovered exception. The stage identifies the failing operation.

References: original code follows [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
(VMX operation, capability MSRs, VMXON/VMXOFF) and
[AMD APM volume 2](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2)
(SVM initialization and VM_CR/EFER/VM_HSAVE_PA).

No guest launch, EPT/NPT page tables, hardware VFIO, Linux guest, verified boot
or SMP virtualization service is claimed. KPTI/VMO/ELF and phases 0–6 designs
are unchanged.
