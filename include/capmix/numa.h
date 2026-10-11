#ifndef CAPMIX_NUMA_H
#define CAPMIX_NUMA_H
#include "types.h"
#include "config.h"

/* Stage 0 topology: one online node owns the contiguous buddy arena.
 * No SRAT discovery, remote allocation fallback, or page migration yet. */
struct cm_task;
typedef struct {
    unsigned node;
    uint64_t cpu_mask;
    unsigned distance; /* local distance = 10 */
} cm_numa_node_t;
typedef struct {
    uint64_t total_pages, free_pages;
    uint64_t free_blocks[CM_BUDDY_MAX_ORDER + 1];
} cm_numa_stats_t;
unsigned cm_numa_node_count(void);
int cm_numa_cpu_node(int cpu);
cm_error_t cm_numa_node_info(unsigned node, cm_numa_node_t *out);
cm_error_t cm_numa_stats(unsigned node, cm_numa_stats_t *out);
/* Explicit policy metadata; unsupported nodes are rejected, not ignored. */
cm_error_t cm_numa_bind_task(struct cm_task *task, unsigned node);
void *cm_page_alloc_node(unsigned node, unsigned order);
#endif
