#include "capmix/cap.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

static cm_object_t g_objs[CM_MAX_CAPS];
static uint32_t    g_used;
static cm_id_t     g_next_id = 1;

void cm_cap_init(void)
{
    memset(g_objs, 0, sizeof(g_objs));
    g_used = 0;
    g_next_id = 1;
}

static const char *names[] = {
    "invalid", "task", "thread", "channel", "endpoint",
    "vmar", "vmo", "irq", "trace", "job", "port", "event"
};

const char *cm_obj_type_name(cm_obj_type_t t)
{
    if ((unsigned)t >= CM_OBJ_NTYPES) return "?";
    return names[t];
}

cm_object_t *cm_obj_mint(cm_obj_type_t type, void *payload, cm_rights_t rights)
{
    if (g_used >= CM_MAX_CAPS) return NULL;
    cm_object_t *o = NULL;
    for (uint32_t i = 0; i < CM_MAX_CAPS; i++) {
        if (g_objs[i].type == CM_OBJ_INVALID) { o = &g_objs[i]; break; }
    }
    if (!o) return NULL;
    o->id = g_next_id++;
    o->type = type;
    o->refs = 1;
    o->default_rights = rights ? rights : CM_RIGHT_ALL;
    o->payload = payload;
    g_used++;
    cm_obs_emit(cm_trace_new(), CM_EV_CAP_MINT, o->id, (uint64_t)type, o->default_rights);
    return o;
}

void cm_obj_retain(cm_object_t *o)
{
    if (o) o->refs++;
}

void cm_obj_release(cm_object_t *o)
{
    if (!o || o->refs == 0) return;
    if (--o->refs == 0) {
        cm_obs_emit(cm_trace_new(), CM_EV_CAP_DROP, o->id, (uint64_t)o->type, 0);
        o->type = CM_OBJ_INVALID;
        o->payload = NULL;
        g_used--;
    }
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
