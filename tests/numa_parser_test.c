/* Isolated hosted parser tests: never mutate a running kernel topology. */
#include "capmix/numa.h"
#include "capmix/klog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
extern cm_error_t cm_numa_parse_srat(const uint8_t *,size_t,uint64_t);
int cm_cpu_count(void) { return 2; }
int cm_cpu_apic_id(int cpu,uint32_t *id) { *id=cpu==0 ? 4 : 300; return 0; }
cm_error_t cm_numa_stats(unsigned n,cm_numa_stats_t *s) { (void)n; memset(s,0,sizeof(*s)); return 0; }
void cm_klog(cm_log_level_t level,const char *fmt,...) { (void)level; (void)fmt; }
static void put32(uint8_t *p,uint32_t v) { memcpy(p,&v,4); }
static void seal(uint8_t *p,size_t n)
{
    put32(p+4,(uint32_t)n); p[9]=0;
    uint8_t sum=0; for (size_t i=0;i<n;i++) sum+=p[i]; p[9]=(uint8_t)(0-sum);
}
static void memory(uint8_t *p,uint32_t domain,uint32_t base,uint32_t size)
{
    p[0]=1; p[1]=40; put32(p+2,domain); put32(p+8,base); put32(p+16,size); put32(p+28,1);
}
int main(void)
{
    uint8_t s[168]={0}; memcpy(s,"SRAT",4);
    memory(s+48,17,0x100000,0x100000); memory(s+88,42,0x300000,0x100000);
    s[128]=0; s[129]=16; s[130]=17; s[131]=4; put32(s+132,1);
    s[144]=2; s[145]=24; put32(s+148,42); put32(s+152,300); put32(s+156,1);
    seal(s,sizeof(s));
    assert(cm_numa_parse_srat(s,sizeof(s),0x100000)==CM_OK);
    assert(cm_numa_node_count()==2);
    assert(cm_numa_cpu_node(0)==0 && cm_numa_cpu_node(1)==1);
    assert(cm_numa_distance(0,1)==20);
    cm_numa_node_t info; assert(cm_numa_node_info(1,&info)==CM_OK && info.domain==42);
    assert(cm_numa_parse_srat(s,sizeof(s),0x300000)==CM_OK);
    assert(cm_numa_cpu_node(0)==1 && cm_numa_cpu_node(1)==0);
    assert(cm_numa_parse_srat(s,sizeof(s)-1,0x100000)==CM_ERR_INVAL);
    assert(cm_numa_node_count()==1);
    s[9]^=1; assert(cm_numa_parse_srat(s,sizeof(s),0x100000)==CM_ERR_INVAL); s[9]^=1;
    s[49]=0; seal(s,sizeof(s)); assert(cm_numa_parse_srat(s,sizeof(s),0x100000)==CM_ERR_INVAL);
    s[49]=40; put32(s+96,0x180000); seal(s,sizeof(s));
    assert(cm_numa_parse_srat(s,sizeof(s),0x100000)==CM_ERR_INVAL); /* overlap */
    put32(s+96,0x300000); put32(s+60,UINT32_MAX); put32(s+56,0xfffff000); seal(s,sizeof(s));
    assert(cm_numa_parse_srat(s,sizeof(s),0x100000)==CM_ERR_INVAL); /* address overflow */
    put32(s+60,0); put32(s+56,0x100000); seal(s,sizeof(s));
    assert(cm_numa_parse_srat(s,sizeof(s),0x900000)==CM_ERR_INVAL); /* arena not covered */
    s[157]=0; s[158]=0; s[159]=0; put32(s+156,0); seal(s,sizeof(s));
    assert(cm_numa_parse_srat(s,sizeof(s),0x100000)==CM_OK);
    assert(cm_numa_cpu_node(1)==0); /* disabled CPU affinity falls back */
    puts("NUMA parser PASS: LAPIC/x2APIC, sparse domains, primary reorder, disabled affinity, malformed/overlap/overflow");
    return 0;
}
