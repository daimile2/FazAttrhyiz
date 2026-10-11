#ifndef CAPMIX_CAP_INTERNAL_H
#define CAPMIX_CAP_INTERNAL_H
#include "cap.h"
/* One guard serializes object refs, handle tables and channel transactions.
 * Never call public cap APIs, destructors, or reschedule while holding it. */
uint64_t cm_cap_lock(void);
void cm_cap_unlock(uint64_t flags);
void cm_obj_retain_locked(cm_object_t *obj);
int cm_handle_install_locked(cm_task_t *task, cm_object_t *obj, cm_rights_t rights);
#endif
