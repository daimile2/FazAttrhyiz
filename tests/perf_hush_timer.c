/* Diagnostic Linux wrapper: bounded fork+exec+wait, guest TSC and CPU recorded. */
typedef unsigned long u64;
static long sc(long nr,long a,long b,long c)
{
    register long r10 __asm__("r10")=0,r8 __asm__("r8")=0,r9 __asm__("r9")=0;long out;
    __asm__ volatile("syscall":"=a"(out):"a"(nr),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","cc","memory");return out;
}
static u64 stamp(void){unsigned lo,hi;__asm__ volatile("lfence;rdtsc":"=a"(lo),"=d"(hi)::"memory");return ((u64)hi<<32)|lo;}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    if(argv[2][0]=='b'){u64 mask=1;if(sc(203,0,8,(long)&mask))return 3;}
    unsigned cpu=999,node=999;if(sc(309,(long)&cpu,(long)&node,0))return 4;
    u64 begin=stamp();long pid=sc(57,0,0,0);if(pid<0)return 5;
    if(!pid){const char *args[]={"/bin/busybox-sh.elf","sh",argv[1],0};const char *env[]={"PATH=/bin",0};sc(59,(long)args[0],(long)args,(long)env);return 6;}
    int status=0;if(sc(61,pid,(long)&status,0)!=pid || status)return 7;
    u64 elapsed=stamp()-begin;static char b[120];unsigned n=0;
    const char *labels[]={"hush timer cycles="," cpu="};u64 values[]={elapsed,cpu};
    for(unsigned i=0;i<2;i++){for(const char *s=labels[i];*s;s++)b[n++]=*s;char d[24];unsigned k=0;u64 v=values[i];do{d[k++]=(char)('0'+v%10);v/=10;}while(v);while(k)b[n++]=d[--k];}
    b[n++]='\n';return sc(1,2,(long)b,n)==n?0:8;
}
