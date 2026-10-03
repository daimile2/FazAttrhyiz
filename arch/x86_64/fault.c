/* Fault reporting + IDT gate installation via sidt.
 *
 * Why sidt and not idt.c's set_gate: cm_isr_stub is installed on the
 * error-code vectors and does not know an error code was pushed, so any
 * #DF/#TS/#NP/#SS/#GP/#PF/#AC turns into a silent jump to garbage. With
 * IST=1 that never triple-faults, so nothing is ever printed and the
 * kernel looks "hung". Re-pointing those gates directly removes the
 * dependency on idt.c (which we have never seen) and makes the very
 * next fault tell us where it died.
 *
 * Reporting goes straight to the UART, not through klog: klog uses a
 * buffer and we may be here because of a fault taken inside klog. */

#include "capmix/arch.h"
#include "capmix/klog.h"

extern char cm_timer_stub[];

extern char cm_fault_stub_8[];
extern char cm_fault_stub_10[];
extern char cm_fault_stub_11[];
extern char cm_fault_stub_12[];
extern char cm_fault_stub_13[];
extern char cm_fault_stub_14[];
extern char cm_fault_stub_17[];

struct cm_idtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

struct cm_gate {
	uint64_t lo;
	uint64_t hi;
};

static void put_str(const char *s)
{
	while (*s)
		cm_serial_putchar(*s++);
}

static void put_hex(uint64_t v)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	cm_serial_putchar('0');
	cm_serial_putchar('x');
	for (i = 60; i >= 0; i -= 4)
		cm_serial_putchar(hex[(v >> i) & 0xfull]);
}

static uint64_t read_cr2(void)
{
	uint64_t v;

	__asm__ volatile("movq %%cr2, %0" : "=r"(v));
	return v;
}

static uint64_t gate_offset(const struct cm_gate *g)
{
	return (g->lo & 0xFFFFull) | ((g->lo >> 48) << 16) | (g->hi << 32);
}

static void write_gate(struct cm_gate *g, uint64_t addr, int ist)
{
	/* [0:15]  offset 15:0
	 * [16:31] selector
	 * [32:39] IST
	 * [40:47] type/attr
	 * [48:63] offset 31:16
	 * [64:95] offset 63:32 */
	g->lo = (addr & 0xFFFFull)
	      | (0x08ull << 16)                /* kernel CS */
	      | ((uint64_t)(ist & 0x7) << 32)
	      | (0x8Eull << 40)                /* P=1 DPL=0 interrupt gate, 64-bit */
	      | ((addr & 0xFFFF0000ull) << 32);
	g->hi = (addr >> 32);
}

void cm_fault_install(void)
{
	struct cm_idtr idtr;
	struct cm_gate *g;
	struct {
		int      vec;
		char    *stub;
	} tab[] = {
		{  8, cm_fault_stub_8  },
		{ 10, cm_fault_stub_10 },
		{ 11, cm_fault_stub_11 },
		{ 12, cm_fault_stub_12 },
		{ 13, cm_fault_stub_13 },
		{ 14, cm_fault_stub_14 },
		{ 17, cm_fault_stub_17 },
	};
	unsigned long i;

	__asm__ volatile("sidt %0" : "=m"(idtr));
	if (idtr.base == 0) {
		kinfo("fault: sidt base=0, IDT not loaded — cannot install");
		return;
	}

	for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
		g = (struct cm_gate *)(idtr.base + (uint64_t)tab[i].vec * 16);
		write_gate(g, (uint64_t)tab[i].stub, 1);
	}
	kinfo("fault: installed error-code stubs for 8,10,11,12,13,14,17 (IST=1)");
}

void cm_fault_dump_gate(uint32_t vec)
{
	struct cm_idtr idtr;
	const struct cm_gate *g;
	uint64_t off;

	__asm__ volatile("sidt %0" : "=m"(idtr));
	if (idtr.base == 0) {
		kinfo("idt[%u]: no IDT", (unsigned)vec);
		return;
	}
	g = (const struct cm_gate *)(idtr.base + (uint64_t)vec * 16);
	off = gate_offset(g);
	kinfo("idt[%u]: off=%llx sel=%x ist=%u type=%x present=%d",
	      (unsigned)vec, (unsigned long)off,
	      (unsigned)((g->lo >> 16) & 0xFFFF),
	      (unsigned)((g->lo >> 32) & 0x7),
	      (unsigned)((g->lo >> 40) & 0xFF),
	      (int)((g->lo >> 47) & 1));
	if (vec == 0x40) {
		kinfo("idt[64]: timer stub is %s (cm_timer_stub=%llx)",
		      (off == (uint64_t)cm_timer_stub) ? "MATCHED" : "MISMATCH",
		      (unsigned long)(uint64_t)cm_timer_stub);
	}
}

void cm_fault_isr(uint64_t vec, uint64_t err, uint64_t rip, uint64_t frame)
{
	put_str("\n[FAULT] vec=");
	put_hex(vec);
	put_str(" err=");
	put_hex(err);
	put_str(" rip=");
	put_hex(rip);
	put_str("\n[FAULT] cs=");
	put_hex(*(uint64_t *)(frame + 16));
	put_str(" rflags=");
	put_hex(*(uint64_t *)(frame + 24));
	put_str(" rsp=");
	put_hex(*(uint64_t *)(frame + 32));
	put_str(" ss=");
	put_hex(*(uint64_t *)(frame + 40));
	put_str("\n[FAULT] cr2=");
	put_hex(read_cr2());
	put_str("  — halting\n");
	for (;;)
		__asm__ volatile("hlt");
}
