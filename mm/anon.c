#include "capmix/trace.h"
#include "capmix/mm.h"
#include "capmix/compress.h"
#include "capmix/io.h"
#include "capmix/proc.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

/* Anonymous memory + the macOS-style compressor (pressure L2/L3).
 *
 *   RESIDENT --L2 compress--> COMPRESSED --L3 swap segment--> SWAPPED
 *       ^                          |                              |
 *       +------- soft fault -------+------------ soft fault ------+
 *
 * The compressor pool is made of ordinary buddy pages ("segments"), so
 * compressing really trades N full pages for ~N/ratio pool pages — RAM
 * the stats can see, not a static array outside the allocator. Blobs are
 * bump-allocated inside a segment with a small header naming their owner
 * slot. L3 writes a WHOLE segment to swap (one 4 KiB I/O) and frees it,
 * like macOS swapping compressor segments rather than single pages; the
 * header lets swap-out find which slots moved without any reverse map.
 *
 * Integrity: every blob header carries a checksum of the blob, verified on
 * swap-in. A segment that comes back damaged loses only the pages whose
 * blobs fail the check (state LOST, access returns NULL) — never silently
 * wrong contents. */

#define A_FREE 0
#define A_RES  1
#define A_COMP 2
#define A_SWAP 3
#define A_LOST 4

#define BLOB_MAGIC   0xB10Bu
#define BLOB_ALIGN   16u
#define BLOB_MAX     3072u          /* worse than 75% is "incompressible" */
#define POOL_MAX     8192u
#define SWAP_SEGS    (CM_BLK_SWAP_SECT / (CM_PAGE_SIZE / CM_BLK_SECTOR))
#define SEG_SECT     (CM_PAGE_SIZE / CM_BLK_SECTOR)

typedef struct {
    cm_id_t  owner;
    uint8_t  state;
    uint8_t  ref;         /* clock referenced bit */
    uint8_t  nocomp;      /* last compression attempt failed */
    uint8_t  inlru;       /* on the resident LRU (incompressible pages are not) */
    uint16_t len;         /* blob payload bytes */
    uint16_t off;         /* blob header offset in pool page / segment */
    uint32_t loc;         /* pool index (COMP) or swap segment (SWAP) */
    void    *page;        /* RESIDENT page */
    uint32_t prev, next;  /* resident LRU list (0 = none; slot 0 is unused) */
} anon_t;

typedef struct {
    uint32_t slot;
    uint16_t len;
    uint16_t magic;
    uint32_t csum;
    uint32_t pad;
} blob_hdr_t;

typedef struct {
    void    *page;
    uint16_t used;        /* bump offset */
    uint16_t nlive;
    uint16_t live_bytes;  /* spans of live blobs: < used/2 means "compact me" */
    int32_t  prev, next;  /* sealed FIFO (oldest first), -1 = none */
    uint8_t  sealed;
} pool_t;

static anon_t  *g_slot;
static uint32_t g_nslots;
static uint32_t g_hint;

/* Every queue below is O(1) per operation. An earlier version scanned the
 * whole slot table (clock hand) and the whole pool table (oldest segment)
 * on every reclaim round; with 32k slots that scan, not compression, was
 * 60% of reclaim time and pushed single allocations past 10 ms. */
static uint32_t g_lru_head, g_lru_tail;      /* resident pages, coldest first */

static pool_t   g_pool[POOL_MAX];
static int      g_pool_open = -1;
static int32_t  g_seal_head = -1, g_seal_tail = -1;
static uint32_t g_pool_free[POOL_MAX];       /* stack of unused descriptors */
static uint32_t g_pool_nfree;
static uint64_t g_pool_pages;
static uint32_t g_seg_rotor;

static uint16_t g_seg_live[SWAP_SEGS];
static uint8_t  g_seg_used[SWAP_SEGS];
static uint64_t g_segs_used;

static uint64_t g_res_n, g_comp_n, g_swap_n;

