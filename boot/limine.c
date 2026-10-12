#include "capmix/arch.h"
#include "capmix/console.h"
#include "capmix/boot.h"
#include <string.h>
#include "capmix/klog.h"
#include "capmix/types.h"

#include <stdint.h>

/* Snapshot before replacing Limine page tables; no later response reads. */
static struct { uint64_t base,length; } usable[128];
static unsigned usable_count;
int cm_limine_usable(unsigned index,uint64_t *base,uint64_t *length)
{
    if (index>=usable_count || !base || !length) return -1;
    *base=usable[index].base; *length=usable[index].length; return 0;
}

static uint64_t g_hhdm;
static uint64_t g_kphys;
static uint64_t g_kvirt;
static uint64_t g_ksize;

uint64_t cm_hhdm(void)        { return g_hhdm; }
uint64_t cm_kernel_phys(void) { return g_kphys; }
uint64_t cm_kernel_virt(void) { return g_kvirt; }
uint64_t cm_kernel_size(void) { return g_ksize; }

static cm_fb_info_t fb_info;
int cm_limine_fb_info(cm_fb_info_t *out)
{
    if (!out || !fb_info.physical) return -1;
    *out = fb_info; return 0;
}

static char boot_cmdline[256];
int cm_boot_parse_selftest(const char *line)
{
    static const char debug[]="capmix.mode=selftest", product[]="capmix.mode=product";
    int selftest=0;
    if(!line)return 0;
    while(*line) {
        while(*line==' ' || *line=='\t')line++;
        const char *begin=line;
        while(*line && *line!=' ' && *line!='\t')line++;
        size_t n=(size_t)(line-begin);
        if(n==sizeof(debug)-1 && !memcmp(begin,debug,n))selftest=1;
        if(n==sizeof(product)-1 && !memcmp(begin,product,n))selftest=0;
    }
    return selftest;
}
int cm_boot_is_selftest(void)
{
#ifdef CAPMIX_HOSTED
    return 1; /* host executable remains the regression runner */
#else
    return cm_boot_parse_selftest(boot_cmdline);
#endif
}

#ifdef CAPMIX_HOSTED
void cm_boot_cmdline_snapshot(void) { memcpy(boot_cmdline,"capmix.mode=selftest",20); }
void cm_limine_fb_snapshot(void) {}

int cm_limine_memory(cm_paddr_t *base, uint64_t *npages, uint64_t *hhdm)
{
    if (base) *base = 0;
    if (npages) *npages = 0;
    if (hhdm) *hhdm = 0;
    return -1;
}

int cm_limine_mp(uint64_t *cpu_count, uint32_t *bsp_lapic_id,
                 uint32_t *lapic_ids, int max_ids)
{
    if (cpu_count) *cpu_count = 1;
    if (bsp_lapic_id) *bsp_lapic_id = 0;
    if (lapic_ids && max_ids > 0) lapic_ids[0] = 0;
    return -1;
}
int cm_limine_mp_start(uint64_t idx, void *entry)
{
    (void)idx;
    (void)entry;
    return -1;
}
#else

#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88ull, 0x0a82e883a194f07bull

