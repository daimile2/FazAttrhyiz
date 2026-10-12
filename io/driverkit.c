#include "capmix/trace.h"
#include "capmix/io.h"
#include "capmix/proc.h"
#include "capmix/mm.h"
#include "capmix/arch.h"
#include "capmix/cap_internal.h"
#include "capmix/klog.h"
#include "capmix/obs.h"
#include <string.h>

extern void cm_blk_set_offline(int off);
static cm_driver_t g_drv[CM_MAX_DRIVERS];
static cm_task_t *g_broker;
static volatile unsigned g_drv_lock;
static uint64_t drv_lock(void)
{
    uint64_t flags = cm_irq_save();
    while (__sync_lock_test_and_set(&g_drv_lock, 1u)) __asm__ volatile("pause" ::: "memory");
    return flags;
}
static void drv_unlock(uint64_t flags)
{ __sync_lock_release(&g_drv_lock); cm_irq_restore(flags); }
static void drv_bottom(int irq, void *arg);
cm_error_t cm_drv_init(void)
{
    memset(g_drv, 0, sizeof(g_drv));
    g_broker = cm_task_create("driver-irq-broker", CM_QOS_UTILITY);
    if (!g_broker) return CM_ERR_NOSPC;
    kinfo("drv: user-mode framework, capability IRQ broker ready");
    return CM_OK;
}
int cm_drv_kernel_alive(void)
{
    void *p = cm_page_alloc(0);
    if (!p) return 0;
    cm_page_free(p, 0);
    return 1;
}
cm_driver_t *cm_drv_get(int id)
{
    if (id < 0 || id >= CM_MAX_DRIVERS || g_drv[id].state == CM_DRV_EMPTY) return NULL;
    return &g_drv[id];
}
static void end_wait_locked(cm_driver_t *d,cm_error_t error,bool wake)
{
    cm_object_t *obj=d->wait_endpoint;
    cm_thread_t *thread=d->wait_thread;
    d->wait_error=error;
    if (!obj) return;
    d->wait_endpoint=NULL; d->wait_thread=NULL;
    cm_ipc_wait_cancel(obj,thread,d->thread_id,wake);
    cm_obj_release(obj);
}
static void offline_locked(cm_driver_t *d)
{
    if (d->state != CM_DRV_ONLINE) return;
    d->state = CM_DRV_OFFLINE;
    end_wait_locked(d,CM_ERR_DEAD,true);
    d->dropped += d->pending; d->pending = 0;
    (void)cm_irq_release(d->irq, drv_bottom, (void *)(uintptr_t)d->id);
    if (d->chan_k > 0) cm_handle_drop(g_broker, d->chan_k);
    d->chan_k = -1;
    cm_blk_set_offline(1);
    kinfo("drv offline: %s pid=%llu device offline, pending IRQs discarded",
          d->name, (unsigned long)d->pid);
}
static int live_receiver_locked(cm_driver_t *d)
{
    cm_proc_t *p = cm_proc_lookup(d->pid);
    if (!p || p->exited || !p->task || !p->main ||
        p->main->id != d->thread_id || p->main->state == CM_TH_DEAD) return 0;
    uint64_t f = cm_cap_lock();
    cm_handle_t *h = cm_handle_lookup(p->task, d->chan_u);
    int live = h && h->obj->id == d->recv_object && (h->rights & CM_RIGHT_READ);
    cm_cap_unlock(f);
    return live;
}
static int deliver_locked(cm_driver_t *d)
{
    if (d->state != CM_DRV_ONLINE) return 0;
    if (!live_receiver_locked(d)) { offline_locked(d); return 0; }
    if (!d->pending) return 0;
    cm_drv_irq_packet_t packet = {(uint32_t)d->irq, (uint32_t)d->id, d->pending_sequence, d->pending};
    cm_ipc_msg_t msg = {0};
    msg.opcode = CM_DRV_IRQ_EVENT; msg.len = sizeof(packet);
    memcpy(msg.data, &packet, sizeof(packet));
    cm_error_t err = cm_ipc_send_deferred(g_broker, d->chan_k, &msg);
    if (err == CM_ERR_AGAIN) { d->backpressure++; return 0; }
    if (err) { offline_locked(d); return 0; }
    CM_TRACE(IRQ_DELIVER,msg.trace,d->irq,d->pid,msg.opcode);
    d->delivered++; d->ios++; d->pending = 0;
    kinfo("drv irq delivered: %s irq=%d pid=%llu seq=%llu count=%llu (channel, deferred)",
          d->name, d->irq, (unsigned long)d->pid,
          (unsigned long)packet.sequence, (unsigned long)packet.count);
    return 1;
}
static void drv_bottom(int irq, void *arg)
{
    uint64_t f = drv_lock();
    cm_driver_t *d = cm_drv_get((int)(uintptr_t)arg);
    if (d && d->irq == irq) {
        d->irqs++;
        if (d->state == CM_DRV_ONLINE) {
            if (!d->pending) d->pending_sequence = d->irqs;
            d->pending++;
            (void)deliver_locked(d);
        } else d->dropped++;
    }
    drv_unlock(f);
}
uint64_t cm_drv_irq_retry(void)
{
    uint64_t f = drv_lock(), sent = 0;
    for (unsigned i = 0; i < CM_MAX_DRIVERS; i++)
        if (g_drv[i].state == CM_DRV_ONLINE) sent += deliver_locked(&g_drv[i]);
    drv_unlock(f);
    return sent;
}
int cm_drv_bind_channel(const char *name, int irq, cm_id_t pid, int send_hid, int recv_hid)
{
    if (!g_broker || irq < 0 || irq >= CM_MAX_IRQ) return CM_ERR_INVAL;
    cm_proc_t *p = cm_proc_lookup(pid);
    if (!p || !p->task || p->exited || !p->user_mode || !p->main || !p->main->id ||
        p->main->state == CM_TH_DEAD) return CM_ERR_DEAD;
    uint64_t f = drv_lock();
    int id = -1;
    for (int i = 0; i < CM_MAX_DRIVERS; i++) {
        if (g_drv[i].state == CM_DRV_ONLINE && (g_drv[i].irq == irq || g_drv[i].pid == pid)) {
            drv_unlock(f); return CM_ERR_BUSY;
        }
        if (id < 0 && g_drv[i].state == CM_DRV_EMPTY) id = i;
    }
    if (id < 0) { drv_unlock(f); return CM_ERR_NOSPC; }
    int kernel_hid;
    cm_id_t recv_object;
    cm_error_t err = cm_ipc_export_sender(p->task, send_hid, recv_hid, g_broker, &kernel_hid, &recv_object);
    if (err) { drv_unlock(f); return err; }
    err = cm_irq_request(irq, drv_bottom, (void *)(uintptr_t)id, name);
    if (err) { cm_handle_drop(g_broker, kernel_hid); drv_unlock(f); return err; }
    cm_driver_t *d = &g_drv[id];
    memset(d, 0, sizeof(*d));
    d->id = id; d->state = CM_DRV_ONLINE; d->irq = irq; d->pid = pid;
    d->chan_k = kernel_hid; d->chan_u = recv_hid; d->recv_object = recv_object;
    d->thread_id = p->main->id;
    const char *n = name ? name : "umd";
    size_t len = 0;
    while (len < sizeof(d->name_storage) - 1 && n[len]) len++;
    memcpy(d->name_storage, n, len); d->name = d->name_storage;
    cm_blk_set_offline(0); /* Explicit replacement registration reattaches the ramdisk. */
    kinfo("drv irq registered: %s id=%d irq=%d pid=%llu send=%d recv=%d",
          d->name, id, irq, (unsigned long)pid, kernel_hid, recv_hid);
    drv_unlock(f);
    return id;
}
int cm_drv_bind(const char *name, int irq, cm_id_t pid)
{
    cm_proc_t *p = cm_proc_lookup(pid);
    if (!p || !p->task || p->exited) return CM_ERR_DEAD;
    int a, b;
    cm_error_t err = cm_channel_create(p->task, &a, &b);
    if (err) return err;
    int id = cm_drv_bind_channel(name, irq, pid, a, b);
    cm_handle_drop(p->task, a);
    if (id < 0) cm_handle_drop(p->task, b);
    return id;
}
int cm_drv_has_receiver(cm_id_t pid)
{
    uint64_t f = drv_lock();
    int found = 0;
    for (unsigned i = 0; i < CM_MAX_DRIVERS; i++)
        if (g_drv[i].pid == pid && g_drv[i].state == CM_DRV_ONLINE) { found = 1; break; }
    drv_unlock(f);
    return found;
}
cm_error_t cm_drv_receive(cm_id_t pid, int recv_hid, cm_ipc_msg_t *out)
{
    if (!out) return CM_ERR_INVAL;
    uint64_t f = drv_lock();
    cm_error_t err = CM_ERR_DENIED;
    for (unsigned i = 0; i < CM_MAX_DRIVERS; i++) {
        cm_driver_t *d = &g_drv[i];
        if (d->pid != pid || d->state == CM_DRV_EMPTY) continue;
        if (d->state != CM_DRV_ONLINE) { err = CM_ERR_DEAD; continue; }
        if (d->chan_u != recv_hid) { err = CM_ERR_DENIED; break; }
        if (!live_receiver_locked(d)) { offline_locked(d); err = CM_ERR_DEAD; break; }
        cm_proc_t *p = cm_proc_lookup(pid);
        err = cm_ipc_try_recv_object(p->task, recv_hid, d->recv_object, out);
        if (!err) {
            d->received++;
            kinfo("drv irq recv: %s pid=%llu irq=%d recv_count=%llu",
                  d->name, (unsigned long)pid, d->irq, (unsigned long)d->received);
        }
        break;
    }
    drv_unlock(f);
    return err;
}
cm_error_t cm_drv_wait_cancel(cm_id_t pid)
{
    uint64_t flags=drv_lock(); cm_error_t result=CM_ERR_AGAIN;
    for (unsigned i=0;i<CM_MAX_DRIVERS;i++) if (g_drv[i].pid==pid && g_drv[i].wait_endpoint) {
        cm_driver_t *d=&g_drv[i]; d->cancels++;
        end_wait_locked(d,CM_ERR_AGAIN,true);
        kinfo("drv wait cancelled: %s pid=%llu",d->name,(unsigned long)pid);
        result=CM_OK; break;
    }
    drv_unlock(flags); return result;
}
cm_error_t cm_drv_receive_wait(cm_id_t pid,int recv_hid,cm_ipc_msg_t *out)
{
    if (!out) return CM_ERR_INVAL;
    uint64_t irq=cm_irq_save(); cm_thread_t *thread=cm_sched_current();
    cm_proc_t *p=cm_proc_lookup(pid);
    if (!thread || !p || p->main!=thread || thread->task!=p->task || cm_cpu_id()!=0) {
        cm_irq_restore(irq); return CM_ERR_DENIED;
    }
    for (;;) {
        uint64_t flags=drv_lock(); cm_driver_t *d=NULL;
        for (unsigned i=0;i<CM_MAX_DRIVERS;i++)
            if (g_drv[i].pid==pid && g_drv[i].state!=CM_DRV_EMPTY && g_drv[i].chan_u==recv_hid) d=&g_drv[i];
        cm_error_t err=CM_ERR_DENIED;
        if (!d) { drv_unlock(flags); cm_irq_restore(irq); return err; }
        if (d->state!=CM_DRV_ONLINE || !live_receiver_locked(d)) {
            offline_locked(d); drv_unlock(flags); cm_irq_restore(irq); return CM_ERR_DEAD;
        }
        err=cm_ipc_try_recv_object(p->task,recv_hid,d->recv_object,out);
        if (err!=CM_ERR_AGAIN) {
            if (!err) { d->received++; kinfo("drv blocking recv: %s recv_count=%llu",d->name,(unsigned long)d->received); }
            drv_unlock(flags); cm_irq_restore(irq); return err;
        }
        bool parked=false; cm_object_t *held=NULL;
        err=cm_ipc_wait_prepare(p->task,recv_hid,d->recv_object,thread,&held,&parked);
        if (err) { drv_unlock(flags); cm_irq_restore(irq); return err; }
        if (!parked) { drv_unlock(flags); continue; }
        d->wait_endpoint=held; d->wait_thread=thread; d->wait_error=CM_OK; d->waits++;
        kinfo("drv wait sleeping: %s pid=%llu thread=%llu state=BLOCKED waits=%llu",d->name,
              (unsigned long)pid,(unsigned long)thread->id,(unsigned long)d->waits);
        drv_unlock(flags); /* irq remains off until the prepared context yields */
        cm_arch_ctx_swap(thread->ctx_slot,-1);
        flags=drv_lock(); d->wakes++;
        err=d->wait_error;
        end_wait_locked(d,err,false);
        kinfo("drv wait awakened: %s pid=%llu wakes=%llu result=%d",d->name,(unsigned long)pid,(unsigned long)d->wakes,err);
        drv_unlock(flags);
        if (err) { cm_irq_restore(irq); return err; }
        /* Retry under the lifecycle lock; wake itself never fabricates a packet. */
    }
}
void cm_drv_process_exit(cm_id_t pid)
{
    uint64_t f = drv_lock();
    int registered = 0;
    for (unsigned i = 0; i < CM_MAX_DRIVERS; i++) {
        if (g_drv[i].state != CM_DRV_EMPTY && g_drv[i].pid == pid) {
            registered = 1; offline_locked(&g_drv[i]);
        }
    }
    if (registered) {
        cm_proc_t *p = cm_proc_lookup(pid);
        if (p && p->task) for (int hid = 1; hid < CM_HANDLES_PER_TASK; hid++)
            if (cm_handle_lookup(p->task, hid)) cm_handle_drop(p->task, hid);
    }
    drv_unlock(f);
}
cm_error_t cm_drv_crash(int id)
{
    uint64_t f = drv_lock();
    cm_driver_t *d = cm_drv_get(id);
    if (!d) { drv_unlock(f); return CM_ERR_INVAL; }
    d->crashes++;
    cm_id_t pid = d->pid;
    cm_proc_t *p = cm_proc_lookup(pid);
    if (p && !p->exited) {
        p->exited = 1; p->exit_code = 11; /* simulated driver crash */
        if (p->main) p->main->state = CM_TH_DEAD;
        if (p->aspace_pages && !p->elf) { cm_commit_uncharge(p->aspace_pages); p->aspace_pages = 0; }
    }
    offline_locked(d); /* DEAD is set before clearing/waking a sleeping receiver. */
    kinfo("drv crash isolated: %s pid=%llu task dead, device offline", d->name, (unsigned long)pid);
    drv_unlock(f);
    cm_drv_process_exit(pid);
    return CM_OK;
}
cm_error_t cm_io_init(void)
{
    cm_irq_init(); cm_uring_init(); cm_blk_init();
    return cm_drv_init();
}
