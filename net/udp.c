#include "capmix/net.h"
#include "capmix/arch.h"
#include "capmix/sched.h"
#include "capmix/cap_internal.h"
#include "capmix/klog.h"
#include <string.h>

typedef struct {
    int used, bound, ifid;
    uint16_t port;
    unsigned head, tail, count;
    cm_udp_datagram_t queue[CM_UDP_QUEUE];
} udp_endpoint_t;
static udp_endpoint_t g_udp[CM_UDP_ENDPOINTS];
static volatile unsigned g_udp_lock;
static uint64_t udp_lock(void)
{
    uint64_t flags = cm_irq_save();
    while (__sync_lock_test_and_set(&g_udp_lock, 1u)) __asm__ volatile("pause" ::: "memory");
    return flags;
}
static void udp_unlock(uint64_t flags)
{ __sync_lock_release(&g_udp_lock); cm_irq_restore(flags); }
static void endpoint_destroy(void *payload)
{
    uint64_t f = udp_lock();
    memset(payload, 0, sizeof(udp_endpoint_t)); /* unbind and discard queued datagrams */
    udp_unlock(f);
}
static cm_error_t acquire(cm_task_t *task, int hid, cm_rights_t rights, cm_object_t **out)
{
    cm_error_t err = cm_handle_acquire(task, hid, CM_OBJ_PORT, rights, out);
    if (err) return err;
    if ((*out)->destroy != endpoint_destroy) { cm_obj_release(*out); return CM_ERR_INVAL; }
    return CM_OK;
}
cm_error_t cm_udp_init(void)
{
    memset(g_udp, 0, sizeof(g_udp));
    kinfo("udp: lo datagrams, %u capability endpoints x %u queued, max payload=%u",
          CM_UDP_ENDPOINTS, CM_UDP_QUEUE, CM_UDP_PAYLOAD);
    return CM_OK;
}
cm_error_t cm_udp_open(cm_task_t *task, int *out_hid)
{
    if (!task || !out_hid) return CM_ERR_INVAL;
    uint64_t f = udp_lock();
    udp_endpoint_t *ep = NULL;
    for (unsigned i = 0; i < CM_UDP_ENDPOINTS; i++)
        if (!g_udp[i].used) { ep = &g_udp[i]; memset(ep, 0, sizeof(*ep)); ep->used = 1; break; }
    udp_unlock(f);
    if (!ep) return CM_ERR_NOSPC;
    cm_rights_t rights = CM_RIGHT_READ | CM_RIGHT_WRITE | CM_RIGHT_DUPLICATE | CM_RIGHT_TRANSFER;
    cm_object_t *obj = cm_obj_mint_owned(CM_OBJ_PORT, ep, rights, endpoint_destroy);
    if (!obj) { endpoint_destroy(ep); return CM_ERR_NOSPC; }
    int hid = cm_handle_install(task, obj, rights);
    cm_obj_release(obj);
    if (hid < 0) return hid;
    *out_hid = hid;
    return CM_OK;
}
cm_error_t cm_udp_bind(cm_task_t *task, int hid, int ifid, uint16_t port)
{
    cm_iface_t *iface = cm_if_get(ifid);
    if (!iface || !port) return CM_ERR_INVAL;
    if (iface->addr != CM_UDP_LO_ADDR) return CM_ERR_NOTSUP;
    if (!iface->up) return CM_ERR_DEAD;
    cm_object_t *obj;
    cm_error_t err = acquire(task, hid, CM_RIGHT_WRITE, &obj);
    if (err) return err;
    udp_endpoint_t *ep = obj->payload;
    uint64_t f = udp_lock();
    if (ep->bound) err = CM_ERR_EXIST;
    for (unsigned i = 0; !err && i < CM_UDP_ENDPOINTS; i++)
        if (g_udp[i].bound && g_udp[i].ifid == ifid && g_udp[i].port == port) err = CM_ERR_EXIST;
    if (!err) { ep->bound = 1; ep->ifid = ifid; ep->port = port; }
    udp_unlock(f);
    cm_obj_release(obj);
    return err;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr16(uint8_t *p, uint16_t n) { p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n; }
static void wr32(uint8_t *p, uint32_t n)
{ p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16); p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n; }
static uint32_t sum_bytes(uint32_t sum, const uint8_t *p, unsigned bytes)
{
    while (bytes >= 2) { sum += rd16(p); p += 2; bytes -= 2; }
    if (bytes) sum += (uint32_t)*p << 8;
    return sum;
}
static uint16_t complement(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return (uint16_t)~sum;
}
static uint16_t udp_checksum(const uint8_t *ip, const uint8_t *udp, uint16_t bytes)
{
    uint32_t sum = sum_bytes(0, ip + 12, 8);
    sum += CM_IPPROTO_UDP + bytes;
    return complement(sum_bytes(sum, udp, bytes));
}
cm_error_t cm_udp_frame(uint32_t addr, uint16_t sport, uint16_t dport,
                        const void *data, uint16_t len, uint8_t *out, uint16_t *frame_len)
{
    if (!out || !frame_len || !data || !sport || !dport || !len || len > CM_UDP_PAYLOAD || addr != CM_UDP_LO_ADDR)
        return CM_ERR_INVAL;
    uint16_t n = (uint16_t)(42 + len);
    memset(out, 0, n);
    wr16(out + 12, CM_ETH_IP);
    uint8_t *ip = out + 14, *udp = ip + 20;
    ip[0] = 0x45; ip[8] = 64; ip[9] = CM_IPPROTO_UDP;
    wr16(ip + 2, (uint16_t)(28 + len)); wr16(ip + 6, 0x4000); /* DF, no fragments */
    wr32(ip + 12, addr); wr32(ip + 16, addr);
    wr16(ip + 10, complement(sum_bytes(0, ip, 20)));
    wr16(udp, sport); wr16(udp + 2, dport); wr16(udp + 4, (uint16_t)(8 + len));
    memcpy(udp + 8, data, len);
    uint16_t check = udp_checksum(ip, udp, (uint16_t)(8 + len));
    wr16(udp + 6, check ? check : 65535);
    *frame_len = n;
    return CM_OK;
}
cm_error_t cm_udp_inject(int ifid, const uint8_t *frame, uint16_t len)
{
    cm_iface_t *iface = cm_if_get(ifid);
    if (!iface || !iface->up) return CM_ERR_DEAD;
    if (iface->addr != CM_UDP_LO_ADDR) return CM_ERR_NOTSUP;
    if (!frame || len < 43 || len > CM_UDP_FRAME_MAX) return CM_ERR_INVAL;
    const uint8_t *ip = frame + 14, *udp = ip + 20;
    /* Strict checks apply only here, not to old metadata-only classifier tests. */
    if (rd16(frame + 12) != CM_ETH_IP || ip[0] != 0x45 || !ip[8] || ip[9] != CM_IPPROTO_UDP ||
        rd16(ip + 2) != len - 14 || (rd16(ip + 6) & ~0x4000u) ||
        rd32(ip + 12) != iface->addr || rd32(ip + 16) != iface->addr ||
        complement(sum_bytes(0, ip, 20)) || !rd16(udp) || !rd16(udp + 2) ||
        rd16(udp + 4) != len - 34 || (rd16(udp + 6) && udp_checksum(ip, udp, (uint16_t)(len - 34)))) {
        __sync_fetch_and_add(&iface->rx, 1); __sync_fetch_and_add(&iface->drop, 1); return CM_ERR_INVAL;
    }
    cm_xdp_act_t act = cm_net_rx(ifid, frame, len);
    if (act != CM_XDP_PASS) return act == CM_XDP_DROP ? CM_ERR_DENIED : CM_ERR_NOTSUP;
    cm_udp_datagram_t datagram = {0};
    datagram.saddr = rd32(ip + 12); datagram.daddr = rd32(ip + 16);
    datagram.sport = rd16(udp); datagram.dport = rd16(udp + 2);
    datagram.len = (uint16_t)(len - 42);
    memcpy(datagram.data, udp + 8, datagram.len);
    uint64_t f = udp_lock();
    cm_error_t err = CM_ERR_NOENT;
    for (unsigned i = 0; i < CM_UDP_ENDPOINTS; i++) {
        udp_endpoint_t *ep = &g_udp[i];
        if (!ep->bound || ep->ifid != ifid || ep->port != datagram.dport) continue;
        if (ep->count == CM_UDP_QUEUE) err = CM_ERR_AGAIN;
        else {
            ep->queue[ep->tail] = datagram;
            ep->tail = (ep->tail + 1) % CM_UDP_QUEUE; ep->count++;
            err = CM_OK;
        }
        break;
    }
    udp_unlock(f);
    if (err) __sync_fetch_and_add(&iface->drop, 1);
    return err;
}
cm_error_t cm_udp_sendto(cm_task_t *task, int hid, uint16_t dport, const void *data, uint16_t len)
{
    if (!data || !dport || !len || len > CM_UDP_PAYLOAD) return CM_ERR_INVAL;
    cm_object_t *obj;
    cm_error_t err = acquire(task, hid, CM_RIGHT_WRITE, &obj);
    if (err) return err;
    udp_endpoint_t *ep = obj->payload;
    uint64_t f = udp_lock();
    int ifid = ep->ifid, bound = ep->bound;
    uint16_t sport = ep->port;
    udp_unlock(f);
    cm_iface_t *iface = cm_if_get(ifid);
    if (!bound) err = CM_ERR_INVAL;
    else if (!iface || !iface->up) err = CM_ERR_DEAD;
    else {
        uint8_t frame[CM_UDP_FRAME_MAX]; uint16_t bytes;
        err = cm_udp_frame(iface->addr, sport, dport, data, len, frame, &bytes);
        if (!err) { __sync_fetch_and_add(&iface->tx, 1); err = cm_udp_inject(ifid, frame, bytes); }
    }
    cm_obj_release(obj);
    return err;
}
cm_error_t cm_udp_recv(cm_task_t *task, int hid, cm_udp_datagram_t *out)
{
    if (!out) return CM_ERR_INVAL;
    cm_object_t *obj;
    cm_error_t err = acquire(task, hid, CM_RIGHT_READ, &obj);
    if (err) return err;
    udp_endpoint_t *ep = obj->payload;
    uint64_t f = udp_lock();
    if (!ep->bound) err = CM_ERR_INVAL;
    else if (!ep->count) err = CM_ERR_AGAIN;
    else { *out = ep->queue[ep->head]; ep->head = (ep->head + 1) % CM_UDP_QUEUE; ep->count--; }
    udp_unlock(f);
    cm_obj_release(obj);
    return err;
}
cm_error_t cm_udp_close(cm_task_t *task, int hid)
{
    uint64_t f = cm_cap_lock();
    cm_handle_t *h = cm_handle_lookup(task, hid);
    if (!h || h->obj->type != CM_OBJ_PORT || h->obj->destroy != endpoint_destroy) {
        cm_cap_unlock(f); return CM_ERR_INVAL;
    }
    cm_object_t *obj = h->obj;
    memset(h, 0, sizeof(*h));
    cm_cap_unlock(f);
    cm_obj_release(obj);
    return CM_OK;
}
void cm_udp_process_exit(cm_task_t *task)
{
    if (!task) return;
    /* Atomically detach only typed UDP capabilities. Finalizers run outside
     * cap guard, as they also take the UDP queue lock. */
    for (int hid = 1; hid < CM_HANDLES_PER_TASK; hid++) {
        uint64_t f = cm_cap_lock();
        cm_handle_t *h = cm_handle_lookup(task, hid);
        cm_object_t *obj = NULL;
        if (h && h->obj->type == CM_OBJ_PORT && h->obj->destroy == endpoint_destroy) {
            obj = h->obj; memset(h, 0, sizeof(*h));
        }
        cm_cap_unlock(f);
        cm_obj_release(obj);
    }
}
