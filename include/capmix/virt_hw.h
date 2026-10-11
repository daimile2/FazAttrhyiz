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
#endif
