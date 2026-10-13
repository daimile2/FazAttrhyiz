#include "capmix/dhcp.h"
#include "capmix/net.h"
#include "capmix/boot.h"
#include "capmix/arch.h"
#include "capmix/sched.h"
#include "capmix/klog.h"
#include <string.h>
/* Original implementation of the bounded RFC2131/2132 bootstrap subset.
 * No renewal, relay, overloaded options, lease persistence or DHCP auth. */
enum { OFF,SELECTING,OFFERED,REQUESTING,BOUND,REJECTED };
struct lease {uint32_t address,mask,gateway,server,seconds;unsigned present;uint8_t type;};
struct client {unsigned state;uint32_t xid;uint8_t mac[6];struct lease lease;int discard;};
static struct client clients[CM_MAX_IFACE];
static unsigned dhcp_lock;
static uint64_t lock(void)
{uint64_t f=cm_irq_save();while(__atomic_exchange_n(&dhcp_lock,1,__ATOMIC_ACQUIRE))__asm__ volatile("pause");return f;}
static void unlock(uint64_t f){__atomic_store_n(&dhcp_lock,0,__ATOMIC_RELEASE);cm_irq_restore(f);}
static uint32_t rd32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void wr32(uint8_t *p,uint32_t x){p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x;}
static int address_ok(uint32_t x){return x && (x>>24)<224 && (x>>24)!=127 && (x>>24)!=0;}
static int mask_ok(uint32_t x){uint32_t inv=~x;return x && !(inv&(inv+1));}
static cm_error_t parse(const uint8_t *p,unsigned length,uint32_t xid,const uint8_t *mac,struct lease *out)
{
    if(!p || length<241 || p[0]!=2 || p[1]!=1 || p[2]!=6 || p[3] ||
       rd32(p+4)!=xid || memcmp(p+28,mac,6) || rd32(p+24) || rd32(p+236)!=0x63825363u)return CM_ERR_INVAL;
    struct lease value={0};value.address=rd32(p+16);int ended=0;
    for(unsigned offset=240;offset<length;) {
        unsigned code=p[offset++];if(!code)continue;if(code==255){ended=1;break;}
        if(offset==length)return CM_ERR_INVAL;
        unsigned n=p[offset++];if(n>length-offset)return CM_ERR_INVAL;
        const uint8_t *v=p+offset;unsigned bit=0;
        switch(code) {
        case 53:if(n!=1)return CM_ERR_INVAL;value.type=v[0];bit=1;break;
        case 54:if(n!=4)return CM_ERR_INVAL;value.server=rd32(v);bit=2;break;
        case 1:if(n!=4)return CM_ERR_INVAL;value.mask=rd32(v);bit=4;break;
        case 3:if(!n || n%4)return CM_ERR_INVAL;value.gateway=rd32(v);bit=8;break;
        case 51:if(n!=4)return CM_ERR_INVAL;value.seconds=rd32(v);bit=16;break;
        case 52:return CM_ERR_NOTSUP;
        default:break;
        }
        if(bit && (value.present&bit))return CM_ERR_INVAL;
        value.present|=bit;offset+=n;
    }
    if(!ended || (value.present&3)!=3 || !address_ok(value.server) ||
       (value.type!=2 && value.type!=5 && value.type!=6))return CM_ERR_INVAL;
    if(value.type!=6 && !address_ok(value.address))return CM_ERR_INVAL;
    if((value.present&4) && !mask_ok(value.mask))return CM_ERR_INVAL;
    if((value.present&8) && !address_ok(value.gateway))return CM_ERR_INVAL;
    if(value.type==5 && ((value.present&20)!=20 || !value.seconds))return CM_ERR_INVAL;
    *out=value;return CM_OK;
}
int cm_dhcp_receiving(int id)
{
    if(id<0 || id>=CM_MAX_IFACE)return 0;
    uint64_t f=lock();unsigned state=clients[id].state;unlock(f);
    return state==SELECTING || state==OFFERED || state==REQUESTING;
}
int cm_dhcp_offered_address(int id,uint32_t address)
{
    if(id<0 || id>=CM_MAX_IFACE || !address)return 0;
    uint64_t f=lock();int match=clients[id].lease.address==address &&
        (clients[id].state==OFFERED || clients[id].state==REQUESTING);unlock(f);return match;
}
cm_error_t cm_dhcp_input(int id,const uint8_t *payload,uint16_t length)
{
    if(id<0 || id>=CM_MAX_IFACE)return CM_ERR_INVAL;
    uint64_t f=lock();struct client *c=&clients[id];struct lease lease;
    cm_error_t e=parse(payload,length,c->xid,c->mac,&lease);
    if(c->discard || e){int dropped=c->discard;unlock(f);return dropped?CM_ERR_AGAIN:e;}
    if(c->state==SELECTING && lease.type==2){c->lease=lease;c->state=OFFERED;}
    else if(c->state==REQUESTING && lease.server==c->lease.server) {
        if(lease.type==6)c->state=REJECTED;
        else if(lease.type==5 && lease.address==c->lease.address){c->lease=lease;c->state=BOUND;}
        else e=CM_ERR_INVAL;
    }else e=CM_ERR_INVAL;
    unlock(f);return e;
}
static unsigned message(uint8_t *p,const struct client *c,int request)
{
    memset(p,0,300);p[0]=1;p[1]=1;p[2]=6;wr32(p+4,c->xid);p[10]=0x80;
    memcpy(p+28,c->mac,6);wr32(p+236,0x63825363u);
    unsigned n=240;p[n++]=53;p[n++]=1;p[n++]=request?3:1;
    p[n++]=61;p[n++]=7;p[n++]=1;memcpy(p+n,c->mac,6);n+=6;
    p[n++]=55;p[n++]=3;p[n++]=1;p[n++]=3;p[n++]=51;
    if(request){p[n++]=50;p[n++]=4;wr32(p+n,c->lease.address);n+=4;
        p[n++]=54;p[n++]=4;wr32(p+n,c->lease.server);n+=4;}
    p[n]=255;return 300; /* BOOTP minimum padded size */
}
static void wait_turn(void)
{if(cm_sched_current())cm_sched_yield();else (void)cm_sched_run_until_idle(1);}
cm_error_t cm_dhcp_configure(int id,unsigned timeout_ms,int discard)
{
    cm_iface_t *iface=cm_if_get(id);
    if(cm_cpu_id()!=0 || !iface || !iface->up || iface->addr==CM_UDP_LO_ADDR || !timeout_ms || timeout_ms>10000)return CM_ERR_INVAL;
    uint32_t old_addr=iface->addr,old_mask=iface->netmask,old_gateway=iface->gateway;
    uint64_t f=lock();struct client *c=&clients[id];
    if(c->state!=OFF){unlock(f);return CM_ERR_BUSY;}
    memset(c,0,sizeof(*c));c->xid=(uint32_t)cm_rdtsc()^(0x434d0000u+(unsigned)id);
    memcpy(c->mac,iface->mac,6);c->state=SELECTING;c->discard=discard;unlock(f);
    uint64_t hz=cm_tsc_hz(),start=cm_rdtsc(),deadline=start+(hz/1000)*timeout_ms,next=start;
    unsigned tries=0;cm_error_t error=CM_ERR_AGAIN;struct client snapshot;
    kinfo("dhcp %s discover: UDP 68->67 broadcast timeout_ms=%u",iface->name,timeout_ms);
    while(cm_rdtsc()<deadline) {
        f=lock();snapshot=*c;
        if(c->state==OFFERED){c->state=REQUESTING;snapshot=*c;next=cm_rdtsc();tries=0;}
        unlock(f);
        if(snapshot.state==BOUND) {error=CM_OK;break;}
        if(snapshot.state==REJECTED){error=CM_ERR_DENIED;break;}
        uint64_t now=cm_rdtsc();
        if(now>=next && tries<3) {
            uint8_t payload[300],frame[342];uint16_t bytes;
            unsigned length=message(payload,&snapshot,snapshot.state==REQUESTING);
            error=cm_udp_frame_to(0,UINT32_MAX,68,67,payload,(uint16_t)length,frame,&bytes);
            if(!error)error=cm_net_dhcp_send(id,frame,bytes);
            if(error)break;
            if(snapshot.state==REQUESTING)kinfo("dhcp %s offer/request: offered=0x%x server=0x%x attempt=%u",iface->name,snapshot.lease.address,snapshot.lease.server,tries+1);
            tries++;next=now+hz;
        }
        error=CM_ERR_AGAIN;wait_turn();
    }
    f=lock();snapshot=*c;
    if(snapshot.state==BOUND)error=CM_OK;
    c->state=OFF;unlock(f);
    if(!error) {
        struct lease *lease=&snapshot.lease;
        error=cm_net_boot_address(id,lease->address,lease->mask,lease->gateway);
        if(!error)kinfo("dhcp %s ack: ip=%u.%u.%u.%u mask=0x%x gateway=0x%x server=0x%x lease=%u seconds renew=unimplemented expire=not-scheduled",
          iface->name,lease->address>>24,(lease->address>>16)&255,(lease->address>>8)&255,lease->address&255,
          lease->mask,lease->gateway,lease->server,lease->seconds);
    }
    if(error)kinfo("dhcp %s fallback: error=%d ip=%u.%u.%u.%u mask=0x%x gateway=0x%x (static unchanged)",
        iface->name,error,old_addr>>24,(old_addr>>16)&255,(old_addr>>8)&255,old_addr&255,old_mask,old_gateway);
    return error;
}
void cm_dhcp_boot_configure(void)
{
    int any=0,enabled=!cm_boot_has_option("capmix.net=static");
    for(int id=0;id<CM_MAX_IFACE;id++) {
        cm_iface_t *iface=cm_if_get(id);if(!iface || !iface->up || iface->addr==CM_UDP_LO_ADDR)continue;
        if(id!=cm_if_find("vnet0") && id!=cm_if_find("enet0"))continue;
        any=1;
        if(!enabled)kinfo("dhcp %s skipped: mode=static ip=0x%x",iface->name,iface->addr);
        else (void)cm_dhcp_configure(id,3000,cm_boot_has_option("capmix.dhcp_drop=1"));
    }
    if(!any)kinfo("dhcp skipped: no up supported NIC (static fallback retained)");
}
int cm_dhcp_codec_selftest(void)
{
    uint8_t p[300],mac[6]={2,0,0,0,0,1};struct client c={.xid=0x12345678};memcpy(c.mac,mac,6);
    message(p,&c,0);p[0]=2;wr32(p+16,0x0a000220);unsigned n=240;
    p[n++]=53;p[n++]=1;p[n++]=5;p[n++]=54;p[n++]=4;wr32(p+n,0x0a000202);n+=4;
    p[n++]=1;p[n++]=4;wr32(p+n,0xffffff00);n+=4;
    p[n++]=51;p[n++]=4;wr32(p+n,3600);n+=4;p[n]=255;
    struct lease value;
    if(parse(p,300,c.xid,mac,&value) || value.address!=0x0a000220 || value.seconds!=3600)return 1;
    if(!parse(p,300,c.xid+1,mac,&value) || !parse(p,239,c.xid,mac,&value))return 1;
    p[28]^=1;if(!parse(p,300,c.xid,mac,&value))return 1;p[28]^=1;
    p[241]=255;if(!parse(p,300,c.xid,mac,&value))return 1;p[241]=1;
    wr32(p+236,0);if(!parse(p,300,c.xid,mac,&value))return 1;
    wr32(p+236,0x63825363u);
    p[251]=0x7f;if(!parse(p,300,c.xid,mac,&value))return 1;p[251]=0xff;
    wr32(p+257,0);if(!parse(p,300,c.xid,mac,&value))return 1;wr32(p+257,3600);
    p[n]=53;p[n+1]=1;p[n+2]=5;p[n+3]=255;
    if(!parse(p,300,c.xid,mac,&value))return 1;
    p[n]=255;
    /* Exercise transitions without changing any NIC config. */
    uint64_t f=lock();clients[0]=c;clients[0].state=SELECTING;unlock(f);
    if(cm_dhcp_input(0,p,300)!=CM_ERR_INVAL)return 1; /* unsolicited ACK */
    p[242]=2;if(cm_dhcp_input(0,p,300))return 1;
    f=lock();int ok=clients[0].state==OFFERED;clients[0].state=REQUESTING;unlock(f);if(!ok)return 1;
    p[242]=5;wr32(p+245,0x0a000203);
    if(cm_dhcp_input(0,p,300)!=CM_ERR_INVAL)return 1;
    wr32(p+245,0x0a000202);if(cm_dhcp_input(0,p,300))return 1;
    f=lock();ok=clients[0].state==BOUND;clients[0].state=REQUESTING;unlock(f);if(!ok)return 1;
    p[242]=6;wr32(p+16,0);if(cm_dhcp_input(0,p,300))return 1;
    f=lock();ok=clients[0].state==REJECTED;memset(&clients[0],0,sizeof(clients[0]));unlock(f);
    if(!ok)return 1;
    kinfo("dhcp codec verified: offer/request/ACK/NAK, lease, xid/MAC/cookie/options/mask/server reject");return 0;
}
