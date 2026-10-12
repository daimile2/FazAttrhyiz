#include "capmix/mm.h"
#include "capmix/numa.h"
#include "capmix/config.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

#ifdef CAPMIX_HOSTED
#include <stdlib.h>
#endif

typedef struct cm_free_block {
    struct cm_free_block *next;
} cm_free_block_t;

typedef struct {
    uint8_t *base;
    uint64_t pages, free_pages;
    unsigned node;
    cm_free_block_t *free[CM_BUDDY_MAX_ORDER+1];
    uint64_t count[CM_BUDDY_MAX_ORDER+1];
} cm_arena_t;
static cm_arena_t arenas[CM_NUMA_MAX_RANGES];
static unsigned arena_count, next_node;
static uint64_t g_total_pages, g_free_pages;
static uint8_t *g_heap, *g_page_state;
static uint64_t g_heap_pages;

#ifdef CAPMIX_HOSTED
static uint8_t *g_sim_heap;
#endif

/* Plain intrusive lists require ONE lock across pop/split/push/coalesce,
 * arena publication, RR cursor, counters and state. Save IRQ state before
 * acquiring, preventing same-CPU maskable-IRQ reentry; no NMI/reentrant use.
 * Boot initialization is quiescent. Observability and foreign subsystem locks
 * stay outside this lock. The historical unlocked lists could double-issue
 * stacks; existing list serialization is retained, read-side gaps are closed. */
static unsigned int g_buddy_lock;

static uint64_t buddy_lock(void)
{
#ifndef CAPMIX_HOSTED
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#else
    uint64_t flags = 0;
#endif
    if (__atomic_exchange_n(&g_buddy_lock, 1u, __ATOMIC_ACQUIRE) == 0u) return flags;
    for (;;) {
        __asm__ volatile("pause" ::: "memory");
        if (__atomic_exchange_n(&g_buddy_lock, 1u, __ATOMIC_ACQUIRE) == 0u) return flags;
    }
}

static void buddy_unlock(uint64_t flags)
{
    __atomic_store_n(&g_buddy_lock, 0u, __ATOMIC_RELEASE);
#ifndef CAPMIX_HOSTED
    if (flags & 0x200) __asm__ volatile("sti");
#else
    (void)flags;
#endif
}

static const char *state_names[] = {
    "Active", "Transition", "Standby", "Modified",
    "ModNoWrite", "Free", "Zeroed", "Bad"
};

const char *cm_page_state_name(cm_page_state_t s)
{
    if ((unsigned)s >= CM_PAGE_NSTATES) return "?";
    return state_names[s];
}

static void push_free(cm_arena_t *a, unsigned order, void *ptr)
{
    cm_free_block_t *b = (cm_free_block_t *)ptr;
    b->next = a->free[order];
    a->free[order] = b;
    a->count[order]++;
}

static void *pop_free(cm_arena_t *a, unsigned order)
{
    cm_free_block_t *b = a->free[order];
    if (!b) return NULL;
    a->free[order] = b->next;
    a->count[order]--;
    return b;
}

static void mark_state(void *ptr, unsigned order, cm_page_state_t st)
{
    if (!g_page_state || !g_heap) return;
    uintptr_t address=(uintptr_t)ptr, base=(uintptr_t)g_heap;
    if (address<base || address-base>=g_heap_pages*CM_PAGE_SIZE) return;
    uint64_t off = (address-base) / CM_PAGE_SIZE;
    uint64_t n = 1ull << order;
    for (uint64_t i = 0; i < n && off + i < g_heap_pages; i++)
        g_page_state[off + i] = (uint8_t)st;
}

void cm_mm_init(const cm_mem_region_t *regions, int nregions)
{
    memset(arenas, 0, sizeof(arenas));
    arena_count=0; next_node=0;
    g_total_pages = 0;
    g_free_pages = 0;

#ifdef CAPMIX_HOSTED
    /* Simulate a 16 MiB usable arena so buddy/slab/commit are real. */
    g_heap_pages = 16 * 1024 * 1024 / CM_PAGE_SIZE;
    g_sim_heap = (uint8_t *)aligned_alloc(CM_PAGE_SIZE, g_heap_pages * CM_PAGE_SIZE);
    if (!g_sim_heap) cm_panic("hosted heap alloc failed");
    memset(g_sim_heap, 0, g_heap_pages * CM_PAGE_SIZE);
    g_heap = g_sim_heap;
    g_page_state = (uint8_t *)calloc(g_heap_pages, 1);
    CM_UNUSED(regions);
    CM_UNUSED(nregions);
#else
    /* Freestanding: caller must have identity-mapped a contiguous usable span
     * into regions[0]. Later phases walk the full Limine mmap. */
    if (nregions < 1) cm_panic("no memory regions");
    g_heap = (uint8_t *)(uintptr_t)regions[0].base;
    g_heap_pages = regions[0].npages;
    g_page_state = NULL;
#endif

    if (cm_buddy_add_region(0,(uint64_t)(uintptr_t)g_heap,g_heap_pages)!=CM_OK)
        cm_panic("invalid primary buddy arena");

    extern void cm_slab_init(void);
    extern void cm_commit_init(uint64_t total_pages);
    cm_slab_init();
    cm_commit_init(g_total_pages);
    cm_pressure_init();
    cm_pcache_init();
    cm_anon_init();
    kinfo("mm: buddy ready, %llu pages (%llu KiB), max order %d",
          (unsigned long)g_total_pages,
          (unsigned long)(g_total_pages * CM_PAGE_SIZE / 1024),
          CM_BUDDY_MAX_ORDER);
}

