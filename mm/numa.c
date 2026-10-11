#include "capmix/numa.h"
#include "capmix/arch.h"
#include "capmix/mm.h"
#include "capmix/sched.h"

unsigned cm_numa_node_count(void) { return 1; }
int cm_numa_cpu_node(int cpu)
{
    return cpu >= 0 && cpu < cm_cpu_count() ? 0 : -1;
}
cm_error_t cm_numa_node_info(unsigned node, cm_numa_node_t *out)
{
    if (node || !out) return CM_ERR_INVAL;
    out->node = 0;
    unsigned cpus = (unsigned)cm_cpu_count();
    out->cpu_mask = cpus >= 64 ? UINT64_MAX : ((1ull << cpus) - 1);
    out->distance = 10;
    return CM_OK;
}
cm_error_t cm_numa_bind_task(struct cm_task *task, unsigned node)
{
    if (!task) return CM_ERR_INVAL;
    if (node) return CM_ERR_NOTSUP;
    task->memory_node = node;
    return CM_OK;
}
void *cm_page_alloc_node(unsigned node, unsigned order)
{
    return node == 0 ? cm_page_alloc(order) : NULL;
}
