# Phase 9: one VMLAUNCH and a VMCALL exit

This is a BSP-only, synchronous, trusted guest stub. It uses a normal 64-bit
paged guest with the current complete kernel CR3. EPT/NPT and unrestricted
real-mode guest support are not enabled. The guest therefore shares host
memory; this is not an isolation boundary or a general guest execution API.

`cm_virt_guest_once(mode)` adds an optional VMCS helper inside the accepted
VMXON lifecycle. `cm_virt_hw_probe()` keeps its phase7 probe-only behavior.
No CR3 entry/KPTI layout or scheduler/VMO/ELF design is changed. Non-VMX,
hosted, firmware-policy or unsupported-control environments explicitly skip.

`virt/vmcs.c` allocates an aligned buddy VMCS page and a guest stack, uses the
VMX_BASIC revision id with shadow bit 31 clear, and checks the VMCS physical
address restriction. Initial VMCLEAR and VMPTRLD have checked CF/ZF status.
Every VMWRITE is checked; failures report the field and VM-instruction error
when available. True control MSRs are used when VMX_BASIC advertises them;
otherwise legacy control MSRs are used. Values are adjusted with allowed masks,
including reserved bits that hardware requires to be one. Unimplemented
control dependencies are rejected rather than given invented state.

Explicitly initialized VMCS fields:

| Group | State |
|---|---|
| Guest | CR0/CR3/CR4; eight segment selectors/bases/limits/access rights; GDTR/IDTR; DR7; RIP/RSP/RFLAGS; interruptibility/activity/pending debug; SYSENTER; DEBUGCTL/PAT/EFER; all-ones VMCS link pointer |
| Host | Current CR0/CR3/CR4; ES/CS/SS/DS/FS/GS/TR selectors; FS/GS/TR bases; GDTR/IDTR bases; SYSENTER; PAT/EFER; assembly return RIP/RSP |
| Controls | Pin/primary execution, entry/exit controls; exception bitmap; page-fault mask/match; CR3 target count; MSR list counts; entry interruption/error/length; CR0/CR4 masks and read shadows |

Segments use the kernel's valid CPL0 long-code/data descriptors. A live host
LDT or unsupported selector layout is refused. Entry enables IA-32e guest mode
and loads PAT/EFER; exit enables the 64-bit host and reloads its PAT/EFER.
Host MSRs, descriptor tables and TR base are read from actual CPU state.
The VM-exit path's fixed DR7/DEBUGCTL effects are explicitly restored, as are
the original descriptor table limits. Outer cleanup restores original CR0/CR4,
IDTR and interrupt state using the phase7 path.

`arch/x86_64/vmcs.S` saves all SysV callee-saved registers on the host kernel
stack and programs HOST_RSP/HOST_RIP to that saved frame and `cm_vmexit_return`.
VMLAUNCH either returns CF/ZF failure directly, recovers a recognized #GP/#UD,
or enters the guest. Guest RIP points at `cm_guest_vmcall` in kernel text,
whose **first instruction is VMCALL** (0F 01 C1). The following UD2 is a fallback;
all guest exceptions are intercepted. VM exit loads the programmed host RIP/RSP
and returns through the saved host frame. No VMRESUME loop exists.

The helper reads EXIT_REASON, EXIT_QUALIFICATION and GUEST_RIP. A normal Intel
VMCALL exit has basic reason **18**. Reason bit 31 distinguishes late VM-entry
failure and is never counted as a real guest exit. `cm_virt_real_vmexits()` is
an independent real-hardware counter; existing per-VM `cm_vcpu_run()` counters
remain explicitly synthetic for compatibility.

Cleanup attempts VMCLEAR followed by outer VMXOFF. VMCS/guest stack/VMXON pages
are freed only after successful VMXOFF releases hardware ownership. On a
rollback failure, potentially referenced regions are retained and the failure
is reported rather than falsely claiming recovery. Ordinary initialization/
launch refusals with intact cleanup are explicit skips and do not fail the
machine selftest.

`boot/phase9_test.c` exercises:
- Mode 1: abort after VMPTRLD, then clear/off and recover all buddy pages.
- Mode 2: RFLAGS reserved bit 1 cleared, forcing a late invalid-guest-state
  entry failure. Observed reason 0x80000021 is logged `vmexit=not-real`.
- Mode 0: actual VMLAUNCH/VMCALL, reason 18, guest RIP at the stub and exactly
  one real exit counted.
- Every attempt: original free-page count and CR4, usable buddy allocation,
  and `kernel_alive=1`.

Success log keywords:
`vmcs init ok VMPTRLD ok`, `vmlaunch ok exit_reason=18`,
`qualification=0x0 guest_rip=0x... vmexit=real count=1 rollback=ok`,
`injected post-VMPTRLD abort cleanup verified`,
`invalid guest entry rejected reason=80000021 vmexit=not-real`,
`buddy=usable kernel_alive=1`, `phase9 selftest: PASS`.
Unavailable environments print `guest launch skipped` and `rollback=ok`.

Verification: make test, make kernel, make iso, standard four-CPU KVM run and
modern memory-backend-ram/memdev two-node run (docs/NUMA.md). A no-VMX KVM run
uses `-enable-kvm -cpu host,-vmx,-svm -smp 2`; this verifies a complete graceful
skip without TCG timing/emulation differences.

The requested `-cpu qemu64` TCG run was also tried. Phase9 skipped and passed,
but the complete suite failed two existing assumptions: phase3's p99 < 1 ms
latency bound and phase7's reserved RDMSR fault check. Their assertions were
preserved. That diagnostic log is `/tmp/capmix_tcg.log`; the verified no-VMX
KVM log is `/tmp/capmix_novmx.log`. Do not claim the TCG full suite passed.

Original implementation references:
[Intel SDM vol.3C](https://cdrdv2-public.intel.com/789585/326019-sdm-vol-3c.pdf)
(VMCS encodings, VM-entry/exit checks and state loading), and
[Intel SDM changes/exit-reason table](https://cdrdv2-public.intel.com/774475/252046-sdm-change-document.pdf).

Not implemented: EPT/NPT, arbitrary/untrusted guest code, guest memory isolation,
VMRESUME, multiple vCPUs, hardware VFIO, Linux guest boot, preemption timer or
verified boot.
