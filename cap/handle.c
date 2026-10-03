#include "capmix/cap.h"
#include "capmix/sched.h"
#include "capmix/klog.h"

#include <string.h>

int cm_handle_install(cm_task_t *task, cm_object_t *obj, cm_rights_t rights)
{
    if (!task || !obj) return -CM_ERR_INVAL;
    for (int i = 1; i < CM_HANDLES_PER_TASK; i++) {
        if (task->handles[i].obj == NULL) {
            task->handles[i].obj = obj;
            task->handles[i].rights = rights & obj->default_rights;
            cm_obj_retain(obj);
            if (i + 1 > task->handle_cap) task->handle_cap = i + 1;
            return i;
        }
    }
    return -CM_ERR_NOSPC;
}

cm_handle_t *cm_handle_lookup(cm_task_t *task, int hid)
{
    if (!task || hid <= 0 || hid >= CM_HANDLES_PER_TASK) return NULL;
    if (!task->handles[hid].obj) return NULL;
    return &task->handles[hid];
}

cm_error_t cm_handle_drop(cm_task_t *task, int hid)
{
    cm_handle_t *h = cm_handle_lookup(task, hid);
    if (!h) return CM_ERR_INVAL;
    cm_obj_release(h->obj);
    h->obj = NULL;
    h->rights = 0;
    return CM_OK;
}

cm_error_t cm_handle_duplicate(cm_task_t *src, int hid, cm_task_t *dst, int *out_hid,
                              cm_rights_t mask)
{
    cm_handle_t *h = cm_handle_lookup(src, hid);
    if (!h) return CM_ERR_INVAL;
    if ((h->rights & CM_RIGHT_DUPLICATE) == 0) return CM_ERR_DENIED;
    int n = cm_handle_install(dst, h->obj, h->rights & mask);
    if (n < 0) return (cm_error_t)n;
    if (out_hid) *out_hid = n;
    return CM_OK;
}
