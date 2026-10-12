#include "capmix/virtio_net.h"
#include "capmix/net_probe.h"
#include "capmix/net.h"
#include "capmix/klog.h"
#include <string.h>
/* Single static neighbor and bounded ARP/IPv4/ICMP, not a routing stack. */
#define LOCAL_IP 0x0a00020fu
#define PEER_IP  0x0a000202u
#define ECHO_ID 0x434du
#define ECHO_SEQ 1u
static const uint8_t payload[]="CapMix virtio-net off-lo payload!";
struct probe {
    int ifid;const char *label;
    cm_error_t (*send)(const uint8_t *,uint16_t);
    cm_error_t (*recv)(uint8_t *,uint16_t *);
    uint8_t peer_mac[6];int peer_valid,echo_seen;
};
static uint16_t rd16(const uint8_t *p) {return (uint16_t)((p[0]<<8)|p[1]);}
static uint32_t rd32(const uint8_t *p) {return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void wr16(uint8_t *p,uint16_t v) {p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void wr32(uint8_t *p,uint32_t v) {p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static uint16_t checksum(const uint8_t *p,unsigned length)
{
    uint32_t sum=0;
    while(length>=2){sum+=rd16(p);p+=2;length-=2;}
    if(length)sum+=(uint32_t)*p<<8;
    while(sum>>16)sum=(sum&65535)+(sum>>16);
    return (uint16_t)~sum;
}
static void arp(uint8_t *frame,const uint8_t *mac,const uint8_t *target,unsigned op)
{
    memset(frame,0,60);
    if(op==1)memset(frame,255,6);else memcpy(frame,target,6);
    memcpy(frame+6,mac,6);wr16(frame+12,0x806);
    uint8_t *a=frame+14;
    wr16(a,1);wr16(a+2,CM_ETH_IP);a[4]=6;a[5]=4;wr16(a+6,(uint16_t)op);
    memcpy(a+8,mac,6);wr32(a+14,LOCAL_IP);
    if(op==2)memcpy(a+18,target,6);
    wr32(a+24,PEER_IP);
}
static uint16_t echo(uint8_t *frame,const uint8_t *mac,const uint8_t *dst)
{
    unsigned n=42+sizeof(payload)-1;
    memset(frame,0,n);memcpy(frame,dst,6);memcpy(frame+6,mac,6);wr16(frame+12,CM_ETH_IP);
    uint8_t *ip=frame+14,*icmp=ip+20;
    ip[0]=0x45;wr16(ip+2,(uint16_t)(n-14));wr16(ip+4,ECHO_SEQ);wr16(ip+6,0x4000);
    ip[8]=64;ip[9]=CM_IPPROTO_ICMP;wr32(ip+12,LOCAL_IP);wr32(ip+16,PEER_IP);
    wr16(ip+10,checksum(ip,20));
    icmp[0]=8;wr16(icmp+4,ECHO_ID);wr16(icmp+6,ECHO_SEQ);
    memcpy(icmp+8,payload,sizeof(payload)-1);wr16(icmp+2,checksum(icmp,n-34));
    return (uint16_t)n;
}
static int valid_ip(const uint8_t *frame,uint16_t len,uint32_t source,uint32_t dest)
{
    if(len<42 || rd16(frame+12)!=CM_ETH_IP)return 0;
    const uint8_t *ip=frame+14;
    unsigned total=rd16(ip+2);
    return ip[0]==0x45 && ip[8] && ip[9]==CM_IPPROTO_ICMP && total>=28 && total<=len-14u &&
           !(rd16(ip+6)&~0x4000u) && rd32(ip+12)==source && rd32(ip+16)==dest &&
           !checksum(ip,20) && !checksum(ip+20,total-20);
}
static int valid_reply(const uint8_t *frame,uint16_t len)
{
    if(!valid_ip(frame,len,PEER_IP,LOCAL_IP))return 0;
    const uint8_t *icmp=frame+34;
    return rd16(frame+16)==28+sizeof(payload)-1 && icmp[0]==0 && !icmp[1] &&
           rd16(icmp+4)==ECHO_ID && rd16(icmp+6)==ECHO_SEQ &&
           !memcmp(icmp+8,payload,sizeof(payload)-1);
}
static cm_error_t ingress(struct probe *p,uint8_t *frame,uint16_t len)
{
    int id=p->ifid;cm_iface_t *iface=cm_if_get(id);
    if(!iface || len<14)return CM_ERR_INVAL;
    if(cm_net_rx(id,frame,len)!=CM_XDP_PASS)return CM_ERR_DENIED;
    if(rd16(frame+12)==0x806) {
        if(len<42)return CM_ERR_INVAL;
        const uint8_t *a=frame+14;
        unsigned op=rd16(a+6),nonzero=0;
        for(unsigned i=0;i<6;i++)nonzero|=a[8+i];
        if(rd16(a)!=1 || rd16(a+2)!=CM_ETH_IP || a[4]!=6 || a[5]!=4 ||
           (op!=1 && op!=2) || !nonzero || (a[8]&1) || memcmp(frame+6,a+8,6) ||
           rd32(a+14)!=iface->gateway || rd32(a+24)!=iface->addr)return CM_ERR_INVAL;
        if(op==2 && (memcmp(a+18,iface->mac,6) || memcmp(frame,iface->mac,6)))return CM_ERR_INVAL;
        if(op==1) {
            static const uint8_t broadcast[6]={255,255,255,255,255,255};
            if(memcmp(frame,broadcast,6) && memcmp(frame,iface->mac,6))return CM_ERR_INVAL;
            uint8_t reply[60];arp(reply,iface->mac,a+8,2);
            cm_error_t err=p->send(reply,sizeof(reply));if(err)return err;
        }
        memcpy(p->peer_mac,a+8,6);p->peer_valid=1;return CM_OK;
    }
    if(rd16(frame+12)!=CM_ETH_IP)return CM_ERR_NOTSUP;
    if(!p->peer_valid || memcmp(frame,iface->mac,6) || memcmp(frame+6,p->peer_mac,6) ||
       !valid_ip(frame,len,iface->gateway,iface->addr))return CM_ERR_INVAL;
    if(valid_reply(frame,len)) {p->echo_seen=1;return CM_OK;}
    if(frame[34]==8 && !frame[35]) {
        unsigned n=rd16(frame+16)+14;
        uint8_t reply[CM_PKT_MAX];memcpy(reply,frame,n);
        memcpy(reply,p->peer_mac,6);memcpy(reply+6,iface->mac,6);
        wr32(reply+26,iface->addr);wr32(reply+30,iface->gateway);
        wr16(reply+24,0);wr16(reply+24,checksum(reply+14,20));
        reply[34]=0;wr16(reply+36,0);wr16(reply+36,checksum(reply+34,n-34));
        return p->send(reply,(uint16_t)n);
    }
    return CM_ERR_NOTSUP;
}
static cm_error_t wait_for(struct probe *p,int *condition)
{
    uint8_t frame[CM_PKT_MAX];uint16_t length=0;
    for(unsigned i=0;i<2000000;i++) {
        if(*condition)return CM_OK;
        cm_error_t e=p->recv(frame,&length);
        if(e==CM_ERR_AGAIN) {__asm__ volatile("pause");continue;}
        if(e)return e;
        e=ingress(p,frame,length);
        if(e==CM_ERR_DEAD || e==CM_ERR_IO)return e;
        if(e) __sync_fetch_and_add(&cm_if_get(p->ifid)->drop,1);
    }
    return *condition?CM_OK:CM_ERR_AGAIN;
}
cm_error_t cm_net_probe(int ifid,const char *label,
    cm_error_t (*send)(const uint8_t *,uint16_t),cm_error_t (*recv)(uint8_t *,uint16_t *))
{
    if(!label || !send || !recv)return CM_ERR_INVAL;
    struct probe state={.ifid=ifid,.label=label,.send=send,.recv=recv};
    struct probe *p=&state;
    cm_iface_t *iface=cm_if_get(p->ifid);
    if(!iface || !iface->up)return CM_ERR_DEAD;
    uint64_t tx=iface->tx,rx=iface->rx;
    uint8_t request[CM_PKT_MAX];p->peer_valid=p->echo_seen=0;
    cm_error_t e=CM_ERR_AGAIN;
    for(unsigned attempt=0;attempt<3 && !p->peer_valid;attempt++) {
        arp(request,iface->mac,NULL,1);e=p->send(request,60);
        if(e)return e;
        e=wait_for(p,&p->peer_valid);
        if(e!=CM_OK && e!=CM_ERR_AGAIN)return e;
    }
    if(!p->peer_valid){kerr("%s ARP timeout: peer 10.0.2.2",p->label);return e;}
    kinfo("%s ARP rx: 10.0.2.2 neighbor verified",p->label);
    for(unsigned attempt=0;attempt<3 && !p->echo_seen;attempt++) {
        uint16_t n=echo(request,iface->mac,p->peer_mac);
        e=p->send(request,n);if(e)return e;
        e=wait_for(p,&p->echo_seen);
        if(e!=CM_OK && e!=CM_ERR_AGAIN)return e;
    }
    if(!p->echo_seen || iface->tx<=tx || iface->rx<=rx)return CM_ERR_IO;
    kinfo("%s tx/rx payload verified: ip=10.0.2.15 peer=10.0.2.2 ICMP id=%x seq=%u bytes=%u tx_delta=%llu rx_delta=%llu (off-lo)",
          p->label,ECHO_ID,ECHO_SEQ,(unsigned)sizeof(payload)-1,(unsigned long)(iface->tx-tx),(unsigned long)(iface->rx-rx));
    return CM_OK;
}
cm_error_t cm_vnet_probe(void)
{ return cm_net_probe(cm_virtio_net_ifid(),"virtio-net",cm_virtio_net_transmit,cm_virtio_net_receive); }

int cm_vnet_codec_selftest(void)
{
    uint8_t mac[6]={2,0,0,0,0,1},dst[6]={2,0,0,0,0,2},frame[CM_PKT_MAX];
    uint16_t n=echo(frame,mac,dst);
    if(!valid_ip(frame,n,LOCAL_IP,PEER_IP) || valid_ip(frame,41,LOCAL_IP,PEER_IP))return 1;
    wr32(frame+26,PEER_IP);wr32(frame+30,LOCAL_IP);
    wr16(frame+24,0);wr16(frame+24,checksum(frame+14,20));frame[34]=0;
    wr16(frame+36,0);wr16(frame+36,checksum(frame+34,n-34));
    if(!valid_reply(frame,n))return 1;
    frame[n-1]^=1;
    if(valid_reply(frame,n))return 1; /* corrupted checksum */
    wr16(frame+36,0);wr16(frame+36,checksum(frame+34,n-34));
    if(valid_reply(frame,n))return 1; /* checksummed but wrong payload */
    frame[n-1]^=1;wr16(frame+36,0);wr16(frame+36,checksum(frame+34,n-34));
    if(!valid_reply(frame,n))return 1;
    frame[20]|=0x20;wr16(frame+24,0);wr16(frame+24,checksum(frame+14,20));
    if(valid_reply(frame,n))return 1; /* checksum valid but fragmented */
    arp(frame,mac,NULL,1);
    if(rd16(frame+12)!=0x806 || rd32(frame+28)!=LOCAL_IP || rd32(frame+38)!=PEER_IP)return 1;
    kinfo("m4 codec verified: ARP/IPv4/ICMP checksum, odd payload, truncated/corrupt/fragment reject");
    return 0;
}