/* Limine protocol sentinels — without these the request block is invisible. */
__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[4] = {
    0xf6b8f4b39de7d1aeull, 0xfab91a6940fcb9cfull,
    0x785c6ed015d3e316ull, 0x181e920a7852b9d9ull
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[2] = {
    0xadc0e0531bb10d03ull, 0x9572709f31764c62ull
};

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_rev[3] = {
    0xf9562b2d5c95a6c8ull, 0x6a7b384944536bdcull, 3
};

struct limine_req {
    uint64_t id[4];
    uint64_t revision;
    void    *response;
};

struct limine_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_mmap_resp {
    uint64_t revision;
    uint64_t entry_count;
    struct limine_mmap_entry **entries;
};

struct limine_hhdm_resp {
    uint64_t revision;
    uint64_t offset;
};

struct limine_kaddr_resp {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
};

/* MP — base revision is 3, so limine.h names these limine_mp_*, NOT
 * limine_smp_*. The magic is identical between the two spellings
 * (0x95a67b819a1b857e / 0xa0b61b723b6a73e0), only the struct names moved.
 * Layout below matches LIMINE_API_REVISION >= 1 for x86_64. */
struct limine_mp_info {
    uint32_t processor_id;
    uint32_t lapic_id;
    uint64_t reserved;
    void (*goto_address)(struct limine_mp_info *);
    uint64_t extra_argument;
};

struct limine_mp_resp {
    uint64_t revision;
    uint32_t flags;
    uint32_t bsp_lapic_id;
    uint64_t cpu_count;
    struct limine_mp_info **cpus;
};

#define LIMINE_MP_MAGIC 0x95a67b819a1b857eull, 0xa0b61b723b6a73e0ull

__attribute__((used, section(".limine_requests")))
static volatile struct limine_req mmap_req = {
    .id = { LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806full, 0xe304acdfc50c3c62ull },
    .revision = 0,
    .response = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_req hhdm_req = {
    .id = { LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852ull, 0x63984e959a98244bull },
    .revision = 0,
    .response = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_req kaddr_req = {
    .id = { LIMINE_COMMON_MAGIC, 0x71ba76863cc55f63ull, 0xb2644a48c516a487ull },
    .revision = 0,
    .response = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_req mp_req = {
    .id = { LIMINE_COMMON_MAGIC, LIMINE_MP_MAGIC },
    .revision = 0,
    .response = 0,
};
/* RSDP — needed to reach the MADT for the IOAPIC address and IRQ->GSI
 * overrides. Limine returns the physical address of the RSDP. */
#define LIMINE_RSDP_MAGIC 0xc5e77b6b397e7b43ull, 0x27637845accdcf3cull

__attribute__((used, section(".limine_requests")))
static volatile struct limine_req rsdp_req = {
    .id = { LIMINE_COMMON_MAGIC, LIMINE_RSDP_MAGIC },
    .revision = 0,
    .response = 0,
};

struct cmdline_response { uint64_t revision; const char *cmdline; };
struct kernel_file_prefix { uint64_t revision; void *address; uint64_t size; const char *path,*cmdline; };
struct kernel_file_response { uint64_t revision; struct kernel_file_prefix *file; };
__attribute__((used, section(".limine_requests")))
static volatile struct limine_req cmdline_req = {
    .id = { LIMINE_COMMON_MAGIC, 0x4b161536e598651eull, 0xb390ad4a2f1f303aull }
};
__attribute__((used, section(".limine_requests")))
static volatile struct limine_req kernel_file_req = {
    .id = { LIMINE_COMMON_MAGIC, 0xad97e90e83f1ed67ull, 0x31eb5d1c5ff23b69ull }
};
void cm_boot_cmdline_snapshot(void)
{
    struct cmdline_response *r=cmdline_req.response;
    const char *line=r?r->cmdline:NULL;
    if(!line) {
        struct kernel_file_response *f=kernel_file_req.response;
        if(f && f->file)line=f->file->cmdline;
    }
    unsigned n=0;
    if(line)while(n+1<sizeof(boot_cmdline) && line[n]) {boot_cmdline[n]=line[n];n++;}
    boot_cmdline[n]=0;
    kinfo("boot mode: %s (Limine cmdline snapshot)",cm_boot_is_selftest()?"selftest":"product");
}

struct fb_desc {
    void *address;
    uint64_t width, height, pitch;
    uint16_t bpp;
    uint8_t model, rs, rp, gs, gp, bs, bp;
};
struct fb_response { uint64_t revision, count; struct fb_desc **framebuffers; };
__attribute__((used, section(".limine_requests")))
static volatile struct limine_req fb_req = {
    .id = { LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75ull, 0xa3148604f6fab11bull }
};
void cm_limine_fb_snapshot(void)
{
    /* Called on Limine's original page tables, before any response is unmapped. */
    struct fb_response *r = fb_req.response;
    if (!r || !r->count || !r->framebuffers || !r->framebuffers[0]) return;
    struct fb_desc *f = r->framebuffers[0];
    uint64_t address = (uint64_t)(uintptr_t)f->address;
    if (!g_hhdm || address < g_hhdm) return;
    fb_info = (cm_fb_info_t){ address-g_hhdm, f->width,f->height,f->pitch,
        f->bpp,f->model,f->rs,f->rp,f->gs,f->gp,f->bs,f->bp };
}

static uint64_t *g_pml4_cached;
static uint64_t  g_hhdm_cached;

int cm_limine_ensure_mapped(uint64_t virt, uint64_t len);
extern uint64_t *cm_paging_pml4(void);
extern uint64_t  cm_hhdm(void);
static void rsdp_cache_init(void);

struct limine_rsdp_resp {
    uint64_t revision;
    uint64_t address;
};

static void rsdp_cache_init(void)
{
    if (!g_pml4_cached) g_pml4_cached = cm_paging_pml4();
    if (!g_hhdm_cached) g_hhdm_cached = cm_hhdm();
}

uint64_t cm_limine_rsdp(void)
{
    void *rp = (void *)rsdp_req.response;
    if (!rp) return 0;

    /* The response sits at an HHDM address our tables may not cover — for
     * the RSDP it is phys 0x1fab0000, which is BEFORE the kernel image at
     * 0x1fab4000. Map it before touching it, same as the MP response
     * does, otherwise this page-faults and takes the box down. */
    rsdp_cache_init();
    if (!g_pml4_cached || !g_hhdm_cached) return 0;
    if (cm_limine_ensure_mapped((uint64_t)(uintptr_t)rp,
                            sizeof(struct limine_rsdp_resp)) != 0)
        return 0;

    struct limine_rsdp_resp *rr = (struct limine_rsdp_resp *)rp;
    uint64_t a = rr->address;
    return a ? a : 0;
}


extern uint8_t __kernel_start[], __kernel_end[];

static int usable_piece(uint64_t b, uint64_t len,
                        uint64_t k0, uint64_t k1,
                        uint64_t *ob, uint64_t *olen)
{
    if (len>UINT64_MAX-b) return 0;
    uint64_t e = b + len;
    uint64_t a0 = b, a1 = e, c0 = 0, c1 = 0;
    if (k1 > k0 && !(e <= k0 || b >= k1)) {
        a1 = k0 < e ? k0 : e;
        if (a1 < a0) a1 = a0;
        c0 = k1 > b ? k1 : b;
        c1 = e;
        if (c1 < c0) c1 = c0;
    }
    uint64_t l0 = a1 > a0 ? a1 - a0 : 0;
    uint64_t l1 = c1 > c0 ? c1 - c0 : 0;
    if (l1 > l0) { *ob = c0; *olen = l1; }
    else { *ob = a0; *olen = l0; }
    return *olen >= CM_PAGE_SIZE;
}

int cm_limine_memory(cm_paddr_t *base, uint64_t *npages, uint64_t *hhdm)
{
    struct limine_hhdm_resp *hr = (struct limine_hhdm_resp *)hhdm_req.response;
    struct limine_mmap_resp *mr = (struct limine_mmap_resp *)mmap_req.response;
    struct limine_kaddr_resp *kr = (struct limine_kaddr_resp *)kaddr_req.response;
    kinfo("req: mmap=%p hhdm=%p kaddr=%p",
          (void *)mmap_req.response, (void *)hhdm_req.response,
          (void *)kaddr_req.response);
    g_hhdm = hr ? hr->offset : 0;
    if (hhdm) *hhdm = g_hhdm;
    if (kr) {
        g_kphys = kr->physical_base;
        g_kvirt = kr->virtual_base;
        g_ksize = (uint64_t)(__kernel_end - __kernel_start);
        kinfo("kaddr: phys=%llx virt=%llx size=%llu (excluded from buddy)",
              (unsigned long)g_kphys, (unsigned long)g_kvirt,
              (unsigned long)g_ksize);
    }
    if (!mr || !mr->entries || mr->entry_count == 0) return -1;

    uint64_t k0 = g_kphys, k1 = g_kphys + g_ksize;

    usable_count=0;
    uint64_t best_len = 0, best_base = 0;
    for (uint64_t i = 0; i < mr->entry_count; i++) {
        struct limine_mmap_entry *e = mr->entries[i];
        if (!e || e->type != 0) continue;
        uint64_t pb = 0, pl = 0;
        if (!usable_piece(e->base, e->length, k0, k1, &pb, &pl)) continue;
        if (usable_count<128 && pl<=UINT64_MAX-pb) {
            usable[usable_count].base=pb; usable[usable_count++].length=pl;
        }
        if (pl > best_len) {
            best_len = pl;
            best_base = pb;
        }
    }
    if (best_len < CM_PAGE_SIZE) return -1;
    if (base) *base = best_base;
    if (npages) *npages = best_len / CM_PAGE_SIZE;
    return 0;
}
/* --- on-demand HHDM mapping for Limine response pages ------------------
 * Limine hands back response structures at HHDM addresses that our own
 * page tables do not necessarily cover. mmap/hhdm/kaddr happened to land
 * inside a mapped region; the MP response did not, which is the -smp 4
 * #PF at cr2=0xffff80001ff60010 (== &resp->cpu_count). Stop relying on
 * luck: walk the tables first and map the page on demand. */

static uint64_t *lvl_next(uint64_t entry)
{
    /* Table entries hold PHYSICAL addresses; we address them through HHDM. */
    return (uint64_t *)(uintptr_t)((entry & 0x000ffffffffff000ull) + g_hhdm_cached);
}

static uint64_t walk_pte(uint64_t virt)
{
    if (!g_pml4_cached) return 0;
    uint64_t *l = g_pml4_cached;
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t e = l[(virt >> shift) & 0x1ff];
        if (!(e & 1)) return 0;
        if (shift == 12 || (e & (1ull << 7))) return e; /* PTE, or huge page */
        l = lvl_next(e);
    }
    return 0;
}

int cm_limine_ensure_mapped(uint64_t virt, uint64_t len)
{
    uint64_t start = virt & ~0xfffull;
    uint64_t end   = (virt + len + 0xfff) & ~0xfffull;
    int mapped = 0;
    for (uint64_t v = start; v < end; v += 0x1000) {
        if (walk_pte(v) & 1) continue;
        uint64_t phys = v - g_hhdm_cached;
        if (cm_space_map_flags(g_pml4_cached, v, phys, 0) != 0) {
            kwarn("mp: cannot map response page %llx (phys %llx)",
                  (unsigned long)v, (unsigned long)phys);
            return -1;
        }
        __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory");
        mapped++;
    }
    if (mapped)
        kinfo("mp: mapped %d response page(s) on demand at %llx",
              mapped, (unsigned long)start);
    return 0;
}
int cm_limine_mp(uint64_t *cpu_count, uint32_t *bsp_lapic_id,
                 uint32_t *lapic_ids, int max_ids)
{
    struct limine_mp_resp *mr = (struct limine_mp_resp *)mp_req.response;

    kinfo("mp: response=%p max_ids=%d", (void *)mr, max_ids);
    if (!mr) {
        kinfo("mp: no response (request not answered)");
        if (cpu_count) *cpu_count = 1;
        if (bsp_lapic_id) *bsp_lapic_id = 0;
        if (lapic_ids && max_ids > 0) lapic_ids[0] = 0;
        return -1;
    }

    g_pml4_cached = cm_paging_pml4();
    g_hhdm_cached = cm_hhdm();
    if (!g_pml4_cached || !g_hhdm_cached) {
        kwarn("mp: no pml4/hhdm yet — refusing to dereference %p", (void *)mr);
        if (cpu_count) *cpu_count = 1;
        if (bsp_lapic_id) *bsp_lapic_id = 0;
        if (lapic_ids && max_ids > 0) lapic_ids[0] = 0;
        return -1;
    }

    if (cm_limine_ensure_mapped((uint64_t)(uintptr_t)mr, sizeof(*mr)) != 0) {
        if (cpu_count) *cpu_count = 1;
        if (bsp_lapic_id) *bsp_lapic_id = 0;
        if (lapic_ids && max_ids > 0) lapic_ids[0] = 0;
        return -1;
    }

    kinfo("mp: revision=%llu flags=%x bsp=%u cpu_count=%llu cpus=%p",
          (unsigned long)mr->revision, (unsigned)mr->flags,
          (unsigned)mr->bsp_lapic_id, (unsigned long)mr->cpu_count,
          (void *)mr->cpus);

    if (cpu_count) *cpu_count = mr->cpu_count;
    if (bsp_lapic_id) *bsp_lapic_id = mr->bsp_lapic_id;

    uint64_t n = mr->cpu_count;

    /* Sanity BEFORE dereferencing the array. */
    if (n == 0 || n > 256) {
        kwarn("mp: cpu_count=%llu implausible — struct layout suspect",
              (unsigned long)n);
        n = 0;
    } else if (!mr->cpus) {
        kwarn("mp: cpus array is NULL");
        n = 0;
    } else if ((uint64_t)(uintptr_t)mr->cpus < 0xffff800000000000ull) {
        kwarn("mp: cpus=%p is not an HHDM address — struct layout suspect",
              (void *)mr->cpus);
        n = 0;
    } else if (cm_limine_ensure_mapped((uint64_t)(uintptr_t)mr->cpus,
                                   n * sizeof(void *)) != 0) {
        n = 0;
    }

    if (n > (uint64_t)max_ids) n = (uint64_t)max_ids;
    for (uint64_t i = 0; i < n; i++) {
        struct limine_mp_info *ci = mr->cpus[i];
        if (!ci) { if (lapic_ids) lapic_ids[i] = 0; continue; }
        if (cm_limine_ensure_mapped((uint64_t)(uintptr_t)ci, sizeof(*ci)) != 0) {
            if (lapic_ids) lapic_ids[i] = 0;
            continue;
        }
        if (lapic_ids) lapic_ids[i] = ci->lapic_id;
        kinfo("mp:   [%llu] info=%p lapic_id=%u",
              (unsigned long)i, (void *)ci, (unsigned)ci->lapic_id);
    }
    for (int i = (int)n; i < max_ids; i++)
        if (lapic_ids) lapic_ids[i] = 0;
    return 0;
}
/* Per the Limine protocol: an ATOMIC WRITE to goto_address makes the
 * parked CPU jump to the written address. It is a field to STORE INTO,
 * NOT a function to call. Limine passes &info in RDI to the AP.
 *
 * Safe to dereference ci here: cm_smp_probe() already ran
 * cm_limine_mp(), which cm_limine_ensure_mapped()'d every info struct. */
int cm_limine_mp_start(uint64_t idx, void *entry)
{
    struct limine_mp_resp *mr = (struct limine_mp_resp *)mp_req.response;
    if (!mr || !mr->cpus || idx >= mr->cpu_count) return -1;

    struct limine_mp_info *ci = mr->cpus[idx];
    if (!ci) return -1;
    if (ci->lapic_id == mr->bsp_lapic_id) return -2;  /* that's us, not an AP */

    ci->extra_argument = idx;
    __atomic_store_n(&ci->goto_address,
                     (void (*)(struct limine_mp_info *))entry,
                     __ATOMIC_RELEASE);
    return 0;
}

#endif