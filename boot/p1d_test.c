#include "capmix/kernel.h"
#include "capmix/tcp.h"
#include "capmix/boot.h"
#include "capmix/virtio_net.h"
#include "capmix/e1000.h"
#include <string.h>
static cm_task_t *client;
static int interface_id,handle,result;
static unsigned completed;
static void echo_client(void *arg)
{
    CM_UNUSED(arg);uint8_t sent[2049],got[2049];int readonly=-1;
    for(unsigned i=0;i<sizeof(sent);i++)sent[i]=(uint8_t)(i*19u+7u);
    result=cm_tcp_open(client,&handle);if(result)goto done;
    result=cm_tcp_connect(client,handle,interface_id,0x0a000202,9998,3000);if(result)goto close;
    if(cm_handle_duplicate(client,handle,client,&readonly,CM_RIGHT_READ) ||
       cm_tcp_send(client,readonly,sent,1,100)!=CM_ERR_DENIED){result=CM_ERR_IO;goto close;}
    (void)cm_handle_drop(client,readonly);readonly=-1;
    if(cm_tcp_send(client,handle,sent,sizeof(sent),3000)!=(int64_t)sizeof(sent)){result=CM_ERR_IO;goto close;}
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*3;unsigned n=0;
    while(n<sizeof(got) && cm_rdtsc()<end) {
        /* Deliberately read small pieces: TCP is a stream, not datagrams. */
        int64_t bytes=cm_tcp_recv(client,handle,got+n,CM_MIN(73u,sizeof(got)-n));
        if(bytes==CM_ERR_AGAIN){cm_sched_yield();continue;}
        if(bytes<=0){result=CM_ERR_IO;goto close;}n+=(unsigned)bytes;
    }
    if(n!=sizeof(got) || memcmp(sent,got,n)){result=CM_ERR_IO;goto close;}
    completed=n;kinfo("tcp payload verified: if=%s bytes=%u MSS=%u partial-read=73",cm_if_get(interface_id)->name,n,CM_TCP_MSS);
close:
    if(readonly>=0)(void)cm_handle_drop(client,readonly);
    {cm_error_t e=cm_tcp_close(client,handle,3000);if(!result)result=e;handle=-1;}
done:
    cm_sched_current()->state=CM_TH_DEAD;
}
static int udp_after_failure(int id,unsigned which)
{
    int hid=-1;cm_udp_datagram_t got;static const char payload[]="UDP after TCP failure";
    int lo=cm_if_find("lo");cm_error_t e=cm_udp_open(client,&hid);
    if(!e)e=cm_udp_bind(client,hid,lo,(uint16_t)(9820+which));
    if(!e)e=cm_udp_sendto(client,hid,(uint16_t)(9820+which),payload,sizeof(payload));
    if(!e)e=cm_udp_recv(client,hid,&got);
    if(!e && (got.len!=sizeof(payload) || memcmp(payload,got.data,sizeof(payload))))e=CM_ERR_IO;
    if(hid>=0)(void)cm_udp_close(client,hid);
    if(e)return 1;
    if(cm_boot_has_option("capmix.udp_echo=1")) {
        hid=-1;e=cm_udp_open(client,&hid);
        if(!e)e=cm_udp_bind(client,hid,id,(uint16_t)(9830+which));
        if(!e && cm_tcp_connect(client,hid,id,0x0a000202,9998,100)!=CM_ERR_INVAL)e=CM_ERR_IO;
        uint64_t end=cm_rdtsc()+cm_tsc_hz()*3;
        if(!e) {
            do {e=cm_udp_sendaddr(client,hid,0x0a000202,9999,payload,sizeof(payload));
                if(e==CM_ERR_AGAIN)(void)cm_sched_run_until_idle(1);
            }while(e==CM_ERR_AGAIN && cm_rdtsc()<end);
        }
        if(!e) {
            do {e=cm_udp_recv(client,hid,&got);if(e==CM_ERR_AGAIN)(void)cm_sched_run_until_idle(1);}
            while(e==CM_ERR_AGAIN && cm_rdtsc()<end);
            if(!e && (got.len!=sizeof(payload) || memcmp(payload,got.data,sizeof(payload))))e=CM_ERR_IO;
        }
        if(hid>=0)(void)cm_udp_close(client,hid);
        if(e)return 1;
        kinfo("tcp failure isolation verified: UDP NIC + lo payload still delivered, typed caps separated");
    } else kinfo("tcp failure isolation verified: UDP lo payload still delivered");
    return 0;
}
int cm_p1d_selftest(void)
{
    int failed=cm_tcp_mechanism_selftest();
    if(!cm_virtio_net_available() && !cm_e1000_available()){kinfo("p1d tcp skipped: no NIC (mechanism failures=%d)",failed);return failed;}
    if(!cm_boot_has_option("capmix.tcp_echo=1")){kinfo("p1d tcp echo skipped: set capmix.tcp_echo=1 + host TCP echo (mechanism verified)");return failed;}
    unsigned before=cm_tcp_live();cm_thread_t *sender=NULL;
    if(cm_net_bg_start()!=CM_OK)return failed+1;
    client=cm_task_create("tcp-client",CM_QOS_USER_INTERACTIVE);
    if(!client){failed++;goto cleanup;}
    for(int which=0;which<2;which++) {
        if(!(which?cm_e1000_available():cm_virtio_net_available()))continue;
        interface_id=which?cm_e1000_ifid():cm_virtio_net_ifid();handle=-1;result=0;completed=0;
        cm_iface_t *iface=cm_if_get(interface_id);uint32_t address=iface->addr,mask=iface->netmask,gateway=iface->gateway;
        sender=cm_thread_create(client,echo_client,NULL,CM_QOS_USER_INTERACTIVE);
        if(!sender){failed++;break;}
        uint64_t end=cm_rdtsc()+cm_tsc_hz()*15;
        while(sender->id && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
        if(sender->id || result || completed!=2049){kerr("p1d TCP echo failed result=%d bytes=%u",result,completed);failed++;break;}
        sender=NULL;
        /* Refused connection must reclaim resources and leave other protocols alive. */
        int bad=-1;cm_error_t e=cm_tcp_open(client,&bad);
        if(!e)e=cm_tcp_connect(client,bad,interface_id,0x0a000202,9997,300);
        if(!e || (e!=CM_ERR_DEAD && e!=CM_ERR_AGAIN)){failed++;}
        if(bad>=0)(void)cm_tcp_close(client,bad,100);
        if(cm_tcp_live()!=before || iface->addr!=address || iface->netmask!=mask || iface->gateway!=gateway)failed++;
        failed+=udp_after_failure(interface_id,(unsigned)which);
        kinfo("tcp failed-connect cleanup verified: error=%d live=%u DHCP address/mask/gateway unchanged",e,cm_tcp_live());
    }
cleanup:
    if(sender && sender->id){sender->state=CM_TH_DEAD;cm_thread_reap(sender);}
    cm_tcp_process_exit(client);cm_net_bg_stop();
    if(cm_tcp_live()!=before)failed++;
    kinfo("p1d selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
