#include "capmix/mm.h"
#include "capmix/config.h"
#include "capmix/klog.h"

#include <string.h>

typedef struct cm_slab_obj {
    struct cm_slab_obj *next;
} cm_slab_obj_t;

typedef struct {
    size_t        size;
    cm_slab_obj_t *free;
    uint64_t      allocs;
    uint64_t      pages;
} cm_slab_t;

static const size_t g_sizes[] = {
    16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};
#define NCLASSES ((int)CM_ARRAY_LEN(g_sizes))

static cm_slab_t g_slabs[NCLASSES];
static uint64_t  g_slab_pages;

static int class_for(size_t size)
{
    for (int i = 0; i < NCLASSES; i++)
        if (g_sizes[i] >= size) return i;
    return -1;
}

void cm_slab_init(void)
{
    memset(g_slabs, 0, sizeof(g_slabs));
    g_slab_pages = 0;
    for (int i = 0; i < NCLASSES; i++)
        g_slabs[i].size = g_sizes[i];
}

static int refill(cm_slab_t *s)
{
    void *page = cm_page_alloc(0);
    if (!page) return -1;
    g_slab_pages++;
    s->pages++;
    size_t n = CM_PAGE_SIZE / s->size;
    uint8_t *p = (uint8_t *)page;
    for (size_t i = 0; i < n; i++) {
        cm_slab_obj_t *o = (cm_slab_obj_t *)(p + i * s->size);
        o->next = s->free;
        s->free = o;
    }
    return 0;
}

void *cm_kmalloc(size_t size)
{
    if (size == 0) size = 1;
    int c = class_for(size);
    if (c < 0) {
        /* Large object: whole pages, store size in header page? Keep simple. */
        unsigned order = 0;
        size_t need = CM_ALIGN_UP(size, CM_PAGE_SIZE);
        while ((CM_PAGE_SIZE << order) < need && order < CM_BUDDY_MAX_ORDER)
            order++;
        return cm_page_alloc(order);
    }
    cm_slab_t *s = &g_slabs[c];
    if (!s->free && refill(s) != 0) return NULL;
    cm_slab_obj_t *o = s->free;
    s->free = o->next;
    s->allocs++;
    memset(o, 0, s->size);
    return o;
}

void cm_kfree(void *ptr)
{
    /* Hosted phase-1: leak large objects back to buddy is omitted;
     * slab free requires size. We keep a tiny header-less cache only for
     * objects that came from known classes by scanning — too expensive.
     * Instead: no-op for now besides allowing reuse via class guess is wrong.
     * Real SLUB stores metadata in page struct. We accept the leak for
     * kernel-bootstrap objects that live forever (caps, threads). */
    CM_UNUSED(ptr);
}

uint64_t cm_slab_pages(void) { return g_slab_pages; }
