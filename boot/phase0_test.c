#include "capmix/kernel.h"
#include "capmix/arch.h"
#include "capmix/numa.h"
#include "capmix/config.h"
#include "capmix/klog.h"

int cm_phase0_selftest(void)
{
    int fails = 0;
    uint64_t root_pa = 0, shadow_pa = 0;
    uint64_t *root = cm_space_create(&root_pa);
    void *page = cm_page_alloc_node(0, 0);
    if (!root || !page) return -1;
    uint64_t h = cm_hhdm();
    uint64_t pa = (uint64_t)(uintptr_t)page - h;
    uint64_t before = cm_paging_tables();
    const uint64_t a = 0x2000000000ull, b = 0x4000000000ull;
    if (cm_space_map(root, a, pa, 1) ||
        cm_paging_tables() - before != cm_paging_levels() - 1 ||
        cm_space_lookup(root, a + CM_PAGE_SIZE) ||
        (cm_space_lookup(root, a) & 0x000ffffffffff007ull) != (pa | 7)) fails++;
    if (cm_space_map(root, b, pa, 1) || !(cm_space_lookup(root, b) & 4)) fails++;
    for (unsigned level = 1; level <= CM_PT_MAX_LEVELS; level++)
        if (cm_paging_level_folded(level) != (level > cm_paging_levels())) fails++;
    if (cm_paging_levels() == 5) {
        /* Different PML5 index, same lower-level indices: catches fake folding. */
        uint64_t upper = a | (1ull << 48);
        if (cm_space_lookup(root, upper) || cm_space_map(root, upper, pa, 1) ||
            !(cm_space_lookup(root, upper) & 4)) fails++;
    }
    uint64_t *shadow = cm_space_shadow(root, &shadow_pa);
    if (!shadow || !shadow_pa || cm_space_lookup(root, a) != cm_space_lookup(shadow, a)) fails++;
#if CM_KPTI
    if (shadow == root || cm_space_lookup(shadow, (uint64_t)(uintptr_t)cm_phase0_selftest) ||
        cm_space_lookup(shadow, (uint64_t)(uintptr_t)page)) fails++;
#ifndef CAPMIX_HOSTED
    extern char __entry_text_start[], __entry_data_start[];
    if ((cm_space_lookup(shadow, (uint64_t)(uintptr_t)__entry_text_start) & 7) != 1 ||
        (cm_space_lookup(shadow, (uint64_t)(uintptr_t)__entry_data_start) & 7) != 3) fails++;
#endif
#else
    if (shadow != root) fails++;
#endif
    cm_page_free(page, 0);
    cm_numa_stats_t old, allocated, freed;
    cm_numa_node_t node;
    if (cm_numa_node_count() != 1 || cm_numa_node_info(0, &node) ||
        !node.cpu_mask || node.distance != 10 || cm_numa_cpu_node(-1) != -1 ||
        cm_numa_cpu_node(cm_cpu_count()) != -1 ||
        cm_numa_node_info(1, &node) != CM_ERR_INVAL ||
        cm_numa_stats(1, &old) != CM_ERR_INVAL ||
        cm_numa_stats(0, NULL) != CM_ERR_INVAL || cm_page_alloc_node(1, 0)) fails++;
    cm_task_t task = {0};
    if (cm_numa_bind_task(&task, 0) || task.memory_node ||
        cm_numa_bind_task(&task, 1) != CM_ERR_NOTSUP ||
        cm_numa_bind_task(NULL, 0) != CM_ERR_INVAL) fails++;
    cm_numa_stats(0, &old);
    page = cm_page_alloc_node(task.memory_node, 2);
    cm_numa_stats(0, &allocated);
    if (!page || old.free_pages != allocated.free_pages + 4 ||
        old.total_pages != allocated.total_pages) fails++;
    cm_page_free(page, 2);
    cm_numa_stats(0, &freed);
    uint64_t sum = 0;
    for (unsigned order = 0; order <= CM_BUDDY_MAX_ORDER; order++)
        sum += freed.free_blocks[order] << order;
    if (sum != freed.free_pages || freed.free_pages != old.free_pages) fails++;
    kinfo("phase0: %u-level walker folding, KPTI=%d, node0 buddy/bind: %s (%d failures)",
          cm_paging_levels(), CM_KPTI, fails ? "FAIL" : "PASS", fails);
    return fails ? -1 : 0;
}
