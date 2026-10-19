#include "capmix/console.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include <string.h>
/* Original ASCII font: five columns, bit 0 at the top. Preserve source case
 * in framebuffer glyphs; never fold filenames, input or serial bytes. */
static const uint8_t letters[][5] = {
 {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
 {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
 {65,65,127,65,65},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
 {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
 {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
 {63,64,64,64,63},{31,32,64,32,31},{127,32,24,32,127},{99,20,8,20,99},
 {3,4,120,4,3},{97,81,73,69,67}
};
/* Original lowercase glyphs, 5x8; row 7 holds descenders inside the 9px cell. */
static const uint8_t lowercase[26][5] = {
 {32,84,84,84,120}, /* a */
 {127,72,68,68,56}, /* b */
 {56,68,68,68,40}, /* c */
 {56,68,68,72,127}, /* d */
 {56,84,84,84,24}, /* e */
 {8,126,9,1,2}, /* f */
 {24,164,164,164,124}, /* g */
 {127,8,4,4,120}, /* h */
 {0,68,125,64,0}, /* i */
 {64,128,132,125,0}, /* j */
 {127,16,40,68,0}, /* k */
 {0,65,127,64,0}, /* l */
 {124,4,24,4,120}, /* m */
 {124,8,4,4,120}, /* n */
 {56,68,68,68,56}, /* o */
 {252,36,36,36,24}, /* p */
 {24,36,36,36,252}, /* q */
 {124,8,4,4,8}, /* r */
 {72,84,84,84,36}, /* s */
 {4,63,68,64,32}, /* t */
 {60,64,64,32,124}, /* u */
 {28,32,64,32,28}, /* v */
 {60,64,48,64,60}, /* w */
 {68,40,16,40,68}, /* x */
 {28,160,160,160,124}, /* y */
 {68,100,84,76,68}, /* z */
};
static const uint8_t digits[][5] = {
 {62,81,73,69,62},{0,66,127,64,0},{98,81,73,73,70},{34,65,73,73,54},
 {24,20,18,127,16},{39,69,69,69,57},{62,73,73,73,48},{1,113,9,5,3},
 {54,73,73,73,54},{6,73,73,73,62}
};
static volatile uint8_t *pixels;
static cm_fb_info_t info;
static unsigned cols, rows, col, row;
static unsigned scale = 1;
static char cells[40][100];
static volatile unsigned lock;
static uint64_t chars;
static uint8_t bits(char ch, unsigned x)
{
    unsigned char c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') return lowercase[c-'a'][x];
    if (c >= 'A' && c <= 'Z') return letters[c-'A'][x];
    if (c >= '0' && c <= '9') return digits[c-'0'][x];
    switch (c) {
    case ' ': return 0;
    case '.': return x==2 ? 64 : 0;
    case ':': return x==2 ? 36 : 0;
    case ';': return x==2 ? 100 : 0;
    case '?': { static const uint8_t a[]={2,1,81,9,6};return a[x]; }
    case '#': return x==1 || x==3 ? 127 : 20;
    case '%': { static const uint8_t a[]={99,19,8,100,99};return a[x]; }
    case '$': { static const uint8_t a[]={36,42,127,42,18};return a[x]; }
    case '&': { static const uint8_t a[]={54,73,85,34,80};return a[x]; }
    case '^': return x==2 ? 1 : (x==1 || x==3 ? 2 : 4);
    case '@': { static const uint8_t a[]={62,65,93,85,30};return a[x]; }
    case '{': return x==1 ? 8 : (x==2 ? 54 : (x==3 ? 65 : 0));
    case '}': return x==3 ? 8 : (x==2 ? 54 : (x==1 ? 65 : 0));
    case ',': return x==2 ? 96 : 0;
    case '-': return 8;
    case '_': return 64;
    case '=': return 20;
    case '/': return (uint8_t)(64u>>x);
    case '\\': return (uint8_t)(1u<<x);
    case '[': return x==0 ? 127 : (x<3 ? 65 : 0);
    case ']': return x==4 ? 127 : (x>1 ? 65 : 0);
    case '(': return x==1 ? 62 : (x==2 ? 65 : 0);
    case ')': return x==3 ? 62 : (x==2 ? 65 : 0);
    case '>': return x<3 ? (uint8_t)((1u<<x)|(64u>>x)) : 0;
    case '<': return x>1 ? (uint8_t)((1u<<(4-x))|(64u>>(4-x))) : 0;
    case '!': return x==2 ? 95 : 0;
    case '+': return x==2 ? 62 : 8;
    case '|': return x==2 ? 127 : 0;
    case '\'': return x==2 ? 3 : 0;
    case '"': return x==1 || x==3 ? 3 : 0;
    case '*': return x==2 ? 62 : 20;
    default: return x==0 || x==4 ? 127 : 65;
    }
}
static uint32_t foreground(void)
{
    return (255u<<info.red_shift)|(255u<<info.green_shift)|(255u<<info.blue_shift);
}
static void paint(unsigned cx,unsigned cy,char old,char next)
{
    for (unsigned x=0;x<5;x++) {
        uint8_t a=bits(old,x), b=bits(next,x), diff=a^b;
        for (unsigned y=0;y<8;y++) if (diff&(1u<<y)) {
            uint32_t v=(b&(1u<<y)) ? foreground() : 0;
            for (unsigned sy=0;sy<scale;sy++) for (unsigned sx=0;sx<scale;sx++) {
                volatile uint8_t *p=pixels+((cy*9+y)*scale+sy)*info.pitch+((cx*6+x)*scale+sx)*(info.bpp/8);
                for (unsigned n=0;n<info.bpp/8;n++) p[n]=(uint8_t)(v>>(8*n));
            }
        }
    }
}
static void setcell(unsigned x,unsigned y,char c)
{ paint(x,y,cells[y][x],c); cells[y][x]=c; }
static void newline(void)
{
    col=0;
    if (++row<rows) return;
    row=rows-1;
    for (unsigned y=0;y+1<rows;y++) for (unsigned x=0;x<cols;x++) setcell(x,y,cells[y+1][x]);
    for (unsigned x=0;x<cols;x++) setcell(x,row,' ');
}
void cm_console_putchar(char c)
{
    if (!pixels) return;
    uint64_t flags=cm_irq_save();
    unsigned n=0;
    while (__sync_lock_test_and_set(&lock,1u)) {
        if (++n==1000000) { cm_irq_restore(flags); return; }
        __asm__ volatile("pause");
    }
    chars++;
    if (c=='\n') newline();
    else if (c=='\r') col=0;
    else if (c=='\b') { if (col) setcell(--col,row,' '); }
    else if (c=='\t') { unsigned count=4-(col%4); while(count--) { setcell(col++,row,' '); if(col==cols)newline(); } }
    else { if ((unsigned char)c<32 || (unsigned char)c>126) c='?'; setcell(col++,row,c); if(col==cols)newline(); }
    __sync_lock_release(&lock); cm_irq_restore(flags);
}
void cm_console_init(void)
{
    if (cm_limine_fb_info(&info)) { kinfo("fb console skipped: no Limine framebuffer (serial retained)"); return; }
    if (info.model!=1 || (info.bpp!=24 && info.bpp!=32) || info.red_size!=8 || info.green_size!=8 || info.blue_size!=8 ||
        info.red_shift>info.bpp-8 || info.green_shift>info.bpp-8 || info.blue_shift>info.bpp-8 ||
        ((255u<<info.red_shift)&(255u<<info.green_shift)) ||
        ((255u<<info.red_shift)&(255u<<info.blue_shift)) || ((255u<<info.green_shift)&(255u<<info.blue_shift)) ||
        info.width<6 || info.width>8192 || info.height<9 || info.height>8192 ||
        info.pitch<info.width*(info.bpp/8) || info.pitch>65536 ||
        info.physical>UINT64_MAX-info.pitch*info.height-4095) {
        kinfo("fb console skipped: unsupported geometry/RGB format"); return;
    }
#ifndef CAPMIX_HOSTED
    uint64_t bytes=info.pitch*info.height, offset=info.physical&4095;
    void *mapping=cm_mmio_map(info.physical&~4095ull,(bytes+offset+4095)/4096);
    if (!mapping) { kinfo("fb console skipped: mapping unavailable"); return; }
    pixels=(volatile uint8_t *)mapping+offset;
#endif
    if (!pixels) return;
    scale=info.width>=960 && info.height>=450 ? 2 : 1;
    cols=(unsigned)(info.width/(6*scale)); if(cols>100)cols=100;
    rows=(unsigned)(info.height/(9*scale)); if(rows>40)rows=40;
    memset(cells,' ',sizeof(cells));
    for (unsigned y=0;y<info.height;y++) for (uint64_t x=0;x<info.width*(info.bpp/8);x++) pixels[y*info.pitch+x]=0;
    kinfo("fb console ready: %llux%llu pitch=%llu bpp=%u text=%ux%u font=ASCII-mixed-case serial retained",
          (unsigned long)info.width,(unsigned long)info.height,(unsigned long)info.pitch,info.bpp,cols,rows);
}
void cm_console_poll(void) { cm_ps2_poll(); }
void cm_console_idle(void)
{
#ifndef CAPMIX_HOSTED
    kinfo("guest console ready: PS/2 keyboard echo when available (%s)",
          pixels ? "serial + framebuffer" : "serial only");
    for (;;) {
        __asm__ volatile("cli" ::: "memory");
        cm_console_poll();
        __asm__ volatile("sti; hlt" ::: "memory");
    }
#endif
}
int cm_console_selftest(void)
{
    if (!letters[0][0]) return 1;
    for(unsigned i=0;i<26;i++) {
        unsigned different=0;
        for(unsigned x=0;x<5;x++)different|=bits((char)('a'+i),x)^bits((char)('A'+i),x);
        if(!different)return 1;
    }
    kinfo("fb ASCII case: a-z distinct from A-Z; path bytes preserved");
#ifdef CAPMIX_HOSTED
    static uint8_t image[64*36*4+16];
    memset(image,0,sizeof(image));
    pixels=image; info=(cm_fb_info_t){.width=64,.height=36,.pitch=256,.bpp=32,.red_shift=16,.green_shift=8};
    scale=1;cols=10;rows=4;row=col=0;memset(cells,' ',sizeof(cells));
    cm_console_putchar('A');
    if (image[256]!=255 || image[257]!=255 || image[258]!=255) { pixels=NULL;return 1; }
    cm_console_putchar('\b');
    for(unsigned i=0;i<64*36*4;i++) if(image[i]) { pixels=NULL;return 1; }
    if(cells[0][0]!=' ') return 1;
    cm_console_putchar('g');
    if(!(bits('g',1)&128) || image[7*256+4]!=255)return 1;
    cm_console_putchar('\b');
    for(unsigned i=0;i<64*36*4;i++)if(image[i])return 1;
    for(unsigned i=0;i<8;i++) { cm_console_putchar('B'); cm_console_putchar('\n'); }
    if(row!=3 || cells[2][0]!='B' || cells[3][0]!=' ') return 1;
    for(unsigned i=64*36*4;i<sizeof(image);i++) if(image[i])return 1;
    pixels=NULL;
    kinfo("fb hosted selftest: mixed-case glyph/descender/backspace/scroll/pitch bounds verified");
#else
    kinfo("fb selftest: %s chars=%llu",pixels ? "active log output" : "skipped (serial only)",(unsigned long)chars);
#endif
    return cm_ps2_selftest();
}
