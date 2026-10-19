#include "capmix/console.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
static uint8_t queue[128];
static unsigned head,tail;
static unsigned shift,caps,caps_down,extended;
#ifndef CAPMIX_HOSTED
static int ready;
#endif
static uint64_t received,dropped,echoed;
static const char plain[58] = {
 [2]='1',[3]='2',[4]='3',[5]='4',[6]='5',[7]='6',[8]='7',[9]='8',[10]='9',[11]='0',
 [12]='-',[13]='=',[14]='\b',[15]='\t',[16]='q',[17]='w',[18]='e',[19]='r',[20]='t',[21]='y',[22]='u',[23]='i',[24]='o',[25]='p',
 [26]='[',[27]=']',[28]='\n',[30]='a',[31]='s',[32]='d',[33]='f',[34]='g',[35]='h',[36]='j',[37]='k',[38]='l',
 [39]=';',[40]='\'',[41]='`',[43]='\\',[44]='z',[45]='x',[46]='c',[47]='v',[48]='b',[49]='n',[50]='m',[51]=',',[52]='.',[53]='/',[57]=' '
};
static char decode(uint8_t code)
{
    if(code==0xe0 || code==0xe1) { extended=1; return 0; }
    if(extended) { extended=0; return 0; }
    unsigned key=code&127, released=code&128;
    if(key==42 || key==54) { unsigned bit=key==42?1:2; if(released)shift&=~bit;else shift|=bit;return 0; }
    /* Toggle once per physical press; typematic make repeats must not flicker. */
    if(key==58) {
        if(released)caps_down=0;
        else if(!caps_down){caps^=1;caps_down=1;}
        return 0;
    }
    if(released) return 0;
    if(key>=sizeof(plain))return 0;
    char c=plain[key];
    if(c>='a' && c<='z') { if((shift!=0)^caps)c-=32; }
    else if(shift) {
        const char *a="1234567890-=[];'`,./\\",*b="!@#$%^&*()_+{}:\"~<>?|";
        for(unsigned i=0;a[i];i++)if(c==a[i]){c=b[i];break;}
    }
    return c;
}
#ifndef CAPMIX_HOSTED
static uint8_t readport(uint16_t port) { uint8_t v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port));return v; }
static void writeport(uint16_t port,uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static int writable(void) { for(unsigned i=0;i<100000;i++)if(!(readport(0x64)&2))return 1;return 0; }
static int command(uint16_t port,uint8_t v) { if(!writable())return 0;writeport(port,v);return 1; }
static int response(void) {
    for(unsigned i=0;i<100000;i++) { uint8_t status=readport(0x64);if(status&1){uint8_t v=readport(0x60);if(!(status&0xe0))return v;} }
    return -1;
}
#endif
int cm_ps2_init(void)
{
#ifndef CAPMIX_HOSTED
    uint64_t flags=cm_irq_save();
    int config=-1,ok=0;
    if(!command(0x64,0xad))goto done;
    for(unsigned i=0;i<64 && (readport(0x64)&1);i++)(void)readport(0x60);
    if(!command(0x64,0x20) || (config=response())<0)goto done;
    if(!command(0x64,0x60) || !command(0x60,(uint8_t)((config|0x40)&~0x11)) || !command(0x64,0xae))goto done;
    /* Standard set 2 translated to set 1; require device acknowledgement. */
    if(!command(0x60,0xf5) || response()!=0xfa)goto done;
    if(!command(0x60,0xf0) || response()!=0xfa || !command(0x60,2) || response()!=0xfa)goto done;
    if(!command(0x60,0xf4) || response()!=0xfa)goto done;
    if(!command(0x64,0x60) || !command(0x60,(uint8_t)((config|0x41)&~0x10)))goto done;
    ready=ok=1;
 done:
    if(!ok && config>=0) { (void)command(0x64,0x60);(void)command(0x60,(uint8_t)config);(void)command(0x64,0xae); }
    cm_irq_restore(flags);
    kinfo("ps2: %s (bounded i8042 initialization)",ok?"ready set1 IRQ1 echo":"skipped: keyboard/controller unavailable");
    return ok?0:-1;
#else
    kinfo("ps2: hosted hardware skipped"); return -1;
#endif
}
void cm_ps2_irq(void)
{
#ifndef CAPMIX_HOSTED
    for(unsigned i=0;i<32;i++) {
        uint8_t status=readport(0x64);if(!(status&1))break;
        uint8_t code=readport(0x60);
        if(!ready || (status&0xe0))continue;
        received++;
        unsigned next=(head+1)%sizeof(queue);
        if(next==__atomic_load_n(&tail,__ATOMIC_ACQUIRE)){dropped++;continue;}
        queue[head]=code;__atomic_store_n(&head,next,__ATOMIC_RELEASE);
    }
#endif
}
void cm_ps2_poll(void)
{
    while(tail!=__atomic_load_n(&head,__ATOMIC_ACQUIRE)) {
        char c=decode(queue[tail]);__atomic_store_n(&tail,(tail+1)%sizeof(queue),__ATOMIC_RELEASE);
        if(c) {
            if (!echoed) kinfo("ps2 echo: IRQ -> queued scancode -> character received=%llu dropped=%llu",
                               (unsigned long)received,(unsigned long)dropped);
            if(cm_klog_lock()) { cm_putchar(c);cm_klog_unlock();echoed++; }
        }
    }
}
cm_error_t cm_ps2_read_char(char *out)
{
    if(!out)return CM_ERR_INVAL;
    uint64_t flags=cm_irq_save();
    while(tail!=__atomic_load_n(&head,__ATOMIC_ACQUIRE)) {
        char c=decode(queue[tail]);
        __atomic_store_n(&tail,(tail+1)%sizeof(queue),__ATOMIC_RELEASE);
        if(c){*out=c;cm_irq_restore(flags);return CM_OK;}
    }
    cm_irq_restore(flags);return CM_ERR_AGAIN;
}

int cm_ps2_selftest(void)
{
    unsigned oldshift=shift,oldcaps=caps,olddown=caps_down,oldextended=extended; shift=caps=caps_down=extended=0;
    int bad=decode(30)!='a' || decode(0x9e)!=0 || decode(42)!=0 || decode(30)!='A' ||
        decode(0xaa)!=0 || decode(58)!=0 || decode(30)!='A' || decode(28)!='\n' || decode(14)!='\b';
    shift=caps=caps_down=extended=0;
    static const uint8_t alphabet[]={30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44};
    for(unsigned i=0;i<sizeof(alphabet);i++)if(decode(alphabet[i])!=(char)('a'+i))bad=1;
    static const uint8_t path[]={53,48,23,49,53,46,30,20,52,18,38,33};
    static const char expected[]="/bin/cat.elf";
    for(unsigned i=0;i<sizeof(path);i++)if(decode(path[i])!=expected[i])bad=1;
    /* Held left/right Shift, independent release, Caps repeat and Shift XOR Caps. */
    decode(42);decode(54);
    if(decode(30)!='A' || decode(48)!='B')bad=1;
    decode(0xaa);if(decode(46)!='C')bad=1;
    decode(0xb6);if(decode(32)!='d')bad=1;
    decode(58);decode(58);decode(0xba);
    if(decode(30)!='A' || decode(2)!='1')bad=1;
    decode(42);if(decode(30)!='a' || decode(2)!='!')bad=1;
    decode(0xaa);if(decode(48)!='B')bad=1;
    decode(58);decode(0xba);if(decode(30)!='a')bad=1;
    shift=oldshift;caps=oldcaps;caps_down=olddown;extended=oldextended;
    kinfo("ps2 modifiers: held left/right Shift + CapsLock toggle/repeat + Shift XOR Caps %s",bad?"FAIL":"verified");
    kinfo("ps2 selftest: default lowercase a-z/path + make/break/shift/caps/enter/backspace %s",bad?"FAIL":"verified");
    return bad;
}
