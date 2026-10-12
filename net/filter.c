#include "capmix/virtio_net.h"
#include "capmix/e1000.h"
#include "capmix/net.h"
#include "capmix/klog.h"

#include <string.h>

static cm_nft_rule_t g_r[CM_NFT_MAXRULE];
static int           g_nr;

cm_error_t cm_nft_init(void)
{
    memset(g_r, 0, sizeof(g_r));
    g_nr = 0;
    return CM_OK;
}

cm_error_t cm_nft_add(uint8_t proto, uint16_t dport, uint32_t src,
                      int drop, const char *label)
{
    if (g_nr >= CM_NFT_MAXRULE) return CM_ERR_NOSPC;
    cm_nft_rule_t *r = &g_r[g_nr++];
    r->used = 1;
    r->proto = proto;
    r->dport = dport;
    r->src = src;
    r->drop = drop;
    r->label = label;
    kinfo("nft: %s proto=%u dport=%u src=%x → %s",
          label ? label : "rule", proto, dport, src, drop ? "drop" : "accept");
    return CM_OK;
}

int cm_nft_match(const cm_pkt_view_t *v)
{
    if (!v) return -1;
    for (int i = 0; i < g_nr; i++) {
        cm_nft_rule_t *r = &g_r[i];
        if (r->proto && r->proto != v->proto) continue;
        if (r->dport && r->dport != v->dport) continue;
        if (r->src && r->src != v->saddr) continue;
        r->hits++;
        return r->drop ? 1 : 0;
    }
    return -1;
}

extern void cm_net_dump_if(void);
extern cm_error_t cm_ebpf_init(void);
extern cm_error_t cm_net_core_if_init(void);

cm_error_t cm_net_init(void)
{
    cm_net_core_if_init();
    cm_ebpf_init();
    cm_nft_init();
    cm_udp_init();
    (void)cm_virtio_net_init();
    (void)cm_e1000_init();
    cm_iface_t *nic=cm_net_primary_iface();
    kinfo("net primary: %s (priority virtio-net > e1000)",nic?nic->name:"unavailable");
    kinfo("net: in-kernel stack (XNU lesson), XDP + nft");
    return CM_OK;
}

void cm_net_dump(void)
{
    kinfo("net: bpf_enabled=%d rejects=%llu nft_rules=%d",
          cm_bpf_enabled(), (unsigned long)cm_bpf_rejects(), g_nr);
    cm_net_dump_if();
}
