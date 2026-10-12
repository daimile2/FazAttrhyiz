# Minimal EPT for the phase9 guest

The guest now uses private four-level first-stage identity paging and a separate
four-level EPT. It no longer shares host CR3. Only seven explicitly allocated
4 KiB guest backing pages are accessible through EPT; the EPT tables and VMCS
are not mapped into the guest. All other GPA leaves are absent.

| GPA / guest virtual address | Backing | EPT permission |
|---|---|---|
| 0x1000 | Copied VMCALL or violation-test bytes | Read/execute, no write |
| 0x2000 | Dedicated stack, RSP=0x2ff0 | Read/write, no execute |
| 0x3000–0x6000 | Private guest PML4/PDPT/PD/PT, CR3=0x3000 | Read/write, no execute |
| 0x7000 | Private GDT/TSS and empty IDT region | Read/write, no execute |
| 0x8000 | First-stage PTE present, deliberately no EPT mapping | No access |

The first-stage PTEs set accessed/dirty bits and point to fixed GPAs, not host
physical addresses. EPT provides GPA-to-buddy-HPA translation. Execute denial
is enforced by EPT independently of guest PTE NX. Code is also read-only in
the initial guest PTE. Normal guest RIP=0x1000; its first instruction is VMCALL.
The unmapped-read test executes MOVABS RAX,0x8000 then MOV RAX,[RAX], so the exit
is an EPT violation rather than a guest page fault.

VMCS primary controls enable secondary controls; secondary controls enable
only EPT. EPTP is WB memory type (6), four-level walk (length encoding 3),
accessed/dirty tracking disabled. Leaf entries use WB and explicit R/W/X;
all non-leaf EPT links are R/W/X. The implementation uses 4 KiB leaves, not
large pages that would expose adjacent host memory. Guest CR4 clears LA57 and
PCID for the private four-level root and is checked against fixed masks.
Host state, phase7 lifecycle and phase8 trace are unchanged.

Capabilities are checked before allocating tables: secondary EPT, EPT four-level
walk, WB, INVEPT and either single-context or all-context INVEPT. Missing support
causes an explicit skip. INVEPT prefers type 1 (descriptor contains EPTP), with
type 2 (zero descriptor) as the capability-based alternative. The 16-byte
aligned descriptor has reserved bits clear. Every instruction has checked
CF/ZF and a restricted #GP/#UD recovery entry.

After EPT construction, INVEPT runs before launch, covering physical root
reuse. Cleanup performs VMCLEAR, clears the EPT leaves, and executes INVEPT
while VMX is still active; then the existing outer VMXOFF and host CR/MSR
restore run. Guest/EPT/VMCS pages are freed only after hardware ownership is
released and cleanup succeeded. Genuine unrecoverable invalidation/restore
failure is reported and affected buffers are retained instead of being freed
while potentially referenced. Successful and all injected software failure
paths verify complete buddy recovery and kernel_alive=1.

Selftests retain the previous VMCS-abort and invalid-guest-state cases and add:
- Partial allocation abort after two EPT pages: no leak or real exit.
- Injected missing INVEPT capabilities: explicit skip, no allocation/leak.
- Successful VMCALL: reason=18, RIP=0x1000, real counter increments once.
- Unmapped GPA read: reason=48, GPA=0x8000, qualification read bit set.
- Code execute permission denied: reason=48, GPA=0x1000, execute bit set.
- Each attempted launch: VMCLEAR/INVEPT/VMXOFF, host restoration, free-page
  comparison, fresh buddy allocation, kernel_alive.

Modes on the trusted kernel test API: 0 success, 1 post-VMPTRLD abort,
2 invalid RFLAGS, 3 unmapped GPA read, 4 code execute denial,
5 partial allocation abort, 6 injected missing INVEPT capability.
This is a bounded test runtime, not an arbitrary guest or device API.

Expected logs:
```
ept init: 4-level 4KiB leaves mapped=7 code=RX data=RW/NX guest_cr3=0x3000
phase9: vmlaunch ok exit_reason=18 ... guest_rip=0x1000 vmexit=real count=1 rollback=ok
ept: violation reason=48 gpa=0x8000 ... test=unmapped read vmexit=real rollback=ok
ept: violation reason=48 gpa=0x1000 ... test=execute permission vmexit=real rollback=ok
ept: teardown INVEPT type=1 ok
ept: real exits=3 (reason=18/48); per-VM synthetic vmexits remain separate
phase9 selftest: PASS (0 failures)
selftest result: PASS (0 failures)
```

Hosted performs explicit skips and passes without claiming hardware execution.
Use make test/kernel/iso, standard single-node KVM and the memory-backend-ram +
memdev two-node command in docs/NUMA.md. The no-VMX KVM regression uses
`-cpu host,-vmx,-svm`; TCG's existing phase3/phase7 limitations remain documented
in PHASE9.md. There is no SVM-equivalent guest launch, EPT fault repair,
VMRESUME loop, guest device model, hotplug or Linux guest support.

Original implementation follows [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html):
EPTP/leaf encodings, secondary controls, IA32_VMX_EPT_VPID_CAP and INVEPT.
