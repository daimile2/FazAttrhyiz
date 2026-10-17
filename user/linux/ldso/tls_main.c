/* No libc/host TLS runtime. TLS is accessed through two initial-exec DSOs. */
#include "runtime.h"
extern int tls_a_set(long),tls_b_set(long),tls_a_check(long),tls_b_check(long);
static uint64_t ticks(void){unsigned lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));return ((uint64_t)hi<<32)|lo;}
int main(void)
{
    long cookie=call6(39,0,0,0,0,0,0);uint64_t fs=0,self=0;
    if(cookie<=0 || call6(158,0x1003,(long)&fs,0,0,0,0) || !fs)return 81;
    __asm__ volatile("movq %%fs:0,%0":"=r"(self));if(self!=fs)return 82;
    if(!tls_a_set(cookie) || !tls_b_set(cookie+123))return 83;
    uint64_t end=ticks()+300000000ull;
    do {if(!tls_a_check(cookie) || !tls_b_check(cookie+123))return 84;}while(ticks()<end);
    text("linux tls: payload verified\n");return 0;
}
