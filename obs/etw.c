#include "capmix/obs.h"
#include "capmix/klog.h"

#include <string.h>

static cm_obs_provider_t g_prov[CM_OBS_PROVIDERS];
static cm_obs_session_t  g_sess[CM_OBS_SESSIONS];
static int g_np, g_ns;

void cm_obs_etw_init(void)
{
    memset(g_prov, 0, sizeof(g_prov));
    memset(g_sess, 0, sizeof(g_sess));
    g_np = g_ns = 0;
    cm_obs_register_provider("capmix.boot", 1u << CM_EV_BOOT);
    cm_obs_register_provider("capmix.ipc",
        (1u << CM_EV_IPC_SEND) | (1u << CM_EV_IPC_RECV) | (1u << CM_EV_IPC_HANDOFF));
    cm_obs_register_provider("capmix.sched",
        (1u << CM_EV_SCHED_PICK) | (1u << CM_EV_SCHED_SWITCH));
    cm_obs_register_provider("capmix.mm",
        (1u << CM_EV_ALLOC) | (1u << CM_EV_FREE) | (1u << CM_EV_PRESSURE));
}

int cm_obs_register_provider(const char *name, uint32_t mask)
{
    if (g_np >= CM_OBS_PROVIDERS) return -1;
    int id = g_np++;
    g_prov[id].id = (uint32_t)id;
    g_prov[id].name = name;
    g_prov[id].event_mask = mask;
    g_prov[id].enabled = 1;
    return id;
}

int cm_obs_open_session(uint32_t provider_mask, cm_id_t owner)
{
    if (g_ns >= CM_OBS_SESSIONS) return -1;
    int id = g_ns++;
    g_sess[id].id = (uint32_t)id;
    g_sess[id].provider_mask = provider_mask;
    g_sess[id].cap_owner = owner;
    g_sess[id].seen = 0;
    g_sess[id].live = 1;
    kinfo("obs: session %d owner=%llu providers=%x (cap-gated)",
          id, (unsigned long)owner, provider_mask);
    return id;
}

cm_error_t cm_obs_authorize(cm_id_t owner, int session)
{
    if (session < 0 || session >= g_ns || !g_sess[session].live)
        return CM_ERR_INVAL;
    if (g_sess[session].cap_owner != owner) return CM_ERR_DENIED;
    g_sess[session].seen++;
    return CM_OK;
}

uint64_t cm_obs_session_seen(int session)
{
    if (session < 0 || session >= g_ns) return 0;
    return g_sess[session].seen;
}

void cm_hist_init(cm_hist_t *h, uint64_t scale)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
    uint64_t b = scale ? scale : 1;
    for (int i = 0; i < CM_HIST_BUCKETS; i++) {
        h->bounds[i] = b;
        b *= 2;
    }
}

void cm_hist_add(cm_hist_t *h, uint64_t v)
{
    if (!h) return;
    h->total++;
    h->sum += v;
    for (int i = 0; i < CM_HIST_BUCKETS; i++) {
        if (v <= h->bounds[i]) { h->count[i]++; return; }
    }
    h->count[CM_HIST_BUCKETS - 1]++;
}

void cm_hist_dump(const char *name, const cm_hist_t *h)
{
    if (!h) return;
    kinfo("hist %s n=%llu sum=%llu", name ? name : "?",
          (unsigned long)h->total, (unsigned long)h->sum);
    for (int i = 0; i < CM_HIST_BUCKETS; i++) {
        if (h->count[i])
            kinfo("  <=%llu : %llu", (unsigned long)h->bounds[i],
                  (unsigned long)h->count[i]);
    }
}
