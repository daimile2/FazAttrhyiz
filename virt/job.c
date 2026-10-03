#include "capmix/virt.h"
#include "capmix/proc.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

static cm_job_t g_job[CM_MAX_JOBS];
static cm_id_t  g_jid = 1;

cm_job_t *cm_job_lookup(cm_id_t id)
{
    for (int i = 0; i < CM_MAX_JOBS; i++)
        if (g_job[i].used && g_job[i].id == id) return &g_job[i];
    return NULL;
}

cm_job_t *cm_job_create(const char *name, uint32_t ns)
{
    for (int i = 0; i < CM_MAX_JOBS; i++) {
        if (g_job[i].used) continue;
        memset(&g_job[i], 0, sizeof(g_job[i]));
        g_job[i].used = 1;
        g_job[i].id = g_jid++;
        g_job[i].name = name ? name : "job";
        g_job[i].ns = ns ? ns : CM_NS_ALL;
        g_job[i].cg.cpu_weight = 100;
        g_job[i].self_obj = cm_obj_mint(CM_OBJ_JOB, &g_job[i], CM_RIGHT_ALL);
        kinfo("job: create '%s' id=%llu ns=%x (single container object)",
              g_job[i].name, (unsigned long)g_job[i].id, g_job[i].ns);
        return &g_job[i];
    }
    return NULL;
}

cm_error_t cm_job_insert(cm_job_t *j, cm_id_t pid)
{
    if (!j || j->killed) return CM_ERR_INVAL;
    if (j->nmembers >= CM_JOB_MEMBERS) return CM_ERR_NOSPC;
    j->members[j->nmembers++] = pid;
    return CM_OK;
}

cm_error_t cm_job_kill(cm_job_t *j)
{
    if (!j) return CM_ERR_INVAL;
    j->killed = 1;
    for (int i = 0; i < j->nmembers; i++) {
        cm_proc_t *p = cm_proc_lookup(j->members[i]);
        if (!p || p->exited) continue;
        p->exited = 1;
        p->exit_code = 15;
        if (p->main) p->main->state = CM_TH_DEAD;
        if (p->aspace_pages) {
            cm_cg_uncharge(j, p->aspace_pages);
            cm_commit_uncharge(p->aspace_pages);
            p->aspace_pages = 0;
        }
    }
    kinfo("job: killed '%s' members=%d (other jobs untouched)",
          j->name, j->nmembers);
    cm_obs_emit(cm_trace_new(), CM_EV_OOM, j->id, (uint64_t)j->nmembers, 0);
    return CM_OK;
}

cm_error_t cm_cg_charge(cm_job_t *j, uint64_t pages)
{
    if (!j) return CM_OK;
    if (j->cg.memory_max && j->cg.memory_cur + pages > j->cg.memory_max) {
        kwarn("cgroup: memory.max hit job=%s cur=%llu +%llu max=%llu",
              j->name, (unsigned long)j->cg.memory_cur,
              (unsigned long)pages, (unsigned long)j->cg.memory_max);
        return CM_ERR_OVERCOMMIT;
    }
    j->cg.memory_cur += pages;
    return CM_OK;
}

void cm_cg_uncharge(cm_job_t *j, uint64_t pages)
{
    if (!j) return;
    if (pages > j->cg.memory_cur) pages = j->cg.memory_cur;
    j->cg.memory_cur -= pages;
}

void cm_job_init(void)
{
    memset(g_job, 0, sizeof(g_job));
    g_jid = 1;
}

void cm_job_dump(void)
{
    for (int i = 0; i < CM_MAX_JOBS; i++) {
        if (!g_job[i].used) continue;
        kinfo("  job %llu '%s' killed=%d mem=%llu/%llu cpu_w=%u n=%d",
              (unsigned long)g_job[i].id, g_job[i].name, g_job[i].killed,
              (unsigned long)g_job[i].cg.memory_cur,
              (unsigned long)g_job[i].cg.memory_max,
              g_job[i].cg.cpu_weight, g_job[i].nmembers);
    }
}
