#include "capmix/cap.h"
#include "capmix/cap_internal.h"
#include "capmix/arch.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

static cm_object_t g_objs[CM_MAX_CAPS];
static uint32_t    g_used;
static cm_id_t     g_next_id = 1;
static volatile unsigned g_cap_lock;

uint64_t cm_cap_lock(void)
{
    uint64_t flags = cm_irq_save();
    while (__sync_lock_test_and_set(&g_cap_lock, 1u))
        __asm__ volatile("pause" ::: "memory");
    return flags;
}
void cm_cap_unlock(uint64_t flags)
{
    __sync_lock_release(&g_cap_lock);
    cm_irq_restore(flags);
}

void cm_cap_init(void)
{
    memset(g_objs, 0, sizeof(g_objs));
    g_used = 0;
    g_next_id = 1;
}

static const char *names[] = {
    "invalid", "task", "thread", "channel", "endpoint",
    "vmar", "vmo", "irq", "trace", "job", "port", "event", "file", "console", "control", "boot", "child"
};

const char *cm_obj_type_name(cm_obj_type_t t)
{
    if ((unsigned)t >= CM_OBJ_NTYPES) return "?";
    return names[t];
}

cm_object_t *cm_obj_mint_owned(cm_obj_type_t type, void *payload, cm_rights_t rights,
                               void (*destroy)(void *))
{
    if (type <= CM_OBJ_INVALID || type >= CM_OBJ_NTYPES) return NULL;
    uint64_t flags = cm_cap_lock();
    cm_object_t *o = NULL;
    if (g_used < CM_MAX_CAPS) {
        for (uint32_t i = 0; i < CM_MAX_CAPS; i++)
            if (g_objs[i].type == CM_OBJ_INVALID) { o = &g_objs[i]; break; }
    }
    if (o) {
        memset(o, 0, sizeof(*o));
        o->id = g_next_id++;
        o->type = type;
        o->refs = 1;
        o->default_rights = rights ? rights : CM_RIGHT_ALL;
        o->payload = payload;
        o->destroy = destroy;
        g_used++;
    }
    cm_cap_unlock(flags);
    if (o) cm_obs_emit(cm_trace_new(), CM_EV_CAP_MINT, o->id, type, o->default_rights);
    return o;
}

cm_object_t *cm_obj_mint(cm_obj_type_t type, void *payload, cm_rights_t rights)
{
    return cm_obj_mint_owned(type, payload, rights, NULL);
}

void cm_obj_retain_locked(cm_object_t *o)
{
    if (o && o->refs && o->type != CM_OBJ_INVALID) o->refs++;
}
void cm_obj_retain(cm_object_t *o)
{
    uint64_t flags = cm_cap_lock();
    cm_obj_retain_locked(o);
    cm_cap_unlock(flags);
}

void cm_obj_release(cm_object_t *o)
{
    if (!o) return;
    uint64_t flags = cm_cap_lock();
    if (!o->refs) { cm_cap_unlock(flags); return; }
    int last = --o->refs == 0;
    cm_cap_unlock(flags);
    if (!last) return;
    /* Keep the slot occupied until its destructor completes. Destructors may
     * release other capabilities, allocate/free pages, and emit observations. */
    cm_obs_emit(cm_trace_new(), CM_EV_CAP_DROP, o->id, o->type, 0);
    if (o->destroy) o->destroy(o->payload);
    flags = cm_cap_lock();
    memset(o, 0, sizeof(*o));
    g_used--;
    cm_cap_unlock(flags);
}

void cm_cap_dump(void)
{
    kinfo("cap: %u live objects", g_used);
    for (uint32_t i = 0; i < CM_MAX_CAPS; i++) {
        cm_object_t *o = &g_objs[i];
        if (o->type == CM_OBJ_INVALID) continue;
        kinfo("  id=%llu type=%s refs=%u rights=%x payload=%p",
              (unsigned long)o->id, cm_obj_type_name(o->type),
              o->refs, o->default_rights, o->payload);
    }
}
