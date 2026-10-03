#include "capmix/mm.h"
#include "capmix/compress.h"
#include "capmix/io.h"
#include "capmix/config.h"
#include "capmix/klog.h"

#include <string.h>

#define CM_CACHE_SLOTS 256
#define CM_ANON_SLOTS  256
#define CM_COMP_SLOTS  128
#define CM_SWAP_MAX    64
#define CM_SWAP_LBA0   64

typedef struct {
    int      used;
    int      dirty;
    cm_id_t  ino;
    uint64_t off;
    void    *page;
} cm_cslot_t;

typedef struct {
    int   used;
    void *page;
} cm_aslot_t;

typedef struct {
    int      used;
    uint16_t len;
    uint8_t  blob[CM_WKDM_MAX];
    int      on_swap;     /* 1 = payload also written to ramdisk */
    uint32_t lba;
} cm_cblob_t;

static cm_cslot_t g_cache[CM_CACHE_SLOTS];
static cm_aslot_t g_anon[CM_ANON_SLOTS];
static cm_cblob_t g_comp[CM_COMP_SLOTS];
static uint64_t   g_file_pages;
static uint64_t   g_anon_n;
static uint64_t   g_comp_n;
static uint64_t   g_swap_n;
static uint32_t   g_swap_next_lba = CM_SWAP_LBA0;

uint64_t cm_file_cache_pages(void) { return g_file_pages; }
uint64_t cm_anon_pages(void)       { return g_anon_n; }
uint64_t cm_compressed_pages(void) { return g_comp_n; }
uint64_t cm_swap_slots(void)       { return g_swap_n; }

cm_error_t cm_cache_add_file(cm_id_t ino, uint64_t off, void *page, int dirty)
{
    for (int i = 0; i < CM_CACHE_SLOTS; i++) {
        if (!g_cache[i].used) {
            g_cache[i].used = 1;
            g_cache[i].dirty = dirty;
            g_cache[i].ino = ino;
            g_cache[i].off = off;
            g_cache[i].page = page;
            g_file_pages++;
            return CM_OK;
        }
    }
    return CM_ERR_NOSPC;
}

uint64_t cm_cache_discard_clean(uint64_t want)
{
    uint64_t n = 0;
    for (int i = 0; i < CM_CACHE_SLOTS && n < want; i++) {
        if (!g_cache[i].used || g_cache[i].dirty) continue;
        if (g_cache[i].page) cm_page_free(g_cache[i].page, 0);
        g_cache[i].used = 0;
        g_cache[i].page = NULL;
        g_file_pages--;
        n++;
    }
    return n;
}

cm_error_t cm_anon_track(void *page)
{
    for (int i = 0; i < CM_ANON_SLOTS; i++) {
        if (!g_anon[i].used) {
            g_anon[i].used = 1;
            g_anon[i].page = page;
            g_anon_n++;
            return CM_OK;
        }
    }
    return CM_ERR_NOSPC;
}

uint64_t cm_anon_compress(uint64_t want)
{
    uint64_t n = 0;
    for (int a = 0; a < CM_ANON_SLOTS && n < want; a++) {
        if (!g_anon[a].used || !g_anon[a].page) continue;
        int slot = -1;
        for (int c = 0; c < CM_COMP_SLOTS; c++)
            if (!g_comp[c].used) { slot = c; break; }
        if (slot < 0) break;

        size_t cl = cm_wkdm_compress(g_anon[a].page, CM_PAGE_SIZE,
                                     g_comp[slot].blob, sizeof(g_comp[slot].blob));
        if (cl == 0) continue;

        g_comp[slot].used = 1;
        g_comp[slot].len = (uint16_t)cl;
        g_comp[slot].on_swap = 0;
        g_comp_n++;

        cm_page_free(g_anon[a].page, 0);
        g_anon[a].used = 0;
        g_anon[a].page = NULL;
        g_anon_n--;
        n++;
    }
    return n;
}

uint64_t cm_comp_swapout(uint64_t want)
{
    uint64_t n = 0;
    for (int c = 0; c < CM_COMP_SLOTS && n < want; c++) {
        if (!g_comp[c].used || g_comp[c].on_swap) continue;
        if (g_swap_n >= CM_SWAP_MAX) break;

        /* Real I/O: write compressed bytes through blk-mq ramdisk. */
        uint8_t sec[CM_BLK_SECTOR];
        memset(sec, 0, sizeof(sec));
        size_t copy = g_comp[c].len < sizeof(sec) ? g_comp[c].len : sizeof(sec);
        memcpy(sec, g_comp[c].blob, copy);
        cm_bio_t bio;
        memset(&bio, 0, sizeof(bio));
        bio.op = CM_OP_WRITE;
        bio.lba = g_swap_next_lba++;
        bio.nsect = 1;
        bio.buf = sec;
        if (cm_blk_submit(&bio) != CM_OK) break;
        cm_bio_t done;
        if (cm_blk_poll(&done) != CM_OK || done.status < 0) break;

        g_comp[c].on_swap = 1;
        g_comp[c].lba = (uint32_t)bio.lba;
        /* Compressed RAM copy can go; disk holds it. */
        g_comp[c].used = 0;
        g_comp_n--;
        g_swap_n++;
        n++;
    }
    return n;
}
