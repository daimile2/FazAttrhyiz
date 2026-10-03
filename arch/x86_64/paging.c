#include "capmix/arch.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/kernel.h"

#include <string.h>

#define PTE_P  1ull
#define PTE_W  2ull
#define PTE_U  4ull
#define PTE_G  (1ull << 8)
#define PTE_PWT (1ull << 3)
#define PTE_PCD (1ull << 4)

/* Kernel text bounds, from linker.ld.
 *
 * WEAK on purpose: if a build's linker script lacks these symbols, the
 * addresses resolve to 0, in_kernel_text() sees e <= s and returns 0 for
 * everything, and the kernel behaves exactly as it did before. A strong
 * reference here would turn a missing script change into a link error
 * instead of a silent, safe fallback. */
extern char __text_start[] __attribute__((weak));
extern char __text_end[]   __attribute__((weak));

static uint64_t *g_pml4;
static uint64_t  g_tables;
static uint64_t  g_mapped_pages;
static uint64_t  g_boot_stack[16384] __attribute__((used, aligned(16)));
static uint64_t  g_loaded_cr3;
static uint64_t  g_mmio_next;

/* True for addresses inside the kernel's executable text.
 *
 * Dropping PTE_W for these pages is what turns SIP's "root cannot write
 * kernel text" from a software check into a hardware fact. Right now
 * cm_sip() refuses the write in code, but the page is still mapped
 * writable — so a wild store from anywhere in the kernel, or from a
 * user-mode driver, can still clobber an instruction. With W cleared, that
 * store faults instead. */
static int in_kernel_text(uint64_t virt)
{
    uint64_t s = (uint64_t)(uintptr_t)__text_start;
    uint64_t e = (uint64_t)(uintptr_t)__text_end;
    return (e > s) && (virt >= s) && (virt < e);
}

static uint64_t v2p(void *v)
{
    uint64_t h = cm_hhdm();
    uint64_t a = (uint64_t)(uintptr_t)v;
    if (h && a >= h)
        return a - h;
    return a;
}

static void *p2v(uint64_t p)
{
    uint64_t h = cm_hhdm();
    if (h)
        return (void *)(uintptr_t)(p + h);
    return (void *)(uintptr_t)p;
}

/* Walk to a leaf PTE without allocating. Used by cm_paging_dump() to report
 * whether text actually ended up read-only. */
static uint64_t *pte_of(uint64_t virt)
{
    if (!g_pml4) return NULL;
    uint64_t *pdpt, *pd, *pt;
    uint64_t i4 = (virt >> 39) & 0x1ff;
    uint64_t i3 = (virt >> 30) & 0x1ff;
    uint64_t i2 = (virt >> 21) & 0x1ff;
    uint64_t i1 = (virt >> 12) & 0x1ff;

    if (!(g_pml4[i4] & PTE_P)) return NULL;
    pdpt = (uint64_t *)p2v(g_pml4[i4] & ~0xFFFull);
    if (!(pdpt[i3] & PTE_P)) return NULL;
    pd = (uint64_t *)p2v(pdpt[i3] & ~0xFFFull);
    if (!(pd[i2] & PTE_P)) return NULL;
    pt = (uint64_t *)p2v(pd[i2] & ~0xFFFull);
    if (!(pt[i1] & PTE_P)) return NULL;
    return &pt[i1];
}

static uint64_t *new_table(void)
{
    void *p = cm_page_alloc(0);
    if (!p) return NULL;
    memset(p, 0, CM_PAGE_SIZE);
    g_tables++;
    return (uint64_t *)p;
}

static int map_page_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!pml4) return -1;
    virt &= ~0xFFFull;
    phys &= ~0xFFFull;
    uint64_t i4 = (virt >> 39) & 0x1ff;
    uint64_t i3 = (virt >> 30) & 0x1ff;
    uint64_t i2 = (virt >> 21) & 0x1ff;
    uint64_t i1 = (virt >> 12) & 0x1ff;

    if (!(pml4[i4] & PTE_P)) {
        uint64_t *t = new_table();
        if (!t) return -1;
        pml4[i4] = v2p(t) | PTE_P | PTE_W | (flags & PTE_U);
    }
    uint64_t *pdpt = (uint64_t *)p2v(pml4[i4] & ~0xFFFull);
    if (!(pdpt[i3] & PTE_P)) {
        uint64_t *t = new_table();
        if (!t) return -1;
        pdpt[i3] = v2p(t) | PTE_P | PTE_W | (flags & PTE_U);
    }
    uint64_t *pd = (uint64_t *)p2v(pdpt[i3] & ~0xFFFull);
    if (!(pd[i2] & PTE_P)) {
        uint64_t *t = new_table();
        if (!t) return -1;
        pd[i2] = v2p(t) | PTE_P | PTE_W | (flags & PTE_U);
    }
    uint64_t *pt = (uint64_t *)p2v(pd[i2] & ~0xFFFull);
    if (!(pt[i1] & PTE_P))
        g_mapped_pages++;
    /* Global only on kernel, non-user leaves. User pages must not be G. */
    uint64_t g = (virt >= 0xffff800000000000ull && !(flags & PTE_U)) ? PTE_G : 0;

    /* Executable text gets no write permission.
     *
     * Intermediate tables keep PTE_W — write permission is the AND of every
     * level, so clearing it on the leaf alone is sufficient and leaves the
     * table walk itself untouched. */
    uint64_t w = in_kernel_text(virt) ? 0 : PTE_W;

    pt[i1] = phys | PTE_P | w | (flags & (PTE_U | PTE_PCD | PTE_PWT)) | g;
    return 0;
}

