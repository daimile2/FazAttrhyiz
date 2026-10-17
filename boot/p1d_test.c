#include "capmix/kernel.h"
#include "capmix/tcp.h"
#include "capmix/boot.h"
#include "capmix/virtio_net.h"
#include "capmix/e1000.h"
#include <string.h>
static cm_task_t *client;
static int interface_id,handle,result;
static unsigned completed;
static unsigned drop_test;
static void echo_client(void *arg)
{
    CM_UNUSED(arg);uint8_t sent[2049],got[2049];int readonly=-1;
    for(unsigned i=0;i<sizeof(sent);i++)sent[i]=(uint8_t)(i*19u+7u);
    result=cm_tcp_open(client,&handle);if(result)goto done;
    if(drop_test){result=cm_tcp_test_drop(client,handle,7);if(result)goto close;}
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
#ifndef CAPMIX_HOSTED
#define MULTI_CLIENTS 8u
#define MULTI_BYTES 8193u
struct multi_client {cm_task_t *task;int iface,hid,error;unsigned index,bytes;uint8_t sent[MULTI_BYTES];};
static struct multi_client multi[MULTI_CLIENTS];
static unsigned multi_connected,multi_verified,multi_abort,multi_peak,multi_running;
static uint64_t multi_end;
static void multi_echo(void *arg)
{
    struct multi_client *c=arg;uint8_t got[333];
    c->error=cm_tcp_open(c->task,&c->hid);if(c->error)goto finish;
    c->error=cm_tcp_connect(c->task,c->hid,c->iface,0x0a000202,9998,3000);if(c->error)goto close;
    __atomic_add_fetch(&multi_connected,1,__ATOMIC_ACQ_REL);
    while(__atomic_load_n(&multi_connected,__ATOMIC_ACQUIRE)!=MULTI_CLIENTS && !__atomic_load_n(&multi_abort,__ATOMIC_ACQUIRE) && cm_rdtsc()<multi_end)cm_sched_yield();
    if(__atomic_load_n(&multi_connected,__ATOMIC_ACQUIRE)!=MULTI_CLIENTS || __atomic_load_n(&multi_abort,__ATOMIC_ACQUIRE)){c->error=CM_ERR_AGAIN;goto close;}
    __atomic_store_n(&multi_peak,MULTI_CLIENTS,__ATOMIC_RELEASE); /* all eight completed handshake and are held open */
    for(unsigned i=0;i<MULTI_BYTES;i++)c->sent[i]=(uint8_t)(i*13u+c->index*29u);
    if(cm_tcp_send(c->task,c->hid,c->sent,MULTI_BYTES,3000)!=MULTI_BYTES){c->error=CM_ERR_IO;goto close;}
    while(c->bytes<MULTI_BYTES && cm_rdtsc()<multi_end) {
        unsigned n=CM_MIN((unsigned)sizeof(got),MULTI_BYTES-c->bytes);
        int64_t r=cm_tcp_recv(c->task,c->hid,got,n);
        if(r==CM_ERR_AGAIN){cm_sched_yield();continue;}
        if(r<=0 || memcmp(got,c->sent+c->bytes,(size_t)r)){c->error=CM_ERR_IO;goto close;}
        c->bytes+=(unsigned)r;
    }
    if(c->bytes!=MULTI_BYTES){c->error=CM_ERR_AGAIN;goto close;}
    __atomic_add_fetch(&multi_verified,1,__ATOMIC_ACQ_REL);
    while(__atomic_load_n(&multi_verified,__ATOMIC_ACQUIRE)!=MULTI_CLIENTS && !__atomic_load_n(&multi_abort,__ATOMIC_ACQUIRE) && cm_rdtsc()<multi_end)cm_sched_yield();
close:
    if(c->error)__atomic_store_n(&multi_abort,1,__ATOMIC_RELEASE);
    if(c->hid>=0){int e=cm_tcp_close(c->task,c->hid,1000);if(!c->error)c->error=e;c->hid=-1;}
finish:
    if(c->error)__atomic_store_n(&multi_abort,1,__ATOMIC_RELEASE);
    cm_sched_current()->state=CM_TH_DEAD;
}
static int multi_selftest(int id)
{
    if(__atomic_exchange_n(&multi_running,1,__ATOMIC_ACQ_REL)){kerr("tcp multi retained context: retry refused");return 1;}
    int failed=0;unsigned before=cm_tcp_live();uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),charge,limit,after;
    cm_commit_stats(&charge,&limit);cm_thread_t *threads[MULTI_CLIENTS]={0};
    cm_task_t *task=cm_task_create("tcp-multi8",CM_QOS_USER_INTERACTIVE);if(!task){__atomic_store_n(&multi_running,0,__ATOMIC_RELEASE);return 1;}
    memset(multi,0,sizeof(multi));multi_connected=multi_verified=multi_abort=multi_peak=0;multi_end=cm_rdtsc()+cm_tsc_hz()*8;
    for(unsigned i=0;i<MULTI_CLIENTS;i++) {
        multi[i].task=task;multi[i].iface=id;multi[i].index=i;multi[i].hid=-1;
        threads[i]=cm_thread_create(task,multi_echo,&multi[i],CM_QOS_USER_INTERACTIVE);
        if(!threads[i]){__atomic_store_n(&multi_abort,1,__ATOMIC_RELEASE);failed=1;break;}
    }
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*12;unsigned active;
    do {active=0;for(unsigned i=0;i<MULTI_CLIENTS;i++)active+=threads[i] && threads[i]->id;
        if(active)(void)cm_sched_run_until_idle(1);
    }while(active && cm_rdtsc()<end);
    if(active){kerr("tcp multi timeout: worker context retained safely");return 1;}
    for(unsigned i=0;i<MULTI_CLIENTS;i++)if(!threads[i] || multi[i].error || multi[i].bytes!=MULTI_BYTES){
        kerr("tcp multi client=%u error=%d bytes=%u",i,multi[i].error,multi[i].bytes);failed=1;}
    if(multi_peak!=MULTI_CLIENTS || multi_verified!=MULTI_CLIENTS)failed=1;
    cm_tcp_process_exit(task);cm_obj_release(task->self_obj);memset(task,0,sizeof(*task));
    end=cm_rdtsc()+cm_tsc_hz();while(cm_tcp_live()!=before && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
    cm_commit_stats(&after,&limit);
    if(cm_tcp_live()!=before || charge!=after || pages!=cm_mm_free_pages()+cm_file_cache_pages()){kerr("tcp multi memory/commit/slot reclaim mismatch");failed=1;}
    __atomic_store_n(&multi_running,0,__ATOMIC_RELEASE);
    if(!failed)kinfo("tcp multi verified: if=%s simultaneous=%u clients=%u bytes_each=%u payload isolated close/reclaim=ok",cm_if_get(id)->name,multi_peak,MULTI_CLIENTS,MULTI_BYTES);
    return failed;
}
#endif
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
        drop_test=cm_boot_has_option("capmix.tcp_drop=1");
        uint64_t retry_before=cm_tcp_retransmits();
        sender=cm_thread_create(client,echo_client,NULL,CM_QOS_USER_INTERACTIVE);
        if(!sender){failed++;break;}
        uint64_t end=cm_rdtsc()+cm_tsc_hz()*15;
        while(sender->id && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
        if(sender->id || result || completed!=2049){kerr("p1d TCP echo failed result=%d bytes=%u",result,completed);failed++;break;}
        sender=NULL;
        if(drop_test){uint64_t retries=cm_tcp_retransmits()-retry_before;
            if(retries<3)failed++;
            kinfo("tcp rto loss recovery: dropped=SYN/data/FIN retransmits=%llu payload=2049 close=ok live=%u",(unsigned long)retries,cm_tcp_live());}
        /* Refused connection must reclaim resources and leave other protocols alive. */
        int bad=-1;cm_error_t e=cm_tcp_open(client,&bad);
        if(!e)e=cm_tcp_connect(client,bad,interface_id,0x0a000202,9997,300);
        if(!e || (e!=CM_ERR_DEAD && e!=CM_ERR_AGAIN)){failed++;}
        if(bad>=0)(void)cm_tcp_close(client,bad,100);
        /* A concurrent RX/RTO worker can still hold a transient slot pin
         * after the last capability closes. Let that continuation finish. */
        uint64_t quiesce=cm_rdtsc()+cm_tsc_hz();
        while(cm_tcp_live()!=before && cm_rdtsc()<quiesce)(void)cm_sched_run_until_idle(1);
        if(cm_tcp_live()!=before || iface->addr!=address || iface->netmask!=mask || iface->gateway!=gateway) {
            kerr("tcp failure cleanup mismatch: live=%u/%u address=%x/%x mask=%x/%x gateway=%x/%x",cm_tcp_live(),before,
                iface->addr,address,iface->netmask,mask,iface->gateway,gateway);failed++;
        }
#ifndef CAPMIX_HOSTED
        failed+=multi_selftest(interface_id);
#endif
        failed+=udp_after_failure(interface_id,(unsigned)which);
        kinfo("tcp failed-connect cleanup verified: error=%d live=%u DHCP address/mask/gateway unchanged",e,cm_tcp_live());
    }
cleanup:
    if(sender && sender->id){sender->state=CM_TH_DEAD;cm_thread_reap(sender);}
    cm_tcp_process_exit(client);cm_net_bg_stop();
    if(cm_tcp_live()!=before)failed++;
    kinfo("p1d selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
