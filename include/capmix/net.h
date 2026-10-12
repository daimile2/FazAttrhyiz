#ifndef CAPMIX_NET_H
#define CAPMIX_NET_H

#include "types.h"
#include "cap.h"

/* Phase 5: in-kernel net (XNU lesson) + nftables expressiveness
 * + eBPF/XDP data plane. Conflict #5: verifier is a CVE source —
 * keep a hard off switch and an audit log.
 */

#define CM_MAX_IFACE     4
#define CM_MAX_PKT       64
#define CM_PKT_MAX       1518
#define CM_EBPF_MAXINS   64
#define CM_EBPF_MAXPROG  4
#define CM_NFT_MAXRULE   16
#define CM_BPF_AUDIT     32

#define CM_ETH_IP        0x0800
#define CM_IPPROTO_TCP   6
#define CM_IPPROTO_UDP   17
#define CM_IPPROTO_ICMP  1

typedef enum {
    CM_XDP_ABORTED = 0,
    CM_XDP_DROP,
    CM_XDP_PASS,
    CM_XDP_TX,
    CM_XDP_REDIRECT,
} cm_xdp_act_t;

/* Tiny verified ISA (restricted C → bytecode stand-in). */
#define CM_BPF_NOP      0
#define CM_BPF_LD_PROTO 1   /* r0 = ip->protocol */
#define CM_BPF_LD_DPORT 2   /* r0 = l4 dport */
#define CM_BPF_LD_SPORT 3
#define CM_BPF_JEQ      4   /* if r0 == imm then pc += off */
#define CM_BPF_JNE      5
#define CM_BPF_RET      6   /* return imm as xdp action */
#define CM_BPF_MAXOP    6

typedef struct {
    uint8_t  op;
    uint8_t  off;
    uint16_t imm;
} cm_bpf_ins_t;

typedef struct {
    int          used;
    const char  *name;
    int          verified;
    int          attached_if;
    uint32_t     nins;
    cm_bpf_ins_t ins[CM_EBPF_MAXINS];
    uint64_t     run;
    uint64_t     drop;
    uint64_t     pass;
} cm_bpf_prog_t;

typedef struct {
    int      used;
    uint8_t  proto;      /* 0 = any */
    uint16_t dport;      /* 0 = any */
    uint32_t src;        /* 0 = any */
    int      drop;       /* 1 drop, 0 accept */
    const char *label;
    uint64_t hits;
} cm_nft_rule_t;

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t etype;
    uint8_t  proto;
    uint16_t sport;
    uint16_t dport;
    uint32_t saddr;
    uint32_t daddr;
    uint16_t len;
} cm_pkt_view_t;

typedef struct {
    int      up;
    const char *name;
    uint32_t addr;
    uint32_t netmask, gateway;
    uint8_t mac[6];
    int      xdp_prog;   /* -1 none */
    uint64_t rx, tx, drop;
} cm_iface_t;

cm_error_t cm_net_init(void);
int        cm_if_add(const char *name, uint32_t addr);
cm_iface_t *cm_if_get(int id);
int cm_if_find(const char *name);
cm_iface_t *cm_net_primary_iface(void);

cm_error_t cm_pkt_parse(const uint8_t *raw, uint16_t len, cm_pkt_view_t *v);
cm_xdp_act_t cm_net_rx(int ifid, const uint8_t *raw, uint16_t len);

int        cm_bpf_load(const char *name, const cm_bpf_ins_t *ins, uint32_t nins);
cm_error_t cm_bpf_attach(int prog, int ifid);
void       cm_bpf_disable(void);     /* conflict #5 kill switch */
void       cm_bpf_enable(void);
int        cm_bpf_enabled(void);
uint64_t   cm_bpf_rejects(void);

cm_error_t cm_nft_add(uint8_t proto, uint16_t dport, uint32_t src,
                      int drop, const char *label);
void       cm_net_dump(void);

/* Phase 5 bounded IPv4 UDP over lo. PORT capabilities carry typed UDP
 * payloads, not POSIX fds. No TCP, routing, fragmentation, or blocking. */
#define CM_UDP_ENDPOINTS 16u
#define CM_UDP_QUEUE 8u
#define CM_UDP_PAYLOAD 256u
#define CM_UDP_LO_ADDR 0x7f000001u
#define CM_UDP_FRAME_MAX (14u + 20u + 8u + CM_UDP_PAYLOAD)
typedef struct {
    uint32_t saddr, daddr;
    uint16_t sport, dport, len, reserved;
    uint8_t data[CM_UDP_PAYLOAD];
} cm_udp_datagram_t;
cm_error_t cm_udp_init(void);
cm_error_t cm_udp_open(cm_task_t *task, int *out_hid);
cm_error_t cm_udp_bind(cm_task_t *task, int hid, int ifid, uint16_t port);
cm_error_t cm_udp_sendto(cm_task_t *task, int hid, uint16_t dport, const void *data, uint16_t len);
cm_error_t cm_udp_recv(cm_task_t *task, int hid, cm_udp_datagram_t *out);
cm_error_t cm_udp_close(cm_task_t *task, int hid);
void cm_udp_process_exit(cm_task_t *task);
/* Strict transport ingress, routed through existing XDP -> nft before queueing. */
cm_error_t cm_udp_inject(int ifid, const uint8_t *frame, uint16_t len);
/* Shared frame encoder for local transmit and malformed-packet tests. */
cm_error_t cm_udp_frame(uint32_t addr, uint16_t sport, uint16_t dport,
                        const void *data, uint16_t len, uint8_t *out, uint16_t *frame_len);

#endif
