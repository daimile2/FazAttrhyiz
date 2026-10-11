#include "capmix/numa.h"
#include "capmix/mm.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include <string.h>
int cm_numa_selftest(void)
{
    unsigned count=cm_numa_node_count(),online=0,seen=0;
    int fails=0; uint64_t flags=cm_irq_save();
    cm_numa_stats_t before[CM_NUMA_MAX_NODES],after;
    for (unsigned n=0;n<count;n++) {
        cm_numa_stats(n,&before[n]); if (before[n].free_pages) online++;
        if (cm_numa_distance(n,n)!=10) fails++;
    }
    for (int cpu=0;cpu<cm_cpu_count();cpu++) {
        int n=cm_numa_cpu_node(cpu); if (n<0 || (unsigned)n>=count) fails++;
        else { cm_numa_node_t info; cm_numa_node_info((unsigned)n,&info);
            if (!(info.cpu_mask&(1ull<<cpu))) fails++; }
    }
    void *pages[CM_NUMA_MAX_NODES*2]={0};
    for (unsigned i=0;i<count*2;i++) {
        pages[i]=cm_page_alloc(0); int n=cm_page_node(pages[i]);
        if (n<0 || (unsigned)n>=count) fails++;
        else { seen|=1u<<n; memset(pages[i],(int)i,CM_PAGE_SIZE); }
    }
    for (unsigned i=0;i<count*2;i++) if (pages[i]) {
        if (((uint8_t *)pages[i])[CM_PAGE_SIZE-1]!=(uint8_t)i) fails++;
        cm_page_free(pages[i],0);
    }
    unsigned visited=0; for (unsigned n=0;n<count;n++) if (seen&(1u<<n)) visited++;
    if (online>1 && visited!=online) fails++;
    for (unsigned n=0;n<count;n++) {
        void *p=cm_page_alloc_node(n,0);
        if (before[n].free_pages && (!p || cm_page_node(p)!=(int)n)) fails++;
        if (p) cm_page_free(p,0);
        cm_numa_stats(n,&after);
        uint64_t blocks=0; for (unsigned o=0;o<=CM_BUDDY_MAX_ORDER;o++) blocks+=after.free_blocks[o]<<o;
        if (after.free_pages!=before[n].free_pages || blocks!=after.free_pages) fails++;
    }
    /* Drain all order-0 pages of one small remote arena, then require normal
     * allocation to survive via another node. Stored pointers live on node0. */
    if (online>1) {
        unsigned victim=1; while (victim<count && !before[victim].free_pages) victim++;
        if (victim<count && before[victim].free_pages<=16384) {
            unsigned order=0; while (((1ull<<order)*CM_PAGE_SIZE)<before[victim].free_pages*sizeof(void *)) order++;
            void **held=cm_page_alloc_node(0,order);
            if (!held) fails++;
            else {
                uint64_t n=0; void *p;
                while (n<before[victim].free_pages && (p=cm_page_alloc_node(victim,0))) held[n++]=p;
                if (cm_page_alloc_node(victim,0)) fails++;
                p=cm_page_alloc(0);
                if (!p || cm_page_node(p)==(int)victim) fails++;
                if (p) cm_page_free(p,0);
                while (n) cm_page_free(held[--n],0);
                cm_page_free(held,order);
                cm_numa_stats(victim,&after);
                if (after.free_pages!=before[victim].free_pages) fails++;
                kinfo("numa: remote node=%u exhaustion fallback/recovery verified",victim);
            }
        }
    }
    cm_irq_restore(flags);
    kinfo("numa: balance verified nodes=%u online_memory_nodes=%u visited_mask=%x (round-robin/strict bind)",count,online,seen);
    kinfo("numa selftest: %s (%d failures)",fails ? "FAIL" : "PASS",fails);
    return fails;
}
