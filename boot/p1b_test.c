#include "capmix/kernel.h"
#include "capmix/net.h"
#include "capmix/boot.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include "capmix/virtio_net.h"
#include "capmix/e1000.h"
#include <string.h>
#include "../build/wire_elf.inc"
#define CHECK(x) do{if(!(x)){kerr("p1b line %d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
_Static_assert(sizeof(cm_udp_send_request_t)==1480 && offsetof(cm_udp_send_request_t,data)==8,"UDP_SENDADDR ABI");
static const int ports[2]={9701,9702};
static cm_task_t *client;
static int selected,handle,result,echo_enabled;
static unsigned bytes_checked;
static void wire_client(void *arg)
{
    CM_UNUSED(arg);uint8_t payload[CM_UDP_PAYLOAD];cm_udp_datagram_t got;
    for(unsigned i=0;i<sizeof(payload);i++)payload[i]=(uint8_t)(i*37u+11u);
    const unsigned lengths[]={1,257,1472};
    for(unsigned round=0;round<(echo_enabled?3u:1u);round++) {
        int id=selected?cm_e1000_ifid():cm_virtio_net_ifid();
        uint64_t queued=cm_net_bg_udp_queued(id);
        uint64_t deadline=cm_rdtsc()+cm_tsc_hz()*3;cm_error_t e;
        do {
            e=cm_udp_sendaddr(client,handle,0x0a000202u,9999,payload,(uint16_t)lengths[round]);
            if(e==CM_ERR_AGAIN)cm_sched_yield();
        }while(e==CM_ERR_AGAIN && cm_rdtsc()<deadline);
        if(e){result=e;break;}
        if(!echo_enabled)break;
        /* Prove ingress happened independently before ANY recv request. */
        while(cm_net_bg_udp_queued(id)==queued && cm_rdtsc()<deadline)cm_sched_yield();
        if(cm_net_bg_udp_queued(id)==queued){result=CM_ERR_AGAIN;break;}
        do {e=cm_udp_recv(client,handle,&got);if(e==CM_ERR_AGAIN)cm_sched_yield();}
        while(e==CM_ERR_AGAIN && cm_rdtsc()<deadline);
        if(e || got.len!=lengths[round] || got.saddr!=0x0a000202u || got.sport!=9999 ||
           got.dport!=ports[selected] || memcmp(got.data,payload,got.len)) {result=e?e:CM_ERR_IO;break;}
        bytes_checked+=got.len;
    }
    cm_sched_current()->state=CM_TH_DEAD;
}
int cm_p1b_selftest(void)
{
    int failed=0,usable=0;cm_file_t *file=NULL;cm_id_t pid=0;
    cm_thread_t *sender=NULL;handle=-1;
    if(!cm_virtio_net_available() && !cm_e1000_available()) {
        kinfo("p1b UDP/bg RX skipped: no supported NIC");return 0;
    }
    echo_enabled=cm_boot_has_option("capmix.udp_echo=1");
    CHECK(cm_net_bg_start()==CM_OK);
    client=cm_task_create("udp-wire-test",CM_QOS_USER_INTERACTIVE);CHECK(client);
    for(selected=0;selected<2;selected++) {
        int id=selected?cm_e1000_ifid():cm_virtio_net_ifid();
        if(!(selected?cm_e1000_available():cm_virtio_net_available()))continue;
        cm_iface_t *candidate=cm_if_get(id);
        CHECK(candidate);
        if(!candidate->up) {
            kinfo("p1b UDP/bg RX wire SKIP: if=%d reason=carrier-down; no UDP/IRQ payload claim",id);
            continue;
        }
        usable++;
        for(unsigned step=0;step<128 && !cm_net_bg_is_parked();step++)(void)cm_sched_run_until_idle(1);
        CHECK(cm_net_bg_is_parked());
        uint64_t hw_before=selected?cm_e1000_irq_count():cm_virtio_net_irq_count();
        uint64_t irq_udp_before=cm_net_bg_irq_udp_queued(id);
        cm_iface_t *iface=cm_if_get(id);uint64_t rx_before=cm_net_bg_frames(),tx_before=iface->tx;
        CHECK(cm_udp_open(client,&handle)==CM_OK && cm_udp_bind(client,handle,id,(uint16_t)ports[selected])==CM_OK);
        result=0;bytes_checked=0;
        sender=cm_thread_create(client,wire_client,NULL,CM_QOS_USER_INTERACTIVE);CHECK(sender);
        uint64_t end=cm_rdtsc()+cm_tsc_hz()*12;
        while(sender->id && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
        CHECK(!sender->id && !result);sender=NULL;
        CHECK(cm_net_bg_frames()>rx_before && iface->tx>tx_before);
        if(echo_enabled) {
            CHECK(bytes_checked==1730);
            if(selected?cm_e1000_irq_enabled():cm_virtio_net_irq_enabled()) {
                uint64_t hw_after=selected?cm_e1000_irq_count():cm_virtio_net_irq_count();
                CHECK(hw_after>hw_before && cm_net_bg_irq_udp_queued(id)>irq_udp_before);
                kinfo("irq rx verified: %s slept=1 hardware_delta=%llu irq_udp_queued=%llu payload=1730 queue-before-recv=1",
                    iface->name,(unsigned long)(hw_after-hw_before),(unsigned long)(cm_net_bg_irq_udp_queued(id)-irq_udp_before));
            }
            kinfo("p1b UDP %s queue-before-recv verified: background ingress, no raw RX by client",iface->name);
        }
        kinfo("p1b UDP %s bg RX verified: tx_delta=%llu bg_rx_delta=%llu bytes=%u echo=%s",
              iface->name,(unsigned long)(iface->tx-tx_before),(unsigned long)(cm_net_bg_frames()-rx_before),
              bytes_checked,echo_enabled?"payload verified":"skipped (set capmix.udp_echo=1 + host echo)");
        CHECK(cm_udp_close(client,handle)==CM_OK);handle=-1;
    }
#ifndef CAPMIX_HOSTED
    if(echo_enabled && usable) {
        CHECK(cm_vfs_open("/bin/udpwire.elf",CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&file)==CM_OK);
        CHECK(cm_vfs_write(file,cm_wire_elf,sizeof(cm_wire_elf))==(int64_t)sizeof(cm_wire_elf));
        CHECK(cm_vfs_fsync(file)==CM_OK);cm_vfs_close(file);file=NULL;
        cm_spawn_attr_t attr={.path="/bin/udpwire.elf",.qos=CM_QOS_USER_INTERACTIVE};
        CHECK(cm_posix_spawn(&attr,&pid)==CM_OK);
        cm_proc_t *p=cm_proc_lookup(pid);cm_iface_t *primary=cm_net_primary_iface();
        CHECK(p && p->elf && primary);
        *(uint32_t *)cm_elf_pointer(p->elf,0x401000)=(uint32_t)cm_if_find(primary->name);
        *(uint64_t *)cm_elf_pointer(p->elf,0x401008)=cm_tsc_hz()*3;
        uint64_t end=cm_rdtsc()+cm_tsc_hz()*12;
        while((!p->exited || (p->main && p->main->id)) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
        CHECK(p->exited && p->elf_started && p->exit_code==0);
        CHECK(cm_proc_reap_elf(pid)==CM_OK);pid=0;
    }
#endif
    if(!usable)kinfo("p1b UDP user ELF SKIP: no link-up NIC; hardware wire coverage unavailable");
cleanup:
    if(file)cm_vfs_close(file);
    if(sender && sender->id){sender->state=CM_TH_DEAD;cm_thread_reap(sender);}
    if(handle>=0)(void)cm_udp_close(client,handle);
    if(pid) {
        cm_proc_t *p=cm_proc_lookup(pid);
        if(p && p->main && p->main->id){p->exited=1;p->main->state=CM_TH_DEAD;cm_thread_reap(p->main);}
        (void)cm_proc_reap_elf(pid);
    }
    cm_net_bg_stop();
    kinfo("p1b selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
