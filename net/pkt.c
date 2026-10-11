#include "capmix/net.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>

extern int  cm_xdp_run(int prog, const cm_pkt_view_t *v);
extern int  cm_nft_match(const cm_pkt_view_t *v); /* 1=drop, 0=pass, -1=no match */
extern int  cm_bpf_enabled(void);

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

cm_error_t cm_pkt_parse(const uint8_t *raw, uint16_t len, cm_pkt_view_t *v)
{
    if (!raw || !v || len < 14) return CM_ERR_INVAL;
    memset(v, 0, sizeof(*v));
    memcpy(v->dst, raw, 6);
    memcpy(v->src, raw + 6, 6);
    v->etype = rd16(raw + 12);
    v->len = len;
    if (v->etype != CM_ETH_IP || len < 34) return CM_OK;
    const uint8_t *ip = raw + 14;
    uint8_t ihl = (uint8_t)((ip[0] & 0x0f) * 4);
    if (ihl < 20) return CM_OK;
    v->proto = ip[9];
    v->saddr = rd32(ip + 12);
    v->daddr = rd32(ip + 16);
    if (len >= 14 + ihl + 4 && (v->proto == CM_IPPROTO_TCP || v->proto == CM_IPPROTO_UDP)) {
        const uint8_t *l4 = ip + ihl;
        v->sport = rd16(l4);
        v->dport = rd16(l4 + 2);
    }
    return CM_OK;
}

cm_xdp_act_t cm_net_rx(int ifid, const uint8_t *raw, uint16_t len)
{
    cm_iface_t *iff = cm_if_get(ifid);
    if (!iff || !iff->up) return CM_XDP_ABORTED;
    __sync_fetch_and_add(&iff->rx, 1);

    cm_pkt_view_t v;
    if (cm_pkt_parse(raw, len, &v) != CM_OK) {
        __sync_fetch_and_add(&iff->drop, 1);
        return CM_XDP_DROP;
    }

    /* XDP first — programmable data plane, before the stack. */
    if (cm_bpf_enabled() && iff->xdp_prog >= 0) {
        int act = cm_xdp_run(iff->xdp_prog, &v);
        if (act == CM_XDP_DROP || act == CM_XDP_ABORTED) {
            __sync_fetch_and_add(&iff->drop, 1);
            return (cm_xdp_act_t)act;
        }
        if (act == CM_XDP_TX) {
            __sync_fetch_and_add(&iff->tx, 1);
            return CM_XDP_TX;
        }
    }

    /* nftables-style slow path. */
    int n = cm_nft_match(&v);
    if (n == 1) {
        __sync_fetch_and_add(&iff->drop, 1);
        return CM_XDP_DROP;
    }
    /* PASS classifies ingress; transmit is counted only by real TX/XDP_TX. */
    cm_obs_emit(cm_trace_new(), CM_EV_ALLOC, v.proto, v.dport, v.saddr);
    return CM_XDP_PASS;
}