int cm_space_map_flags(uint64_t *pml4, uint64_t virt, uint64_t phys, int uncached)
{
    uint64_t f = uncached ? (PTE_PCD | PTE_PWT) : 0;
    return map_page_in(pml4, virt, phys, f);
}

/* Map device MMIO at a fresh 2 MiB-aligned slot above the kernel image.
 *
 * Deliberately does NOT reuse whatever convention cm_lapic_map_before_cr3()
 * uses: that one runs before CR3 is loaded and its addressing is not visible
 * from here. Picking our own guarded slots keeps MMIO clear of .text/.data
 * and of the heap.
 *
 * Call only AFTER cm_paging_init() has loaded CR3 — these PTEs go into the
 * live page table. A page that was not present before cannot have a stale
 * TLB entry, so a first-time mapping needs no invalidation. */
void *cm_mmio_map(uint64_t phys, uint64_t npages)
{
    if (!g_pml4 || npages == 0) return NULL;
    if (!g_mmio_next) {
        uint64_t kv = cm_kernel_virt();
        uint64_t ks = cm_kernel_size();
        if (!kv || !ks) return NULL;
        g_mmio_next = (kv + ks + 0x200000ull) & ~0x1fffffull;
    }
    uint64_t virt = g_mmio_next;
    for (uint64_t i = 0; i < npages; i++) {
        if (map_page_in(g_pml4, virt + i * CM_PAGE_SIZE,
                        phys + i * CM_PAGE_SIZE, PTE_PCD | PTE_PWT) != 0)
            return NULL;
    }
    /* 2 MiB guard gap between devices catches a one-page overrun instead of
     * letting it land in the next device's registers. */
    g_mmio_next = (virt + npages * CM_PAGE_SIZE + 0x200000ull) & ~0x1fffffull;
    return (void *)(uintptr_t)virt;
}

static int map_page(uint64_t virt, uint64_t phys)
{
    return map_page_in(g_pml4, virt, phys, 0);
}

void *cm_boot_stack_base(void) { return g_boot_stack; }

uint64_t *cm_paging_pml4(void) { return g_pml4; }
uint64_t  cm_paging_cr3(void) { return g_pml4 ? v2p(g_pml4) : 0; }

uint64_t *cm_space_create(uint64_t *out_phys)
{
    uint64_t *p = new_table();
    if (!p || !g_pml4) return NULL;
    for (int i = 256; i < 512; i++)
        p[i] = g_pml4[i];
    if (out_phys) *out_phys = v2p(p);
    return p;
}

int cm_space_map(uint64_t *pml4, uint64_t virt, uint64_t phys, int user)
{
    return map_page_in(pml4, virt, phys, user ? PTE_U : 0);
}

void cm_space_load(uint64_t cr3)
{
#ifndef CAPMIX_HOSTED
    if (cr3)
        __asm__ volatile("movq %0, %%cr3" :: "r"(cr3) : "memory");
#else
    (void)cr3;
#endif
}

static int map_range(uint64_t virt, uint64_t phys, uint64_t npages)
{
    for (uint64_t i = 0; i < npages; i++) {
        if (map_page(virt + i * CM_PAGE_SIZE, phys + i * CM_PAGE_SIZE) != 0)
            return -1;
    }
    return 0;
}