void cm_buddy_primary(uint64_t *base, uint64_t *pages)
{
    *base=(uint64_t)(uintptr_t)g_heap; *pages=g_heap_pages;
}
static int arena_of(const void *ptr)
{
    uintptr_t p=(uintptr_t)ptr;
    for (unsigned i=0;i<arena_count;i++) {
        uintptr_t b=(uintptr_t)arenas[i].base;
        if (p>=b && p-b<arenas[i].pages*CM_PAGE_SIZE) return (int)i;
    }
    return -1;
}
int cm_page_node(const void *ptr)
{
    uint64_t flags=buddy_lock(); int i=arena_of(ptr);
    int node=i<0 ? -1 : (int)arenas[i].node; buddy_unlock(flags); return node;
}
cm_error_t cm_buddy_add_region(unsigned node,uint64_t virt,uint64_t pages)
{
    if (node>=CM_NUMA_MAX_NODES || !pages || (virt&CM_PAGE_MASK) ||
        pages>(UINT64_MAX-virt)/CM_PAGE_SIZE) return CM_ERR_INVAL;
    uint64_t flags=buddy_lock();
    if (arena_count==CM_NUMA_MAX_RANGES) { buddy_unlock(flags); return CM_ERR_NOSPC; }
    for (unsigned i=0;i<arena_count;i++) {
        uint64_t b=(uint64_t)(uintptr_t)arenas[i].base, end=b+arenas[i].pages*CM_PAGE_SIZE;
        if (virt<end && b<virt+pages*CM_PAGE_SIZE) { buddy_unlock(flags); return CM_ERR_EXIST; }
    }
    cm_arena_t *a=&arenas[arena_count++];
    a->base=(uint8_t *)(uintptr_t)virt; a->pages=pages; a->node=node; a->free_pages=pages;
    uint64_t remaining=pages; uint8_t *p=a->base;
    while (remaining) {
        unsigned o=CM_BUDDY_MAX_ORDER;
        while (o && ((1ull<<o)>remaining || (((uintptr_t)p>>CM_PAGE_SHIFT)&((1ull<<o)-1)))) o--;
        push_free(a,o,p); mark_state(p,o,CM_PAGE_FREE);
        p+=(1ull<<o)*CM_PAGE_SIZE; remaining-=1ull<<o;
    }
    g_total_pages+=pages; g_free_pages+=pages;
    buddy_unlock(flags); return CM_OK;
}
static void *alloc_node_locked(unsigned node,unsigned order)
{
    for (unsigned i=0;i<arena_count;i++) {
        cm_arena_t *a=&arenas[i]; if (a->node!=node) continue;
        unsigned o=order; while (o<=CM_BUDDY_MAX_ORDER && !a->free[o]) o++;
        if (o>CM_BUDDY_MAX_ORDER) continue;
        void *blk=pop_free(a,o);
        while (o>order) { o--; void *p=(uint8_t *)blk+(1ull<<o)*CM_PAGE_SIZE;
            push_free(a,o,p); mark_state(p,o,CM_PAGE_FREE); }
        a->free_pages-=1ull<<order; g_free_pages-=1ull<<order;
        mark_state(blk,order,CM_PAGE_ACTIVE); return blk;
    }
    return NULL;
}
static void *allocate(unsigned node,unsigned order,bool automatic)
{
    if (order>CM_BUDDY_MAX_ORDER || node>=cm_numa_node_count()) return NULL;
    uint64_t flags=buddy_lock(); void *ptr=NULL;
    unsigned count=cm_numa_node_count();
    if (automatic) {
        for (unsigned n=0;n<count;n++) {
            unsigned selected=(next_node+n)%count;
            ptr=alloc_node_locked(selected,order);
            if (ptr) { next_node=(selected+1)%count; break; }
        }
    } else ptr=alloc_node_locked(node,order);
    buddy_unlock(flags);
    if (ptr) cm_obs_emit(cm_trace_new(),CM_EV_ALLOC,(uint64_t)(uintptr_t)ptr,order,1ull<<order);
    return ptr;
}
void *cm_page_alloc(unsigned order) { return allocate(0,order,true); }
void *cm_page_alloc_node(unsigned node,unsigned order) { return allocate(node,order,false); }
void cm_page_free(void *ptr,unsigned order)
{
    if (!ptr || order>CM_BUDDY_MAX_ORDER) return;
    uint64_t flags=buddy_lock(); int index=arena_of(ptr);
    if (index<0) { buddy_unlock(flags); return; }
    cm_arena_t *a=&arenas[index];
    uintptr_t begin=(uintptr_t)a->base, cur=(uintptr_t)ptr;
    uint64_t n=1ull<<order;
    if ((cur&((n*CM_PAGE_SIZE)-1)) || (cur-begin)/CM_PAGE_SIZE+n>a->pages) {
        buddy_unlock(flags); return;
    }
    a->free_pages+=n; g_free_pages+=n; mark_state(ptr,order,CM_PAGE_FREE);
    unsigned o=order;
    /* Absolute alignment matches initialization; never merge across arenas. */
    while (o<CM_BUDDY_MAX_ORDER) {
        uintptr_t buddy=cur^((1ull<<o)*CM_PAGE_SIZE);
        uintptr_t low=buddy<cur ? buddy : cur;
        if (low<begin || low+(2ull<<o)*CM_PAGE_SIZE>begin+a->pages*CM_PAGE_SIZE) break;
        cm_free_block_t **pp=&a->free[o];
        while (*pp && (uintptr_t)*pp!=buddy) pp=&(*pp)->next;
        if (!*pp) break;
        *pp=(*pp)->next; a->count[o]--; cur=low; o++;
    }
    push_free(a,o,(void *)cur); buddy_unlock(flags);
    cm_obs_emit(cm_trace_new(),CM_EV_FREE,(uint64_t)(uintptr_t)ptr,order,n);
}
cm_error_t cm_numa_stats(unsigned node,cm_numa_stats_t *out)
{
    if (node>=cm_numa_node_count() || !out) return CM_ERR_INVAL;
    uint64_t flags=buddy_lock(); memset(out,0,sizeof(*out));
    for (unsigned i=0;i<arena_count;i++) {
        cm_arena_t *a=&arenas[i]; if (a->node!=node) continue;
        out->total_pages+=a->pages; out->free_pages+=a->free_pages;
        for (unsigned o=0;o<=CM_BUDDY_MAX_ORDER;o++) out->free_blocks[o]+=a->count[o];
    }
    buddy_unlock(flags); return CM_OK;
}

