#include "capmix/power.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include <string.h>

/* ACPI 6.6 sections 5/16. Original implementation; no AML interpreter.
 * Only root Name(_S5_, Package(integer, integer, ...)) is supported.
 * Never search byte strings inside methods/buffers for register values. */
static uint16_t pm1a, pm1b, smi;
static uint8_t typea, typeb;
#ifndef CAPMIX_HOSTED
static uint8_t enable;
#endif
static const char *unavailable = "not probed";
#ifndef CAPMIX_HOSTED
static uint32_t load32(const void *p) { uint32_t v; memcpy(&v,p,4); return v; }
extern int cm_limine_ensure_mapped(uint64_t, uint64_t);
static uint64_t load64(const void *p) { uint64_t v; memcpy(&v,p,8); return v; }
static int sum_ok(const uint8_t *p, size_t n)
{ uint8_t s=0; while(n--)s+=*p++; return !s; }
static const uint8_t *physical(uint64_t p,size_t n)
{
    uint64_t h=cm_hhdm();
    if(!p || !h || n>0x100000 || p>UINT64_MAX-h ||
       p+h>UINT64_MAX-4095 || n>UINT64_MAX-(p+h)-4095)return NULL;
    if(cm_limine_ensure_mapped(p+h,n))return NULL;
    return (const uint8_t *)(uintptr_t)(p+h);
}
static const uint8_t *table(uint64_t p)
{
    const uint8_t *t=physical(p,36); if(!t)return NULL;
    uint32_t n=load32(t+4); if(n<36 || n>0x100000)return NULL;
    t=physical(p,n); return t && sum_ok(t,n)?t:NULL;
}
static const uint8_t *fadt(void)
{
    uint64_t p=cm_limine_rsdp(); const uint8_t *r=physical(p,20);
    if(!r || memcmp(r,"RSD PTR ",8) || !sum_ok(r,20))return NULL;
    uint64_t root=load32(r+16); unsigned width=4;
    if(r[15]>=2) {
        r=physical(p,36); if(!r)return NULL;
        uint32_t n=load32(r+20); if(n<36 || n>4096)return NULL;
        r=physical(p,n); if(!r || !sum_ok(r,n))return NULL;
        if(load64(r+24)){root=load64(r+24);width=8;}
    }
    const uint8_t *t=table(root); if(!t)return NULL;
    if(memcmp(t,width==8?"XSDT":"RSDT",4) || (load32(t+4)-36)%width)return NULL;
    for(unsigned i=36;i+width<=load32(t+4);i+=width) {
        const uint8_t *v=table(width==8?load64(t+i):load32(t+i));
        if(v && !memcmp(v,"FACP",4))return v;
    }
    return NULL;
}
static uint16_t in16(uint16_t port)
{ uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(port));return v; }
static void out16(uint16_t port,uint16_t v)
{ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(port):"memory"); }
#endif
/* PkgLength includes its own encoding, but excludes the opcode. */
static int package_end(const uint8_t **p,const uint8_t *end,const uint8_t **stop)
{
    if(*p==end)return 0;
    const uint8_t *start=*p;unsigned lead=*(*p)++, count=lead>>6;
    if((size_t)(end-*p)<count)return 0;
    uint32_t len=count?(lead&15):(lead&63);
    for(unsigned i=0;i<count;i++)len|=(uint32_t)*(*p)++<<(4+8*i);
    if(len<(unsigned)(*p-start) || len>(size_t)(end-start))return 0;
    *stop=start+len;return 1;
}
static int integer(const uint8_t **p,const uint8_t *end,uint32_t *value)
{
    if(*p==end)return 0;
    unsigned op=*(*p)++, n;
    if(op==0 || op==1){*value=op;return 1;}
    if(op==0xff){*value=UINT32_MAX;return 1;}
    n=op==0x0a?1:op==0x0b?2:op==0x0c?4:0;
    if(!n || (size_t)(end-*p)<n)return 0;
    *value=0;for(unsigned i=0;i<n;i++)*value|=(uint32_t)*(*p)++<<(8*i);
    return 1;
}
static int skip_data(const uint8_t **p,const uint8_t *end)
{
    if(*p==end)return 0;
    if(**p==0x11 || **p==0x12 || **p==0x13) {
        (*p)++;const uint8_t *stop;
        if(!package_end(p,end,&stop))return 0;
        *p=stop;return 1;
    }
    if(**p==0x0d){(*p)++;while(*p<end && **p)(*p)++;if(*p==end)return 0;(*p)++;return 1;}
    if(**p==0x0e){if(end-*p<9)return 0;*p+=9;return 1;}
    uint32_t v;return integer(p,end,&v);
}
static int name_skip(const uint8_t **p,const uint8_t *end)
{
    if(*p<end && **p=='\\')(*p)++;
    while(*p<end && **p=='^')(*p)++;
    if(*p==end)return 0;
    unsigned n=1;
    if(**p==0){(*p)++;return 1;}
    if(**p==0x2e){(*p)++;n=2;}
    else if(**p==0x2f){(*p)++;if(*p==end)return 0;n=*(*p)++;}
    if((size_t)(end-*p)<n*4u)return 0;
    *p+=n*4u;return 1;
}
static int s5_walk(const uint8_t *p,size_t length,uint8_t *a,uint8_t *b,unsigned depth)
{
    if(depth>8)return -1;
    const uint8_t *end=p+length;
    while(p<end) {
        unsigned op=*p++;
        if(op==0x08) { /* NameOp at root; only a literal S5 package is accepted. */
            const uint8_t *name=p;
            if(name<end && *name=='\\')name++;
            int match=end-name>=4 && !memcmp(name,"_S5_",4);
            if(!name_skip(&p,end))return -1;
            if(match) {
                if(p==end || *p++!=0x12)return -1;
                const uint8_t *stop;uint32_t va,vb;
                if(!package_end(&p,end,&stop) || p==stop)return -1;
                unsigned count=*p++;
                if(count<2 || count>16 || !integer(&p,stop,&va) ||
                   !integer(&p,stop,&vb) || va>7 || vb>7)return -1;
                for(unsigned i=2;i<count;i++) {uint32_t reserved;if(!integer(&p,stop,&reserved))return -1;}
                if(p!=stop)return -1;
                *a=(uint8_t)va;*b=(uint8_t)vb;return 1;
            }
            if(!skip_data(&p,end))return -1;
        } else if(op==0x10) {
            const uint8_t *stop;if(!package_end(&p,end,&stop))return -1;
            int root=stop-p>=2 && p[0]==0x5c && p[1]==0;
            if(!name_skip(&p,stop))return -1;
            if(root) {int found=s5_walk(p,(size_t)(stop-p),a,b,depth+1);if(found)return found;}
            p=stop;
        } else if(op==0x14 || op==0xa0 || op==0xa1 || op==0xa2 || op==0x5b) {
            /* Scope/Method/If/Else/While and scoped extended declarations. */
            if(op==0x5b) {
                if(p==end)return -1;
                unsigned ext=*p++;
                if(ext==0x80) {
                    uint32_t offset,size;
                    if(!name_skip(&p,end) || p==end)return -1;
                    p++; /* region address space */
                    if(!integer(&p,end,&offset) || !integer(&p,end,&size))return -1;
                    continue;
                }
                if(ext!=0x81 && ext!=0x82 && ext!=0x83 && ext!=0x84 && ext!=0x85 && ext!=0x86)return -1;
            }
            const uint8_t *stop;if(!package_end(&p,end,&stop))return -1;p=stop;
        } else if(op==0x06) {if(!name_skip(&p,end) || !name_skip(&p,end))return -1;}
        else if(op!=0xa3)return -1; /* unknown AML: graceful unsupported */
    }
    return 0;
}
static int s5(const uint8_t *p,size_t length,uint8_t *a,uint8_t *b)
{ return s5_walk(p,length,a,b,0)>0; }