int cm_paging_init(cm_paddr_t heap_virt, uint64_t npages)
{
    g_tables = 0;
    g_mapped_pages = 0;
    g_pml4 = new_table();
    if (!g_pml4) return -1;

#ifdef CAPMIX_HOSTED
    /* Hosted: walk a short span so the builder is tested. No CR3. */
    uint64_t n = npages < 32 ? npages : 32;
    (void)map_range((uint64_t)heap_virt, (uint64_t)heap_virt, n ? n : 32);
    kinfo("paging: pml4=%p tables=%llu mapped=%llu (CR3 skipped on hosted)",
          (void *)g_pml4, (unsigned long)g_tables, (unsigned long)g_mapped_pages);
    return 0;
#else
    uint64_t kv = cm_kernel_virt();
    uint64_t kp = cm_kernel_phys();
    uint64_t ks = cm_kernel_size();
    if (kv && kp && ks) {
        /* Round up AND add a cushion.
         *
         * The kernel image ends at __bss_end and the BSS is ~4.25MB (g_arena
         * alone is 2MB), so cm_kernel_size() can sit within a few dozen KB of
         * the real end. Mapping exactly that many pages leaves no room: any
         * new static — even 16 bytes — pushes the image past the last mapped
         * page and the box dies before the first kinfo prints.
         *
         * Extra pages cost nothing (the buddy already excludes this range)
         * and they turn "adding a static kills boot" into a non-event. */
        uint64_t kn = (ks + CM_PAGE_SIZE - 1) / CM_PAGE_SIZE;
        if (map_range(kv, kp, kn) != 0)
            kwarn("paging: kernel map incomplete");
        kinfo("paging: mapped kernel virt=%llx phys=%llx pages=%llu",
              (unsigned long)kv, (unsigned long)kp, (unsigned long)kn);
    }

    uint64_t hhdm = cm_hhdm();
    uint64_t heap_phys = (hhdm && (uint64_t)heap_virt >= hhdm)
        ? (uint64_t)heap_virt - hhdm : (uint64_t)heap_virt;
    if (npages && map_range((uint64_t)heap_virt, heap_phys, npages) != 0)
        kwarn("paging: heap map incomplete");
    kinfo("paging: mapped heap virt=%llx phys=%llx pages=%llu",
          (unsigned long)heap_virt, (unsigned long)heap_phys,
          (unsigned long)npages);
    cm_lapic_map_before_cr3();

    if (hhdm && g_pml4) {
        g_loaded_cr3 = v2p(g_pml4);
        __asm__ volatile(
            "leaq g_boot_stack(%%rip), %%rsp\n\t"
            "addq $0x1fff0, %%rsp\n\t"
            "xorq %%rbp, %%rbp\n\t"
            "movq %%rax, %%cr3\n\t"
            "call paging_land\n\t"
            "1: hlt; jmp 1b\n\t"
            :
            : "a"(g_loaded_cr3)
            : "memory"
        );
    }
    kinfo("paging: tables=%llu mapped=%llu (no HHDM, CR3 not loaded)",
          (unsigned long)g_tables, (unsigned long)g_mapped_pages);
    capmix_after_paging();
    return 0;
#endif
}

#ifndef CAPMIX_HOSTED
static void mark_kernel_global(void)
{
    if (!g_pml4) return;
    for (int i4 = 256; i4 < 512; i4++) {
        if (!(g_pml4[i4] & PTE_P)) continue;
        uint64_t *pdpt = (uint64_t *)p2v(g_pml4[i4] & ~0xFFFull);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_P)) continue;
            uint64_t *pd = (uint64_t *)p2v(pdpt[i3] & ~0xFFFull);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_P)) continue;
                uint64_t *pt = (uint64_t *)p2v(pd[i2] & ~0xFFFull);
                for (int i1 = 0; i1 < 512; i1++) {
                    if ((pt[i1] & (PTE_P | PTE_U)) == PTE_P)
                        pt[i1] |= PTE_G;
                }
            }
        }
    }
}

void cm_paging_enable_pge(void)
{
#ifndef CAPMIX_HOSTED
    uint64_t cr4 = cm_read_cr4();
    cr4 |= (1ull << 7);
    __asm__ volatile("movq %0, %%cr4" :: "r"(cr4) : "memory");
    mark_kernel_global();
    kinfo("paging: CR4.PGE on cr4=%llx kernel leaves marked Global",
          (unsigned long)cm_read_cr4());
#endif
}

void paging_land(void)
{
    {
        uint64_t sp;
        __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
        kinfo("probe rsp=%llx (boot_stack=%p) land",
              (unsigned long)sp, (void *)g_boot_stack);
    }
    kinfo("paging: CR3=%llx tables=%llu mapped=%llu stack=bss (own walker live)",
          (unsigned long)g_loaded_cr3,
          (unsigned long)g_tables,
          (unsigned long)g_mapped_pages);
    cm_paging_enable_pge();
    capmix_after_paging();
    cm_irq_enable();
    kinfo("freestanding: paging live — selftest");
    int rc = capmix_selftest();
    kinfo("freestanding selftest rc=%d — hlt", rc);
    for (;;)
        __asm__ volatile("hlt");
}
#endif

uint64_t cm_paging_tables(void) { return g_tables; }

void cm_paging_dump(void)
{
    kinfo("paging: tables=%llu mapped_pages=%llu pml4=%p cr3_would=%llx",
          (unsigned long)g_tables, (unsigned long)g_mapped_pages, (void *)g_pml4,
          (unsigned long)(g_pml4 ? v2p(g_pml4) : 0));

    /* Report whether text actually ended up read-only. Without this line the
     * W-bit change is invisible from the log and a silently missing linker
     * symbol would look identical to success. */
    uint64_t s = (uint64_t)(uintptr_t)__text_start;
    uint64_t e = (uint64_t)(uintptr_t)__text_end;
    if (e > s) {
        uint64_t *a = pte_of(s);
        uint64_t *b = pte_of(e - CM_PAGE_SIZE);
        int ro = (a && b) ? (((*a & PTE_W) == 0) && ((*b & PTE_W) == 0)) : -1;
        kinfo("paging: kernel text %llx..%llx (%llu pages) write_protected=%d",
              (unsigned long)s, (unsigned long)e,
              (unsigned long)((e - s) / CM_PAGE_SIZE), ro);
    } else {
        kinfo("paging: kernel text symbols absent — W bit NOT cleared");
    }
}