/* Counters exported through cm_pressure_stats(). */
uint64_t g_anon_l2_pages, g_anon_l2_incomp, g_anon_l3_segs, g_anon_l3_pages;
uint64_t g_anon_swapins, g_anon_decomp, g_anon_lost, g_anon_compact;

static uint8_t g_cbuf[CM_WKDM_MAX + BLOB_ALIGN] __attribute__((aligned(16)));
static uint8_t g_sbuf[CM_PAGE_SIZE + CM_BLK_SECTOR] __attribute__((aligned(16)));

uint64_t cm_anon_pages(void)       { return g_res_n; }
uint64_t cm_compressed_pages(void) { return g_comp_n; }
uint64_t cm_swapped_pages(void)    { return g_swap_n; }
uint64_t cm_pool_pages(void)       { return g_pool_pages; }
uint64_t cm_swap_slots(void)       { return g_segs_used; }
uint64_t cm_swap_total(void)       { return SWAP_SEGS; }

static uint32_t blob_csum(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t blob_span(uint32_t len)
{
    return (uint32_t)((sizeof(blob_hdr_t) + len + BLOB_ALIGN - 1) & ~(BLOB_ALIGN - 1));
}

void cm_anon_init(void)
{
    uint64_t want = cm_mm_total_pages() * 2;
    if (want < 4096) want = 4096;
    unsigned order = 0;
    while (((CM_PAGE_SIZE << order) / sizeof(anon_t)) < want && order < CM_BUDDY_MAX_ORDER)
        order++;
    g_slot = (anon_t *)cm_page_alloc(order);
    g_nslots = g_slot ? (uint32_t)((CM_PAGE_SIZE << order) / sizeof(anon_t)) : 0;
    if (g_slot) memset(g_slot, 0, CM_PAGE_SIZE << order);
    memset(g_pool, 0, sizeof(g_pool));
    memset(g_seg_live, 0, sizeof(g_seg_live));
    memset(g_seg_used, 0, sizeof(g_seg_used));
    for (uint32_t p = 0; p < POOL_MAX; p++) {
        g_pool[p].prev = g_pool[p].next = -1;
        g_pool_free[p] = POOL_MAX - 1 - p;
    }
    g_pool_nfree = POOL_MAX;
    g_pool_open = -1;
    g_seal_head = g_seal_tail = -1;
    g_pool_pages = g_segs_used = 0;
    g_seg_rotor = 0;
    g_res_n = g_comp_n = g_swap_n = 0;
    g_lru_head = g_lru_tail = 0;
    g_hint = 1;
    /* Windows: commit limit = RAM + pagefile. The swap partition backs
     * at least one page per segment (compression only makes it more). */
    cm_commit_add_backing(SWAP_SEGS);
    kinfo("anon: %u page slots, compressor pool <= %u segs, swap %u x 4 KiB segs",
          g_nslots, POOL_MAX, (unsigned)SWAP_SEGS);
}

static anon_t *slot_get(cm_anon_id_t id)
{
    if (!g_slot || id == 0 || id >= g_nslots || g_slot[id].state == A_FREE) return NULL;
    return &g_slot[id];
}

/* ---- resident LRU (second chance) ------------------------------------- */

static void lru_add(uint32_t id)
{
    anon_t *a = &g_slot[id];
    if (a->inlru) return;
    a->inlru = 1;
    a->prev = g_lru_tail;
    a->next = 0;
    if (g_lru_tail) g_slot[g_lru_tail].next = id; else g_lru_head = id;
    g_lru_tail = id;
}

static void lru_del(uint32_t id)
{
    anon_t *a = &g_slot[id];
    if (!a->inlru) return;
    a->inlru = 0;
    if (a->prev) g_slot[a->prev].next = a->next; else g_lru_head = a->next;
    if (a->next) g_slot[a->next].prev = a->prev; else g_lru_tail = a->prev;
    a->prev = a->next = 0;
}

/* ---- compressor pool -------------------------------------------------- */

static void seal_del(uint32_t p)
{
    pool_t *q = &g_pool[p];
    if (!q->sealed) return;
    if (q->prev >= 0) g_pool[q->prev].next = q->next; else g_seal_head = q->next;
    if (q->next >= 0) g_pool[q->next].prev = q->prev; else g_seal_tail = q->prev;
    q->prev = q->next = -1;
    q->sealed = 0;
}

static void seal(uint32_t p)
{
    pool_t *q = &g_pool[p];
    q->sealed = 1;
    q->next = -1;
    q->prev = g_seal_tail;
    if (g_seal_tail >= 0) g_pool[g_seal_tail].next = (int32_t)p; else g_seal_head = (int32_t)p;
    g_seal_tail = (int32_t)p;
}

static void pool_release(uint32_t p)
{
    if (!g_pool[p].page) return;
    seal_del(p);
    cm_page_free(g_pool[p].page, 0);
    g_pool[p].page = NULL;
    g_pool[p].used = 0;
    g_pool[p].nlive = 0;
    g_pool[p].live_bytes = 0;
    g_pool_free[g_pool_nfree++] = p;
    g_pool_pages--;
    if ((int)p == g_pool_open) g_pool_open = -1;
}

static void pool_blob_dead(uint32_t p, uint32_t span)
{
    if (p >= POOL_MAX || !g_pool[p].page) return;
    if (g_pool[p].nlive) g_pool[p].nlive--;
    g_pool[p].live_bytes = (uint16_t)(g_pool[p].live_bytes > span ? g_pool[p].live_bytes - span : 0);
    if (!g_pool[p].nlive) pool_release(p);   /* open one too: never hold an empty page */
}

/* Room for `span` bytes; returns pool index or -1. *new_page is set when
 * a fresh pool page had to be taken (it counts against what L2 frees). */
static int pool_place(uint32_t span, int *new_page)
{
    *new_page = 0;
    if (g_pool_open >= 0 && g_pool[g_pool_open].used + span <= CM_PAGE_SIZE)
        return g_pool_open;
    if (g_pool_open >= 0) {
        int o = g_pool_open;
        g_pool_open = -1;
        if (!g_pool[o].nlive) pool_release((uint32_t)o);
        else seal((uint32_t)o);
    }
    if (!g_pool_nfree) return -1;
    /* Raw allocator on purpose: the reclaim path may dip below the min
     * watermark (Linux PF_MEMALLOC) — it is what frees memory. */
    void *pg = cm_page_alloc(0);
    if (!pg) return -1;
    uint32_t p = g_pool_free[--g_pool_nfree];
    g_pool[p].page = pg;
    g_pool[p].used = 0;
    g_pool[p].nlive = 0;
    g_pool[p].live_bytes = 0;
    g_pool[p].sealed = 0;
    g_pool[p].prev = g_pool[p].next = -1;
    g_pool_pages++;
    g_pool_open = (int)p;
    *new_page = 1;
    return (int)p;
}

/* ---- L2: compress ----------------------------------------------------- */

static int compress_one(uint32_t id, int *pool_new)
{
    anon_t *a = &g_slot[id];
    size_t cl = cm_wkdm_compress(a->page, CM_PAGE_SIZE, g_cbuf, sizeof(g_cbuf));
    if (cl == 0 || cl > BLOB_MAX) {
        a->nocomp = 1;
        g_anon_l2_incomp++;
        return 0;
    }
    uint32_t span = blob_span((uint32_t)cl);
    int p = pool_place(span, pool_new);
    if (p < 0) return -1;
    uint8_t *dst = (uint8_t *)g_pool[p].page + g_pool[p].used;
    blob_hdr_t h = { id, (uint16_t)cl, BLOB_MAGIC, blob_csum(g_cbuf, (uint32_t)cl), 0 };
    memcpy(dst, &h, sizeof(h));
    memcpy(dst + sizeof(h), g_cbuf, cl);
    a->off = g_pool[p].used;
    a->len = (uint16_t)cl;
    a->loc = (uint32_t)p;
    g_pool[p].used = (uint16_t)(g_pool[p].used + span);
    g_pool[p].nlive++;
    g_pool[p].live_bytes = (uint16_t)(g_pool[p].live_bytes + span);

    lru_del(id);
    cm_page_free(a->page, 0);
    a->page = NULL;
    a->state = A_COMP;
    g_res_n--;
    g_comp_n++;
    g_anon_l2_pages++;
    return 1;
}

uint64_t cm_anon_compress(uint64_t want)
{
    if (!g_slot) return 0;
    cm_mm_lock();
    int64_t net = 0;
    /* Second chance over the resident LRU: a referenced page loses its bit
     * and goes to the tail. Budget = two passes over what is resident now,
     * so a fully-hot or fully-incompressible list cannot spin. */
    uint64_t budget = 2 * g_res_n;
    while (net < (int64_t)want && budget-- && g_lru_head) {
        uint32_t id = g_lru_head;
        anon_t *a = &g_slot[id];
        if (a->ref) {
            a->ref = 0;
            lru_del(id);
            lru_add(id);
            continue;
        }
        int pool_new = 0;
        int r = compress_one(id, &pool_new);
        if (r < 0) break;
        /* Incompressible: off the LRU until it is written again (Linux
         * keeps unevictable pages off the scan lists for the same reason —
         * re-trying them on every reclaim was a million wasted steps). */
        if (r == 0) lru_del(id);
        net += r - pool_new;
    }
    cm_mm_unlock();
    return net > 0 ? (uint64_t)net : 0;
}

/* ---- L3: swap out whole compressor segments --------------------------- */

/* Move every live blob of sealed segment p into the open segment, then
 * free p. Returns 1 if p was released. Never touches the open segment. */
static int pool_compact(uint32_t p)
{
    if ((int)p == g_pool_open || !g_pool[p].page) return 0;
    const uint8_t *base = (const uint8_t *)g_pool[p].page;
    for (uint32_t off = 0; off + sizeof(blob_hdr_t) <= g_pool[p].used; ) {
        blob_hdr_t h;
        memcpy(&h, base + off, sizeof(h));
        if (h.magic != BLOB_MAGIC) break;
        uint32_t span = blob_span(h.len);
        anon_t *a = h.slot < g_nslots ? &g_slot[h.slot] : NULL;
        if (a && a->state == A_COMP && a->loc == p && a->off == off) {
            int pool_new = 0;
            int q = pool_place(span, &pool_new);
            if (q < 0 || (uint32_t)q == p) return 0;     /* no room: leave as is */
            memcpy((uint8_t *)g_pool[q].page + g_pool[q].used, base + off, span);
            a->loc = (uint32_t)q;
            a->off = g_pool[q].used;
            g_pool[q].used = (uint16_t)(g_pool[q].used + span);
            g_pool[q].nlive++;
            g_pool[q].live_bytes = (uint16_t)(g_pool[q].live_bytes + span);
            if (g_pool[p].nlive) g_pool[p].nlive--;
        }
        off += span;
    }
    g_pool[p].nlive = 0;
    pool_release(p);
    g_anon_compact++;
    return 1;
}

static int pick_victim_pool(void)
{
    if (g_seal_head >= 0) return g_seal_head;          /* oldest sealed */
    if (g_pool_open >= 0 && g_pool[g_pool_open].nlive) {
        int p = g_pool_open;                           /* last resort */
        g_pool_open = -1;
        return p;
    }
    return -1;
}

uint64_t cm_comp_swapout(uint64_t want)
{
    if (!g_slot) return 0;
    cm_mm_lock();
    uint64_t freed = 0;
    while (freed < want && cm_blk_online()) {
        int p = pick_victim_pool();
        if (p < 0) break;

        /* Minor compaction (macOS c_seg_minorcompact): a segment that is
         * mostly dead space is not worth an I/O and a swap slot. Move its
         * live blobs into the open segment and give the page back. */
        if (g_pool[p].live_bytes * 2u <= g_pool[p].used && pool_compact((uint32_t)p)) {
            freed++;
            continue;
        }

        /* Zero the unused tail: swap-in walks headers until it hits zeros,
         * and the page may hold stale bytes from an earlier life. */
        memset((uint8_t *)g_pool[p].page + g_pool[p].used, 0,
               CM_PAGE_SIZE - g_pool[p].used);
        uint32_t seg = SWAP_SEGS;
        if (g_segs_used < SWAP_SEGS)
            for (uint32_t k = 0; k < SWAP_SEGS; k++) {
                uint32_t s = (g_seg_rotor + k) % SWAP_SEGS;
                if (!g_seg_used[s]) { seg = s; g_seg_rotor = s + 1; break; }
            }
        if (seg == SWAP_SEGS) break;                    /* swap full */

        if (cm_blk_rw(CM_OP_WRITE, CM_BLK_SWAP_LBA + (uint64_t)seg * SEG_SECT,
                      SEG_SECT, g_pool[p].page) != CM_OK)
            break;                                      /* device gone: stay compressed */

        uint32_t moved = 0;
        const uint8_t *base = (const uint8_t *)g_pool[p].page;
        for (uint32_t off = 0; off + sizeof(blob_hdr_t) <= g_pool[p].used; ) {
            blob_hdr_t h;
            memcpy(&h, base + off, sizeof(h));
            if (h.magic != BLOB_MAGIC) break;
            anon_t *a = h.slot < g_nslots ? &g_slot[h.slot] : NULL;
            if (a && a->state == A_COMP && a->loc == (uint32_t)p && a->off == off) {
                a->state = A_SWAP;
                a->loc = seg;
                g_comp_n--;
                g_swap_n++;
                moved++;
            }
            off += blob_span(h.len);
        }
        g_seg_used[seg] = 1;
        g_seg_live[seg] = (uint16_t)moved;
        g_segs_used++;
        g_anon_l3_segs++;
        g_anon_l3_pages += moved;
        g_pool[p].nlive = 0;
        pool_release((uint32_t)p);
        freed++;
        if (!moved) { g_seg_used[seg] = 0; g_segs_used--; }
    }
    cm_mm_unlock();
    return freed;
}

static void seg_blob_dead(uint32_t seg)
{
    if (seg >= SWAP_SEGS || !g_seg_used[seg]) return;
    if (g_seg_live[seg]) g_seg_live[seg]--;
    if (!g_seg_live[seg]) { g_seg_used[seg] = 0; g_segs_used--; }
}

/* ---- allocation / fault / free ---------------------------------------- */

cm_anon_id_t cm_anon_alloc(cm_id_t owner)
{
    if (!g_slot) return 0;
    if (cm_commit_charge(1) != CM_OK) return 0;
    void *pg = cm_page_alloc_user();             /* may reclaim, may jetsam */
    if (!pg) { cm_commit_uncharge(1); return 0; }
    if (owner && !cm_proc_alive(owner)) {
        /* L4 picked the requester itself while making room. */
        cm_page_free(pg, 0);
        cm_commit_uncharge(1);
        return 0;
    }
    cm_mm_lock();
    uint32_t id = 0;
    for (uint32_t k = 0; k < g_nslots - 1; k++) {
        uint32_t i = 1 + (g_hint - 1 + k) % (g_nslots - 1);
        if (g_slot[i].state == A_FREE) { id = i; break; }
    }
    if (!id) {
        cm_mm_unlock();
        cm_page_free(pg, 0);
        cm_commit_uncharge(1);
        return 0;
    }
    g_hint = id + 1 >= g_nslots ? 1 : id + 1;
    memset(pg, 0, CM_PAGE_SIZE);
    anon_t *a = &g_slot[id];
    memset(a, 0, sizeof(*a));
    a->owner = owner;
    a->state = A_RES;
    a->ref = 1;
    a->page = pg;
    lru_add(id);
    g_res_n++;
    cm_mm_unlock();
    return id;
}

/* Swap in a WHOLE segment: one 4 KiB read puts every live blob of `seg`
 * back into the pool as a sealed segment and frees the swap slot at once.
 * Faulting single blobs instead left segments allocated for as long as
 * any one blob in them stayed out — swap filled up with mostly-dead
 * segments and L4 started killing processes L3 should have absorbed.
 * (macOS swaps compressor segments in whole for the same reason.)
 * Returns the pool index, -1 if no page / descriptor (caller falls back to
 * a partial read), -2 on I/O failure. */
static int seg_swapin(uint32_t seg)
{
    if (!g_pool_nfree) return -1;
    void *pg = cm_page_alloc(0);
    if (!pg) return -1;
    if (cm_blk_rw(CM_OP_READ, CM_BLK_SWAP_LBA + (uint64_t)seg * SEG_SECT,
                  SEG_SECT, pg) != CM_OK) {
        cm_page_free(pg, 0);
        return -2;
    }
    uint32_t p = g_pool_free[--g_pool_nfree];
    pool_t *q = &g_pool[p];
    q->page = pg;
    q->used = q->nlive = q->live_bytes = 0;
    q->prev = q->next = -1;
    q->sealed = 0;
    g_pool_pages++;
    const uint8_t *base = (const uint8_t *)pg;
    for (uint32_t off = 0; off + sizeof(blob_hdr_t) <= CM_PAGE_SIZE; ) {
        blob_hdr_t h;
        memcpy(&h, base + off, sizeof(h));
        if (h.magic != BLOB_MAGIC) break;
        uint32_t span = blob_span(h.len);
        if (off + span > CM_PAGE_SIZE) break;
        anon_t *a = h.slot < g_nslots ? &g_slot[h.slot] : NULL;
        if (a && a->state == A_SWAP && a->loc == seg && a->off == off) {
            a->state = A_COMP;
            a->loc = p;
            q->nlive++;
            q->live_bytes = (uint16_t)(q->live_bytes + span);
            g_swap_n--;
            g_comp_n++;
        }
        off += span;
        q->used = (uint16_t)off;
    }
    g_seg_used[seg] = 0;
    g_seg_live[seg] = 0;
    g_segs_used--;
    g_anon_swapins++;
    if (q->nlive) seal(p); else pool_release(p);
    return (int)p;
}

/* Bring a COMP/SWAP page back. Caller holds the mm lock and has a fresh
 * page `pg` ready; on failure the slot becomes LOST. */
static int fault_in(anon_t *a, uint32_t id, void *pg)
{
    const uint8_t *blob;
    blob_hdr_t h;
    int from_swap = a->state == A_SWAP;
    if (from_swap) {
        int r = seg_swapin(a->loc);
        if (r == -2) goto lost;
        /* r >= 0: a is COMP now and takes the pool path below. */
    }
    if (a->state == A_COMP) {
        blob = (const uint8_t *)g_pool[a->loc].page + a->off;
    } else {
        uint32_t s0 = a->off / CM_BLK_SECTOR;
        uint32_t s1 = (a->off + blob_span(a->len) - 1) / CM_BLK_SECTOR;
        if (cm_blk_rw(CM_OP_READ,
                      CM_BLK_SWAP_LBA + (uint64_t)a->loc * SEG_SECT + s0,
                      s1 - s0 + 1, g_sbuf) != CM_OK)
            goto lost;
        blob = g_sbuf + (a->off % CM_BLK_SECTOR);
    }
    memcpy(&h, blob, sizeof(h));
    if (h.magic != BLOB_MAGIC || h.slot != id || h.len != a->len ||
        h.csum != blob_csum(blob + sizeof(h), h.len))
        goto lost;
    if (cm_wkdm_decompress(blob + sizeof(h), h.len, pg, CM_PAGE_SIZE) != CM_PAGE_SIZE)
        goto lost;

    if (a->state == A_COMP) {
        pool_blob_dead(a->loc, blob_span(a->len));
        g_comp_n--;
        if (!from_swap) g_anon_decomp++;
    } else {
        seg_blob_dead(a->loc);                 /* partial-read fallback */
        g_swap_n--;
        g_anon_swapins++;
    }
    a->state = A_RES;
    a->page = pg;
    a->nocomp = 0;
    lru_add(id);
    g_res_n++;
    return 1;

lost:
    if (a->state == A_COMP) { pool_blob_dead(a->loc, blob_span(a->len)); g_comp_n--; }
    else                    { seg_blob_dead(a->loc);  g_swap_n--; }
    a->state = A_LOST;
    g_anon_lost++;
    kerr("anon: page %u (owner %llu) lost — swap I/O or checksum failure",
         id, (unsigned long)a->owner);
    return 0;
}

void *cm_anon_access(cm_anon_id_t id, int write)
{
    cm_mm_lock();
    anon_t *a = slot_get(id);
    if (!a || a->state == A_LOST) { cm_mm_unlock(); return NULL; }
    if (a->state == A_RES) {
        a->ref = 1;
        if (write && a->nocomp) { a->nocomp = 0; lru_add(id); }
        void *p = a->page;
        cm_mm_unlock();
        return p;
    }
    cm_id_t owner = a->owner;
    cm_mm_unlock();

    /* Needs a page: may reclaim, and reclaim may even kill this owner. */
    void *pg = cm_page_alloc_user();
    if (!pg) return NULL;
    cm_mm_lock();
    a = slot_get(id);
    if (!a || a->owner != owner || (a->state != A_COMP && a->state != A_SWAP)) {
        void *r = (a && a->state == A_RES) ? a->page : NULL;
        cm_mm_unlock();
        cm_page_free(pg, 0);
        return r;
    }
    CM_TRACE(MM_FAULT,0,id,owner,a->state);
    if (!fault_in(a, id, pg)) {
        cm_mm_unlock();
        cm_page_free(pg, 0);
        return NULL;
    }
    a->ref = 1;
    cm_mm_unlock();
    return pg;
}

static void free_slot(uint32_t id)
{
    anon_t *a = &g_slot[id];
    switch (a->state) {
    case A_RES:  lru_del(id); cm_page_free(a->page, 0); g_res_n--; break;
    case A_COMP: pool_blob_dead(a->loc, blob_span(a->len)); g_comp_n--; break;
    case A_SWAP: seg_blob_dead(a->loc);  g_swap_n--; break;
    default: break;
    }
    memset(a, 0, sizeof(*a));
    cm_commit_uncharge(1);
}

void cm_anon_free(cm_anon_id_t id)
{
    cm_mm_lock();
    if (slot_get(id)) free_slot(id);
    cm_mm_unlock();
}

uint64_t cm_anon_free_owner(cm_id_t owner)
{
    if (!g_slot || !owner) return 0;
    uint64_t n = 0;
    cm_mm_lock();
    for (uint32_t i = 1; i < g_nslots; i++)
        if (g_slot[i].state != A_FREE && g_slot[i].owner == owner) { free_slot(i); n++; }
    cm_mm_unlock();
    return n;
}

uint64_t cm_anon_owner_pages(cm_id_t owner)
{
    if (!g_slot || !owner) return 0;
    uint64_t n = 0;
    cm_mm_lock();
    for (uint32_t i = 1; i < g_nslots; i++)
        if (g_slot[i].state != A_FREE && g_slot[i].owner == owner) n++;
    cm_mm_unlock();
    return n;
}
