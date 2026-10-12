#include "capmix/kernel.h"
#include "capmix/console.h"
#include "capmix/boot.h"

#ifndef CAPMIX_HOSTED

extern uint8_t __bss_start[], __bss_end[];

__attribute__((aligned(4096)))
static uint8_t g_arena[2 * 1024 * 1024];

static void bss_zero(void)
{
    uint64_t *p = (uint64_t *)__bss_start;
    uint64_t *e = (uint64_t *)((uintptr_t)__bss_end & ~(uintptr_t)7);
    for (; p < e; p++) *p = 0;
    for (uint8_t *q = (uint8_t *)e; q < __bss_end; q++) *q = 0;
}

void kmain(void)
{
    bss_zero();
    cm_serial_init();
    cm_boot_cmdline_snapshot();

    cm_mem_region_t r = {
        .base = (cm_paddr_t)(uintptr_t)g_arena,
        .npages = sizeof(g_arena) / CM_PAGE_SIZE,
        .type = 1,
    };
    const char *src = "bss-fallback";

    cm_paddr_t phys = 0;
    uint64_t np = 0, hhdm = 0;
    if (cm_limine_memory(&phys, &np, &hhdm) == 0 && hhdm != 0 && np > 32) {
        uint64_t skip = 16;
        if (np <= skip + 16) skip = 0;
        uint64_t use = np - skip;
        if (use > 16384) use = 16384; /* 64 MiB cap */
        r.base = (cm_paddr_t)(hhdm + phys + skip * CM_PAGE_SIZE);
        r.npages = use;
        src = "limine-hhdm";
        kinfo("limine: usable phys=%llx pages=%llu hhdm=%llx → buddy virt=%llx n=%llu",
              (unsigned long)phys, (unsigned long)np, (unsigned long)hhdm,
              (unsigned long)r.base, (unsigned long)r.npages);
    } else {
        kinfo("limine: mmap/hhdm unusable — buddy on %llu-page BSS arena",
              (unsigned long)r.npages);
    }

    cm_limine_fb_snapshot();

    /* paging_init loads CR3 and does not return here. */
    capmix_init(&r, 1);
    kinfo("freestanding: unexpected return from init src=%s", src);
    for (;;) {
#if defined(__x86_64__)
        __asm__ volatile("hlt");
#endif
    }
}

#endif
