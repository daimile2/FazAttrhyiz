#include "capmix/disk_layout.h"
#include "capmix/klog.h"
#include <string.h>
static uint32_t rd32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
cm_error_t cm_disk_layout_parse(const void *sector,uint64_t capacity,uint64_t *base)
{
    if(!sector || !base)return CM_ERR_INVAL;
    *base=0;const uint8_t *p=sector;
    if(rd32(p+440)!=CM_INSTALL_DISK_ID)return CM_ERR_NOENT;
    if(p[510]!=0x55 || p[511]!=0xaa || p[444] || p[445] || capacity<CM_INSTALL_MIN_SECT)return CM_ERR_INVAL;
    const uint8_t *root=p+446,*esp=root+16;
    if(root[0] || root[4]!=0xda || rd32(root+8)!=CM_INSTALL_ROOT_LBA || rd32(root+12)!=CM_INSTALL_ROOT_SECT ||
       esp[0]!=0x80 || esp[4]!=0xef || rd32(esp+8)!=CM_INSTALL_ESP_LBA || rd32(esp+12)!=CM_INSTALL_ESP_SECT)return CM_ERR_INVAL;
    for(unsigned i=478;i<510;i++)if(p[i])return CM_ERR_INVAL;
    if((uint64_t)CM_INSTALL_ESP_LBA+CM_INSTALL_ESP_SECT>capacity)return CM_ERR_INVAL;
    *base=CM_INSTALL_ROOT_LBA;return CM_OK;
}
static void wr32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
int cm_disk_layout_selftest(void)
{
    uint8_t mbr[512]={0};uint64_t base=99;int failed=0;
    if(cm_disk_layout_parse(mbr,CM_INSTALL_MIN_SECT,&base)!=CM_ERR_NOENT || base)failed++;
    wr32(mbr+440,CM_INSTALL_DISK_ID);mbr[510]=0x55;mbr[511]=0xaa;
    mbr[450]=0xda;wr32(mbr+454,CM_INSTALL_ROOT_LBA);wr32(mbr+458,CM_INSTALL_ROOT_SECT);
    mbr[462]=0x80;mbr[466]=0xef;wr32(mbr+470,CM_INSTALL_ESP_LBA);wr32(mbr+474,CM_INSTALL_ESP_SECT);
    if(cm_disk_layout_parse(mbr,CM_INSTALL_MIN_SECT,&base) || base!=CM_INSTALL_ROOT_LBA)failed++;
    if(cm_disk_layout_parse(mbr,CM_INSTALL_MIN_SECT-1,&base)!=CM_ERR_INVAL || base)failed++;
    for(unsigned i=0;i<5;i++) {
        unsigned off=(unsigned[]){510,450,454,470,478}[i];mbr[off]^=1;
        if(cm_disk_layout_parse(mbr,CM_INSTALL_MIN_SECT,&base)!=CM_ERR_INVAL || base)failed++;
        mbr[off]^=1;
    }
    kinfo("disk install layout selftest: %s (%d failures) legacy/offset/bounds/overlap/tamper refusal verified",failed?"FAIL":"PASS",failed);
    return failed;
}
