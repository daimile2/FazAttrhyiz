#include "capmix/mm.h"
#include "capmix/config.h"
#include "capmix/klog.h"
#include "capmix/obs.h"
#include "capmix/arch.h"

/* Windows no-overcommit: every anonymous mapping charges commit charge
 * up front. When charge would exceed commit_limit, fail the allocation
 * instead of hoping a page appears later (the OOM-killer path is last). */

static uint64_t g_committed;
static uint64_t g_limit;
static unsigned g_lock;
static uint64_t lock(void){uint64_t f=cm_irq_save();while(__atomic_exchange_n(&g_lock,1,__ATOMIC_ACQUIRE))__asm__ volatile("pause");return f;}
static void unlock(uint64_t f){__atomic_store_n(&g_lock,0,__ATOMIC_RELEASE);cm_irq_restore(f);}

void cm_commit_init(uint64_t total_pages)
{
    uint64_t reserve = (total_pages * CM_COMMIT_RESERVE_PCT) / 100;
    if (reserve < 16) reserve = 16;
    g_limit = total_pages > reserve ? total_pages - reserve : total_pages / 2;
    g_committed = 0;
    kinfo("mm: commit limit %llu pages (no-overcommit, reserve %llu)",
          (unsigned long)g_limit, (unsigned long)reserve);
}

cm_error_t cm_commit_charge(uint64_t pages)
{
    uint64_t f=lock();
    if(pages>UINT64_MAX-g_committed){unlock(f);return CM_ERR_OVERCOMMIT;}
#if CM_NO_OVERCOMMIT
    if (g_committed > g_limit || pages > g_limit-g_committed) {
        kwarn("mm: commit denied %llu+%llu > limit %llu",
              (unsigned long)g_committed, (unsigned long)pages,
              (unsigned long)g_limit);
        unlock(f);return CM_ERR_OVERCOMMIT;
    }
#endif
    g_committed += pages;
    unlock(f);return CM_OK;
}

/* Windows: commit limit = RAM + pagefile. Called once swap is known. */
void cm_commit_add_backing(uint64_t pages)
{
    uint64_t f=lock();
    if(pages<=UINT64_MAX-g_limit)g_limit += pages;
    kinfo("mm: commit limit %llu pages (+%llu backed by swap)",
          (unsigned long)g_limit, (unsigned long)pages);
    unlock(f);
}

void cm_commit_uncharge(uint64_t pages)
{
    uint64_t f=lock();
    if (pages > g_committed) pages = g_committed;
    g_committed -= pages;unlock(f);
}

void cm_commit_stats(uint64_t *committed, uint64_t *limit)
{
    uint64_t f=lock();
    if (committed) *committed = g_committed;
    if (limit) *limit = g_limit;
    unlock(f);
}
