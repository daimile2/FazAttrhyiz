#ifndef CAPMIX_NUMA_H
#define CAPMIX_NUMA_H
#include "types.h"
#include "config.h"
#define CM_NUMA_MAX_NODES 8
#define CM_NUMA_MAX_RANGES 64
struct cm_task;
typedef struct { unsigned node; uint64_t cpu_mask; unsigned distance; uint32_t domain; } cm_numa_node_t;
typedef struct { uint64_t total_pages, free_pages; uint64_t free_blocks[CM_BUDDY_MAX_ORDER+1]; } cm_numa_stats_t;
void cm_numa_init(void);
void cm_numa_update_cpus(void);
unsigned cm_numa_node_count(void);
int cm_numa_cpu_node(int cpu);
unsigned cm_numa_distance(unsigned from, unsigned to);
cm_error_t cm_numa_node_info(unsigned node, cm_numa_node_t *out);
cm_error_t cm_numa_stats(unsigned node, cm_numa_stats_t *out);
cm_error_t cm_numa_bind_task(struct cm_task *task, unsigned node);
/* Strict node allocation; ordinary cm_page_alloc balances and falls back. */
void *cm_page_alloc_node(unsigned node, unsigned order);
int cm_page_node(const void *ptr);
cm_error_t cm_buddy_add_region(unsigned node, uint64_t virt, uint64_t pages);
void cm_buddy_primary(uint64_t *base, uint64_t *pages);
#endif
