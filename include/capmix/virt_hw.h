#ifndef CAPMIX_VIRT_HW_H
#define CAPMIX_VIRT_HW_H
#include "types.h"
typedef enum {
    CM_VIRT_HW_OK, CM_VIRT_HW_UNSUPPORTED, CM_VIRT_HW_HOSTED,
    CM_VIRT_HW_POLICY, CM_VIRT_HW_INVALID, CM_VIRT_HW_NOMEM,
    CM_VIRT_HW_FAULT, CM_VIRT_HW_INSTRUCTION, CM_VIRT_HW_ABORTED,
    CM_VIRT_HW_ROLLBACK
} cm_virt_hw_status_t;
typedef struct {
    bool vmx, svm, initialized, rolled_back, recovery_tested;
    unsigned fault_vector, instruction_status;
    const char *stage;
    cm_virt_hw_status_t status;
} cm_virt_hw_result_t;
/* BSP synchronous probe only; no guest launch. abort_prepare tests cleanup. */
cm_virt_hw_result_t cm_virt_hw_probe(bool abort_prepare);
typedef struct {
    cm_virt_hw_result_t hardware;
    bool vmcs_ready, entered, real_exit, vmcs_cleared;
    int instruction_status;
    uint64_t instruction_error, failed_field, exit_reason, qualification, guest_rip;
    bool ept_ready,ept_invalidated;
    uint64_t eptp,guest_gpa,guest_cr3,guest_entry;
    unsigned invept_type;
    void *ept_tables[4],*guest_pages[7];
    const char *stage;
    void *vmcs_region,*guest_stack; /* retained only if rollback cannot release hardware ownership */
} cm_guest_result_t;
/* Modes: 0 VMCALL, 1 abort after VMPTRLD, 2 invalid guest state,
 * 3 unmapped GPA read, 4 execute permission violation,
 * 5 partial allocation abort, 6 injected unavailable INVEPT capability. */
cm_guest_result_t cm_virt_guest_once(unsigned mode);
#ifndef CAPMIX_HOSTED
bool cm_vmcs_guest_once(uint64_t basic,unsigned mode,cm_guest_result_t *result);
bool cm_ept_prepare(cm_guest_result_t *result,unsigned mode);
int cm_ept_invalidate(cm_guest_result_t *result);
int cm_hw_invept(uint64_t type,const void *descriptor);
int cm_hw_vmclear(const uint64_t *phys);
int cm_hw_vmptrld(const uint64_t *phys);
int cm_hw_vmwrite(uint64_t field,uint64_t value);
int cm_hw_vmread(uint64_t field,uint64_t *out);
int cm_hw_vmlaunch_once(void);
extern char cm_guest_vmcall[],cm_vmexit_return[];
extern const uint64_t cm_vmcs_fixups_begin[],cm_vmcs_fixups_end[];
#endif
#endif
