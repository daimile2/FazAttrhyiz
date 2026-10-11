#include "capmix/cap.h"
#include "capmix/cap_internal.h"
#include "capmix/sched.h"
#include "capmix/klog.h"

#include <string.h>

int cm_handle_install_locked(cm_task_t *task, cm_object_t *obj, cm_rights_t rights)
{
    if (!task || !obj || !obj->refs || obj->type == CM_OBJ_INVALID) return CM_ERR_INVAL;
    for (int i = 1; i < CM_HANDLES_PER_TASK; i++) {
        if (task->handles[i].obj == NULL) {
            task->handles[i].obj = obj;
            task->handles[i].rights = rights & obj->default_rights;
            cm_obj_retain_locked(obj);
            if (i + 1 > task->handle_cap) task->handle_cap = i + 1;
            return i;
        }
    }
    return CM_ERR_NOSPC;
}

int cm_handle_install(cm_task_t *task, cm_object_t *obj, cm_rights_t rights)
{
    uint64_t flags = cm_cap_lock();
    int result = cm_handle_install_locked(task, obj, rights);
    cm_cap_unlock(flags);
    return result;
}

cm_handle_t *cm_handle_lookup(cm_task_t *task, int hid)
{
    if (!task || hid <= 0 || hid >= CM_HANDLES_PER_TASK) return NULL;
    if (!task->handles[hid].obj) return NULL;
    return &task->handles[hid];
}

cm_error_t cm_handle_acquire(cm_task_t *task, int hid, cm_obj_type_t type,
                             cm_rights_t required, cm_object_t **out)
{
    if (!out) return CM_ERR_INVAL;
    uint64_t flags = cm_cap_lock();
    cm_handle_t *h = cm_handle_lookup(task, hid);
    cm_error_t result = CM_OK;
    if (!h || h->obj->type != type) result = CM_ERR_INVAL;
    else if ((h->rights & required) != required) result = CM_ERR_DENIED;
    else { cm_obj_retain_locked(h->obj); *out = h->obj; }
    cm_cap_unlock(flags);
    return result;
}

cm_error_t cm_handle_drop(cm_task_t *task, int hid)
{
    uint64_t flags = cm_cap_lock();
    cm_handle_t *h = cm_handle_lookup(task, hid);
    if (!h) { cm_cap_unlock(flags); return CM_ERR_INVAL; }
    cm_object_t *obj = h->obj;
    memset(h, 0, sizeof(*h));
    cm_cap_unlock(flags);
    cm_obj_release(obj);
    return CM_OK;
}

cm_error_t cm_handle_duplicate(cm_task_t *src, int hid, cm_task_t *dst, int *out_hid,
                              cm_rights_t mask)
{
    if (!dst || !out_hid) return CM_ERR_INVAL;
    uint64_t flags = cm_cap_lock();
    cm_handle_t *h = cm_handle_lookup(src, hid);
    int n;
    if (!h) n = CM_ERR_INVAL;
    else if (!(h->rights & CM_RIGHT_DUPLICATE)) n = CM_ERR_DENIED;
    else n = cm_handle_install_locked(dst, h->obj, h->rights & mask);
    if (n > 0) *out_hid = n;
    cm_cap_unlock(flags);
    return n > 0 ? CM_OK : n;
}
