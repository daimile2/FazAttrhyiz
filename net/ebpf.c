#include "capmix/net.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

static cm_bpf_prog_t g_prog[CM_EBPF_MAXPROG];
static int           g_on = 1;
static uint64_t      g_rejects;
static const char   *g_audit[CM_BPF_AUDIT];
static uint32_t      g_naudit;

static void audit(const char *s)
{
    if (g_naudit < CM_BPF_AUDIT) g_audit[g_naudit++] = s;
    kinfo("ebpf.audit: %s", s);
}

static int verify(const cm_bpf_ins_t *ins, uint32_t n)
{
    if (!ins || n == 0 || n > CM_EBPF_MAXINS) {
        audit("reject: empty or too long");
        return 0;
    }
    int has_ret = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (ins[i].op > CM_BPF_MAXOP) {
            audit("reject: unknown opcode");
            return 0;
        }
        if (ins[i].op == CM_BPF_JEQ || ins[i].op == CM_BPF_JNE) {
            int32_t tgt = (int32_t)i + 1 + (int8_t)ins[i].off;
            if (tgt < 0 || (uint32_t)tgt >= n) {
                audit("reject: jump out of bounds");
                return 0;
            }
            /* No backward jumps — keeps the CFG acyclic (verifier core). */
            if (tgt <= (int32_t)i) {
                audit("reject: backward jump");
                return 0;
            }
        }
        if (ins[i].op == CM_BPF_RET) {
            if (ins[i].imm > CM_XDP_REDIRECT) {
                audit("reject: bad xdp action");
                return 0;
            }
            has_ret = 1;
        }
    }
    if (!has_ret) {
        audit("reject: no RET");
        return 0;
    }
    audit("accept: verified");
    return 1;
}

int cm_bpf_load(const char *name, const cm_bpf_ins_t *ins, uint32_t nins)
{
    if (!verify(ins, nins)) {
        g_rejects++;
        return -1;
    }
    int slot = -1;
    for (int i = 0; i < CM_EBPF_MAXPROG; i++)
        if (!g_prog[i].used) { slot = i; break; }
    if (slot < 0) return -1;
    memset(&g_prog[slot], 0, sizeof(g_prog[slot]));
    g_prog[slot].used = 1;
    g_prog[slot].name = name ? name : "prog";
    g_prog[slot].verified = 1;
    g_prog[slot].attached_if = -1;
    g_prog[slot].nins = nins;
    memcpy(g_prog[slot].ins, ins, nins * sizeof(cm_bpf_ins_t));
    kinfo("ebpf: loaded '%s' id=%d ins=%u", g_prog[slot].name, slot, nins);
    return slot;
}

cm_error_t cm_bpf_attach(int prog, int ifid)
{
    if (prog < 0 || prog >= CM_EBPF_MAXPROG || !g_prog[prog].used)
        return CM_ERR_INVAL;
    cm_iface_t *iff = cm_if_get(ifid);
    if (!iff) return CM_ERR_INVAL;
    iff->xdp_prog = prog;
    g_prog[prog].attached_if = ifid;
    kinfo("ebpf: attach '%s' → %s (XDP)", g_prog[prog].name, iff->name);
    return CM_OK;
}

void cm_bpf_disable(void)
{
    g_on = 0;
    audit("kill-switch: eBPF off");
    kwarn("ebpf: DISABLED (conflict #5 switch) — packets skip XDP");
}

void cm_bpf_enable(void)
{
    g_on = 1;
    audit("kill-switch: eBPF on");
}

int      cm_bpf_enabled(void) { return g_on; }
uint64_t cm_bpf_rejects(void) { return g_rejects; }

int cm_xdp_run(int prog, const cm_pkt_view_t *v)
{
    if (!g_on || prog < 0 || !g_prog[prog].used || !v)
        return CM_XDP_PASS;
    cm_bpf_prog_t *p = &g_prog[prog];
    p->run++;
    uint16_t r0 = 0;
    uint32_t pc = 0;
    uint32_t steps = 0;
    while (pc < p->nins && steps++ < CM_EBPF_MAXINS + 1) {
        cm_bpf_ins_t in = p->ins[pc];
        switch (in.op) {
        case CM_BPF_NOP:      pc++; break;
        case CM_BPF_LD_PROTO: r0 = v->proto; pc++; break;
        case CM_BPF_LD_DPORT: r0 = v->dport; pc++; break;
        case CM_BPF_LD_SPORT: r0 = v->sport; pc++; break;
        case CM_BPF_JEQ:
            pc = (r0 == in.imm) ? pc + 1 + in.off : pc + 1;
            break;
        case CM_BPF_JNE:
            pc = (r0 != in.imm) ? pc + 1 + in.off : pc + 1;
            break;
        case CM_BPF_RET:
            if (in.imm == CM_XDP_DROP) p->drop++;
            else p->pass++;
            return (int)in.imm;
        default:
            return CM_XDP_ABORTED;
        }
    }
    return CM_XDP_ABORTED;
}

cm_error_t cm_ebpf_init(void)
{
    memset(g_prog, 0, sizeof(g_prog));
    g_on = 1;
    g_rejects = 0;
    g_naudit = 0;
    kinfo("ebpf: verifier on, kill-switch available, JIT later");
    return CM_OK;
}