void cm_mm_stats(cm_mm_stats_t *out)
{
    extern void cm_commit_stats(uint64_t *committed, uint64_t *limit);
    extern uint64_t cm_slab_pages(void);
    extern cm_pressure_level_t cm_pressure_current(void);
    memset(out, 0, sizeof(*out));
    uint64_t flags=buddy_lock();
    out->total_pages = g_total_pages;
    out->free_pages  = g_free_pages;
    buddy_unlock(flags);
    out->slab_pages  = cm_slab_pages();
    cm_commit_stats(&out->committed_pages, &out->commit_limit);
    out->pressure         = cm_pressure_current();
    out->compressed_pages = cm_compressed_pages();
    out->pool_pages       = cm_pool_pages();
    out->file_cache_pages = cm_file_cache_pages();
    out->anon_pages       = cm_anon_pages();
    out->swapped_pages    = cm_swapped_pages();
    out->swap_slots       = cm_swap_slots();
    out->swap_total       = cm_swap_total();
    out->jetsam_kills     = cm_jetsam_kills();
    out->oom_kills        = cm_oom_kills();
}

uint64_t cm_mm_free_pages(void)
{ uint64_t flags=buddy_lock(),n=g_free_pages;buddy_unlock(flags);return n; }
uint64_t cm_mm_total_pages(void)
{ uint64_t flags=buddy_lock(),n=g_total_pages;buddy_unlock(flags);return n; }

cm_error_t cm_buddy_arena_snapshot(unsigned index,cm_buddy_arena_info_t *out)
{
    if(!out)return CM_ERR_INVAL;
    uint64_t flags=buddy_lock();
    if(index>=arena_count){buddy_unlock(flags);return CM_ERR_NOENT;}
    cm_arena_t *a=&arenas[index];
    *out=(cm_buddy_arena_info_t){(uint64_t)(uintptr_t)a->base,a->pages,a->free_pages,a->node};
    buddy_unlock(flags);return CM_OK;
}

void cm_mm_dump(void)
{
    cm_mm_stats_t s;
    cm_mm_stats(&s);
    kinfo("mm: total=%llu free=%llu commit=%llu/%llu cache=%llu anon=%llu "
          "comp=%llu(pool %llu) swapped=%llu(segs %llu/%llu) jetsam=%u oom=%u press=%s",
          (unsigned long)s.total_pages, (unsigned long)s.free_pages,
          (unsigned long)s.committed_pages, (unsigned long)s.commit_limit,
          (unsigned long)s.file_cache_pages, (unsigned long)s.anon_pages,
          (unsigned long)s.compressed_pages, (unsigned long)s.pool_pages,
          (unsigned long)s.swapped_pages, (unsigned long)s.swap_slots,
          (unsigned long)s.swap_total, s.jetsam_kills, s.oom_kills,
          cm_pressure_name(s.pressure));
    cm_buddy_arena_info_t a;
    for(unsigned i=0;cm_buddy_arena_snapshot(i,&a)==CM_OK;i++)
        kinfo("  buddy node=%u arena=%u pages=%llu free=%llu",a.node,i,
              (unsigned long)a.pages,(unsigned long)a.free_pages);
}