void cm_power_init(void)
{
    pm1a=pm1b=smi=0;
#ifdef CAPMIX_HOSTED
    unavailable="hosted: no firmware I/O";
#else
    unavailable="missing/invalid FADT";
    const uint8_t *f=fadt();if(!f)goto done;
    uint32_t n=load32(f+4);
    if(n<116)goto done;
    unavailable="hardware-reduced ACPI unsupported";
    if(load32(f+112)&(1u<<20))goto done;
    unavailable="unsupported PM1 control register";
    uint64_t pa=load32(f+64),pb=load32(f+68);
    if(f[89]<2)goto done;
    /* Prefer extended GAS if present; support only aligned 16-bit system I/O. */
    for(unsigned i=0;i<2;i++) {
        unsigned off=172+12*i;
        if(n>=off+12 && load64(f+off+4)) {
            if(f[off]!=1 || f[off+1]!=16 || f[off+2]!=0 ||
               (f[off+3]!=0 && f[off+3]!=2))goto done;
            if(i)pb=load64(f+off+4);else pa=load64(f+off+4);
        }
    }
    if(!pa || pa>65534 || pb>65534)goto done;
    uint64_t dp=load32(f+40);if(n>=148 && load64(f+140))dp=load64(f+140);
    const uint8_t *d=table(dp);
    unavailable="missing/invalid DSDT or unsupported root _S5 AML";
    if(!d || memcmp(d,"DSDT",4) || !s5(d+36,load32(d+4)-36,&typea,&typeb))goto done;
    uint32_t sp=load32(f+48);
    if(!(in16((uint16_t)pa)&1) && (!sp || sp>65535 || !f[52])) {
        unavailable="ACPI mode disabled, no supported enable command";goto done;
    }
    pm1a=(uint16_t)pa;pm1b=(uint16_t)pb;smi=sp<=65535?(uint16_t)sp:0;enable=f[52];
    unavailable=NULL;
 done:
#endif
    if(unavailable)kinfo("acpi poweroff skipped: %s; fallback=safe-hlt",unavailable);
    else kinfo("acpi poweroff ready: S5=%u/%u PM1=%x/%x",typea,typeb,pm1a,pm1b);
}
void cm_poweroff(void)
{
#ifdef CAPMIX_HOSTED
    kinfo("poweroff skipped: hosted has no hardware; no halt executed");
#else
    if(pm1a) {
        if(!(in16(pm1a)&1)) {
            __asm__ volatile("outb %0,%1"::"a"(enable),"Nd"(smi):"memory");
            uint64_t until=cm_rdtsc()+cm_tsc_hz();unsigned limit=10000000;
            while(!(in16(pm1a)&1) && limit-- && cm_rdtsc()<until)__asm__ volatile("pause");
        }
        if((in16(pm1a)&1) && (!pm1b || (in16(pm1b)&1))) {
            kinfo("poweroff: ACPI S5 request (root already synced)");
            (void)cm_irq_save();
            uint16_t a=(in16(pm1a)&~(uint16_t)0x3c00) | ((uint16_t)typea<<10) | 0x2000;
            uint16_t b=pm1b?(in16(pm1b)&~(uint16_t)0x3c00) | ((uint16_t)typeb<<10) | 0x2000:0;
            out16(pm1a,a);if(pm1b)out16(pm1b,b);
            uint64_t until=cm_rdtsc()+cm_tsc_hz()/2;unsigned limit=10000000;
            while(limit-- && cm_rdtsc()<until)__asm__ volatile("pause");
            kinfo("poweroff: ACPI request returned without shutdown");
        } else kinfo("poweroff: ACPI enable timed out; fallback=safe-hlt");
    } else kinfo("poweroff: ACPI unavailable (%s)",unavailable);
    kinfo("poweroff: safe-hlt (BSP stopped; AP scheduling disabled)");
    (void)cm_irq_save();for(;;)__asm__ volatile("hlt");
#endif
}
int cm_power_selftest(void)
{
    uint8_t a=0,b=0;
    static const uint8_t good[]={8,'_','S','5','_',0x12,6,2,0x0a,5,0x0a,5};
    static const uint8_t fake[]={8,'B','U','F','0',0x11,15,0x0a,12,8,'_','S','5','_',0x12,6,2,0x0a,5,0x0a,5};
    uint8_t scoped[sizeof(good)+4]={0x10,sizeof(good)+3,0x5c,0};
    uint8_t method[sizeof(good)+7]={0x14,sizeof(good)+6,'M','T','H','0',0};
    memcpy(scoped+4,good,sizeof(good));memcpy(method+7,good,sizeof(good));
    int fail=!s5(good,sizeof(good),&a,&b) || a!=5 || b!=5;
    for(size_t n=0;n<sizeof(good);n++)if(s5(good,n,&a,&b))fail++;
    if(s5(fake,sizeof(fake),&a,&b) || s5(method,sizeof(method),&a,&b) ||
       !s5(scoped,sizeof(scoped),&a,&b))fail++;
    uint8_t malformed[sizeof(good)];memcpy(malformed,good,sizeof(good));malformed[7]=4;
    if(s5(malformed,sizeof(malformed),&a,&b))fail++;
    kinfo("acpi poweroff parser: root-Scope/truncated/element-count/buffer-method fake %s",fail?"FAIL":"verified");
    return fail;
}
