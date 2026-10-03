#include "capmix/arch.h"
#include "capmix/klog.h"
#include "capmix/io.h"

#include <string.h>

/* Keyboard IRQ1 -> vector 0x41 (0x40 is the LAPIC timer). */
#define IOAPIC_VEC_KBD 0x41

struct rsdp {
    char     sig[8];
    uint8_t  cksum;
    char     oem[6];
    uint8_t  rev;
    uint32_t rsdt;
    uint32_t len;
    uint64_t xsdt;
    uint8_t  xcksum;
    uint8_t  res[3];
} __attribute__((packed));

struct sdth {
    char     sig[4];
    uint32_t len;
    uint8_t  rev, cksum;
    char     oem[6], oemtab[8];
    uint32_t oemrev;
    uint32_t creator, creatrev;
} __attribute__((packed));

struct madt {
    struct sdth h;
    uint32_t    lapic_addr;
    uint32_t    flags;
} __attribute__((packed));

static uint64_t            g_ioapic_phys;
static volatile uint32_t  *g_ioapic;
static int                 g_irq1_gsi = 1;

/* Limine puts firmware tables (and its own response structs) at addresses
 * our page tables do not cover: the RSDP response was at phys 0x1fab0000,
 * below the kernel image at 0x1fab4000, and the RSDP itself is at 0xf5290.
 * Map before reading, or the walk page-faults and the box dies. */
extern int cm_limine_ensure_mapped(uint64_t virt, uint64_t len);

static uint32_t ioapic_read(int reg)
{
    g_ioapic[0] = (uint32_t)reg;
    return g_ioapic[4];
}

static void ioapic_write(int reg, uint32_t v)
{
    g_ioapic[0] = (uint32_t)reg;
    g_ioapic[4] = v;
}

static volatile uint64_t g_kbd_irqs;

void cm_ioapic_kbd_isr(void)
{
    /* First interrupt that ever arrives from real hardware. Log the first
     * few, then only every 10th, so holding a key down cannot flood the
     * console. */
    uint64_t n = __sync_add_and_fetch(&g_kbd_irqs, 1);
    if (n == 1 || (n % 100) == 0)
        kinfo("kbd: IRQ1 n=%llu", (unsigned long)n);

    cm_irq_inject(1);
    cm_lapic_eoi();
}

int cm_ioapic_init(void)
{
#ifdef CAPMIX_HOSTED
    kwarn("ioapic: skipped on hosted");
    return -1;
#else

    uint64_t rsdp = cm_limine_rsdp();
    if (!rsdp) { kwarn("ioapic: no RSDP"); return -1; }

    uint64_t hhdm = cm_hhdm();
    if (!hhdm) { kwarn("ioapic: no HHDM"); return -1; }

    if (cm_limine_ensure_mapped(rsdp + hhdm, 0x1000) != 0) {
        kwarn("ioapic: cannot map RSDP page");
        return -1;
    }

    struct rsdp *r = (struct rsdp *)(rsdp + hhdm);
    if (memcmp(r->sig, "RSD PTR ", 8) != 0) {
        kwarn("ioapic: bad RSDP signature");
        return -1;
    }

    uint64_t root = (r->rev >= 2 && r->xsdt) ? r->xsdt : r->rsdt;
    if (!root) { kwarn("ioapic: no XSDT/RSDT"); return -1; }

    if (cm_limine_ensure_mapped(root + hhdm, 0x2000) != 0) {
        kwarn("ioapic: cannot map root table");
        return -1;
    }
    struct sdth *xs = (struct sdth *)(root + hhdm);
    int isx = (memcmp(xs->sig, "XSDT", 4) == 0);
    if (!isx && memcmp(xs->sig, "RSDT", 4) != 0) {
        kwarn("ioapic: bad root sig");
        return -1;
    }
    /* Bound the table: a garbage length would otherwise send the scan
     * walking unmapped memory forever. */
    if (xs->len < sizeof(struct sdth) || xs->len > 0x100000) {
        kwarn("ioapic: root len=%u out of range", (unsigned)xs->len);
        return -1;
    }
    uint32_t ent_sz = isx ? 8 : 4;
    uint32_t nent = (xs->len - sizeof(struct sdth)) / ent_sz;
    if (nent > 128) nent = 128;

    uint64_t madt = 0;
    for (uint32_t i = 0; i < nent; i++) {
        uint64_t e;
        if (isx) e = *(uint64_t *)((uint64_t)xs + sizeof(struct sdth) + i * 8);
        else     e = *(uint32_t *)((uint64_t)xs + sizeof(struct sdth) + i * 4);
        if (!e) continue;
        struct sdth *t = (struct sdth *)(e + hhdm);
        if (memcmp(t->sig, "APIC", 4) == 0) { madt = e; break; }
    }
    if (!madt) { kwarn("ioapic: no MADT"); return -1; }

    struct madt *m = (struct madt *)(madt + hhdm);
    if (m->h.len < sizeof(struct madt) || m->h.len > 0x100000) {
        kwarn("ioapic: MADT len=%u out of range", (unsigned)m->h.len);
        return -1;
    }

    uint64_t off = sizeof(struct madt);
    int guard = 0;
    while (off + 2 <= m->h.len && guard++ < 4096) {
        uint8_t *p = (uint8_t *)m + off;
        uint8_t type = p[0];
        uint8_t len  = p[1];
        if (len < 2) break;

        if (type == 1 && len >= 12)
            g_ioapic_phys = *(uint32_t *)(p + 4);
        else if (type == 2 && len >= 10) {
            uint8_t src = p[3];
            uint32_t gsi = *(uint32_t *)(p + 4);
            if (src == 1) g_irq1_gsi = (int)gsi;
        }
        off += len;
    }
    kinfo("ioapic: MADT addr=%llx IRQ1 gsi=%d",
          (unsigned long)g_ioapic_phys, g_irq1_gsi);
    if (!g_ioapic_phys) { kwarn("ioapic: MADT had no IOAPIC entry"); return -1; }

    g_ioapic = (volatile uint32_t *)cm_mmio_map(g_ioapic_phys, 1);
    if (!g_ioapic) { kwarn("ioapic: mmio map failed"); return -1; }
    kinfo("ioapic: version=%x", (unsigned)ioapic_read(1));

    __asm__ volatile("movb $0xff, %%al; outb %%al, $0xa1; outb %%al, $0x21"
                     ::: "rax");

    int gsi = g_irq1_gsi;
    ioapic_write(0x10 + gsi * 2,     (uint32_t)IOAPIC_VEC_KBD);
    ioapic_write(0x10 + gsi * 2 + 1, (uint32_t)cm_lapic_id() << 24);
    kinfo("ioapic: IRQ1 gsi=%d -> vec 0x%x dest=%u",
          gsi, (unsigned)IOAPIC_VEC_KBD, (unsigned)cm_lapic_id());

    extern void cm_irq_stub_kbd(void);
    cm_idt_set_gate(IOAPIC_VEC_KBD, (void *)cm_irq_stub_kbd, 0);
    return 0;
#endif
}