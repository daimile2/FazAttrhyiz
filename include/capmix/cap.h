#ifndef CAPMIX_CAP_H
#define CAPMIX_CAP_H

#include "types.h"
#include "config.h"

/* Layer 0 + 6 foundation: first-class capabilities (Zircon / seL4 / Mach).
 * Security grows from the object graph; LSM hooks are optional, not the base.
 */

typedef enum {
    CM_OBJ_INVALID = 0,
    CM_OBJ_TASK,
    CM_OBJ_THREAD,
    CM_OBJ_CHANNEL,
    CM_OBJ_ENDPOINT,
    CM_OBJ_VMAR,          /* virtual memory address region */
    CM_OBJ_VMO,           /* virtual memory object (Mach memory object) */
    CM_OBJ_IRQ,
    CM_OBJ_TRACE,         /* observation is a capability */
    CM_OBJ_JOB,           /* Fuchsia-style container / coalition */
    CM_OBJ_PORT,
    CM_OBJ_EVENT,
    CM_OBJ_FILE,          /* open VFS file; fdio maps fd -> this handle */
    CM_OBJ_CONSOLE,
    CM_OBJ_CONTROL,
    CM_OBJ_BOOT,
    CM_OBJ_CHILD,
    CM_OBJ_LINUX_SOCKET,  /* Linux fd wrapper; native object ids stay fixed */
    CM_OBJ_LINUX_PIPE,    /* bounded Linux-only pipe endpoint */
    CM_OBJ_NTYPES
} cm_obj_type_t;

#define CM_RIGHT_READ     (1u << 0)
#define CM_RIGHT_WRITE    (1u << 1)
#define CM_RIGHT_EXEC     (1u << 2)
#define CM_RIGHT_TRANSFER (1u << 3)
#define CM_RIGHT_DUPLICATE (1u << 4)
#define CM_RIGHT_DESTROY  (1u << 5)
#define CM_RIGHT_SIGNAL   (1u << 6)
#define CM_RIGHT_WAIT     (1u << 7)
#define CM_RIGHT_MAP      (1u << 8)
#define CM_RIGHT_INSPECT  (1u << 9)
#define CM_RIGHT_ALL      0x3FFu

typedef struct cm_object cm_object_t;
typedef struct cm_task    cm_task_t;

struct cm_object {
    cm_id_t       id;
    cm_obj_type_t type;
    uint32_t      refs;
    cm_rights_t   default_rights;
    void         *payload;
    void        (*destroy)(void *payload); /* NULL for legacy/bootstrap objects */
};

typedef struct {
    cm_object_t *obj;
    cm_rights_t  rights;
} cm_handle_t;

void        cm_cap_init(void);
cm_object_t *cm_obj_mint(cm_obj_type_t type, void *payload, cm_rights_t rights);
cm_object_t *cm_obj_mint_owned(cm_obj_type_t type, void *payload, cm_rights_t rights,
                               void (*destroy)(void *));
void         cm_obj_retain(cm_object_t *o);
void         cm_obj_release(cm_object_t *o);

int          cm_handle_install(cm_task_t *task, cm_object_t *obj, cm_rights_t rights);
cm_handle_t *cm_handle_lookup(cm_task_t *task, int hid);
/* Acquire a stable ref with an exact type and required rights. Caller releases it. */
cm_error_t cm_handle_acquire(cm_task_t *task, int hid, cm_obj_type_t type,
                             cm_rights_t required, cm_object_t **out);
cm_error_t   cm_handle_drop(cm_task_t *task, int hid);
cm_error_t   cm_handle_duplicate(cm_task_t *src, int hid, cm_task_t *dst, int *out_hid,
                                cm_rights_t mask);

const char  *cm_obj_type_name(cm_obj_type_t t);
void         cm_cap_dump(void);

#endif
