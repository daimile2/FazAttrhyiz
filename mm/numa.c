#include "capmix/numa.h"
#include "capmix/arch.h"
#include "capmix/mm.h"
#include "capmix/sched.h"
#include "capmix/klog.h"
#include <string.h>
/* ACPI 6.6 SRAT types 0/1/2 and SLIT. Topology becomes immutable before APs. */
static unsigned nodes=1;
static uint32_t domains[CM_NUMA_MAX_NODES];
static uint64_t masks[CM_NUMA_MAX_NODES];
static uint8_t distances[CM_NUMA_MAX_NODES][CM_NUMA_MAX_NODES];
static struct { uint32_t apic,domain; } cpus[64];
static unsigned ncpu;
static struct { uint64_t base,end; uint32_t domain; } ranges[CM_NUMA_MAX_RANGES];
static unsigned nranges;
static bool discovered;
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v,p,4); return v; }
#ifndef CAPMIX_HOSTED
static uint64_t u64(const void *p) { uint64_t v; memcpy(&v,p,8); return v; }
static bool checksum(const uint8_t *p,size_t n) { uint8_t sum=0; while (n--) sum+=*p++; return !sum; }
extern int cm_limine_ensure_mapped(uint64_t,uint64_t);
static const uint8_t *physical(uint64_t p,size_t n)
{
    uint64_t h=cm_hhdm();
    if (!p || !h || n>0x100000 || p>UINT64_MAX-h || p+h>UINT64_MAX-4095 || n>UINT64_MAX-(p+h)-4095) return NULL;
    if (cm_limine_ensure_mapped(p+h,n)) return NULL;
    return (const uint8_t *)(uintptr_t)(p+h);
}
static const uint8_t *table(uint64_t p)
{
    const uint8_t *t=physical(p,36); if (!t) return NULL;
    uint32_t n=u32(t+4); if (n<36 || n>0x100000) return NULL;
    t=physical(p,n); return t && checksum(t,n) ? t : NULL;
}
static const uint8_t *find_table(const char *signature)
{
    const uint8_t *r=physical(cm_limine_rsdp(),36);
    if (!r || memcmp(r,"RSD PTR ",8) || !checksum(r,20)) return NULL;
    bool x=r[15]>=2;
    if (x) {
        unsigned len=u32(r+20); if (len<36 || len>4096) return NULL;
        r=physical(cm_limine_rsdp(),len); if (!r || !checksum(r,len)) return NULL;
    }
    uint64_t root=x && u64(r+24) ? u64(r+24) : u32(r+16);
    const uint8_t *t=table(root); if (!t) return NULL;
    unsigned width=!memcmp(t,"XSDT",4) ? 8 : !memcmp(t,"RSDT",4) ? 4 : 0;
    if (!width || (u32(t+4)-36)%width) return NULL;
    for (unsigned off=36;off+width<=u32(t+4);off+=width) {
        const uint8_t *s=table(width==8 ? u64(t+off) : u32(t+off));
        if (s && !memcmp(s,signature,4)) return s;
    }
    return NULL;
}
#endif
static int node_for(uint32_t domain)
{
    for (unsigned i=0;i<nodes;i++) if (domains[i]==domain) return (int)i;
    return -1;
}
static bool add_domain(uint32_t domain)
{
    if (node_for(domain)>=0) return true;
    if (nodes>=CM_NUMA_MAX_NODES) return false;
    domains[nodes++]=domain; return true;
}
/* Separate parser entry permits hosted malformed-table tests without firmware. */
cm_error_t cm_numa_parse_srat(const uint8_t *s,size_t length,uint64_t primary_phys)
{
    nodes=1; domains[0]=0; discovered=false; nranges=ncpu=0;
    if (!s || length<48 || memcmp(s,"SRAT",4) || u32(s+4)!=length) return CM_ERR_INVAL;
    uint8_t sum=0; for (size_t i=0;i<length;i++) sum+=s[i];
    if (sum) return CM_ERR_INVAL;
    nranges=0; ncpu=0;
    for (size_t off=48;off<length;) {
        if (length-off<2 || s[off+1]<2 || s[off+1]>length-off) goto invalid;
        const uint8_t *p=s+off; unsigned len=p[1]; uint32_t flags=0,domain=0,id=0;
        if (p[0]==1) {
            if (len<40) goto invalid;
            flags=u32(p+28); domain=u32(p+2);
            if (flags&1) {
                uint64_t b=(uint64_t)u32(p+8)|((uint64_t)u32(p+12)<<32);
                uint64_t n=(uint64_t)u32(p+16)|((uint64_t)u32(p+20)<<32);
                if (!n || n>UINT64_MAX-b || nranges==CM_NUMA_MAX_RANGES) goto invalid;
                for (unsigned i=0;i<nranges;i++) if (b<ranges[i].end && ranges[i].base<b+n) goto invalid;
                ranges[nranges].base=b; ranges[nranges].end=b+n; ranges[nranges++].domain=domain;
            }
        } else if (p[0]==0 || p[0]==2) {
            if (len<(p[0]==0 ? 16u : 24u)) goto invalid;
            flags=u32(p+(p[0]==0 ? 4 : 12));
            domain=p[0]==0 ? p[2]|((uint32_t)p[9]<<8)|((uint32_t)p[10]<<16)|((uint32_t)p[11]<<24) : u32(p+4);
            id=p[0]==0 ? p[3] : u32(p+8);
            if (flags&1) {
                if (ncpu==64) goto invalid;
                for (unsigned i=0;i<ncpu;i++) if (cpus[i].apic==id) goto invalid;
                cpus[ncpu].apic=id; cpus[ncpu++].domain=domain;
            }
        }
        off+=len;
    }
    bool found=false;
    for (unsigned i=0;i<nranges;i++) if (primary_phys>=ranges[i].base && primary_phys<ranges[i].end) {
        domains[0]=ranges[i].domain; found=true; break;
    }
    if (!found) goto invalid;
    nodes=1;
    for (unsigned i=0;i<nranges;i++) if (!add_domain(ranges[i].domain)) goto invalid;
    for (unsigned i=0;i<ncpu;i++) if (!add_domain(cpus[i].domain)) goto invalid;
    for (unsigned i=0;i<nodes;i++) for (unsigned j=0;j<nodes;j++) distances[i][j]=i==j ? 10 : 20;
    discovered=true; return CM_OK;
invalid:
    nodes=1; domains[0]=0; nranges=0; ncpu=0; discovered=false; return CM_ERR_INVAL;
}
void cm_numa_init(void)
{
    nodes=1; domains[0]=0; discovered=false; distances[0][0]=10;
#ifndef CAPMIX_HOSTED
    uint64_t primary,pages; cm_buddy_primary(&primary,&pages);
    uint64_t initial_total=cm_mm_total_pages();
    const uint8_t *s=find_table("SRAT");
    if (s && cm_numa_parse_srat(s,u32(s+4),primary-cm_hhdm())==CM_OK) {
        bool covered=false;
        uint64_t pa=primary-cm_hhdm();
        for (unsigned i=0;i<nranges;i++)
            if (ranges[i].domain==domains[0] && pa>=ranges[i].base &&
                pages<=UINT64_MAX/CM_PAGE_SIZE && pa<ranges[i].end && pages*CM_PAGE_SIZE<=ranges[i].end-pa)
                covered=true;
        if (!covered) {
            nodes=1; discovered=false; nranges=ncpu=0; domains[0]=0;
            kinfo("numa: primary arena crosses affinity boundary; safe node0 fallback");
        }
        const uint8_t *slit=find_table("SLIT");
        if (slit && u32(slit+4)>=44) {
            uint64_t count=u64(slit+36), len=u32(slit+4)-44;
            bool valid=count && count<=len/count;
            for (unsigned i=0;i<nodes && valid;i++) if (domains[i]>=count) valid=false;
            if (valid) {
                for (unsigned i=0;i<nodes;i++) for (unsigned j=0;j<nodes;j++) {
                    uint8_t d=slit[44+(uint64_t)domains[i]*count+domains[j]];
                    if ((i==j && d!=10) || (i!=j && d<=10)) valid=false;
                }
            }
            if (valid) {
                for (unsigned i=0;i<nodes;i++) for (unsigned j=0;j<nodes;j++)
                    distances[i][j]=slit[44+(uint64_t)domains[i]*count+domains[j]];
                kinfo("numa: SLIT distances validated");
            } else kinfo("numa: SLIT invalid/missing coverage; default distances 10/20");
        } else kinfo("numa: no SLIT; default distances 10/20");
        /* Keep node0's existing arena exactly; add at most 64 MiB per remote node. */
        if (nodes>1) for (unsigned node=1;node<nodes;node++) {
            uint64_t best=0,best_n=0;
            for (unsigned i=0;i<nranges;i++) if (ranges[i].domain==domains[node]) {
                for (unsigned j=0;;j++) {
                    uint64_t b,n; if (cm_limine_usable(j,&b,&n)) break;
                    uint64_t start=b>ranges[i].base ? b : ranges[i].base;
                    uint64_t end=b+n<ranges[i].end ? b+n : ranges[i].end;
                    if (start>UINT64_MAX-4095) continue;
                    start=(start+4095)&~4095ull; end&=~4095ull;
                    if (end>start && end-start>best_n) { best=start; best_n=end-start; }
                }
            }
            if (best_n) {
                uint64_t n=best_n/CM_PAGE_SIZE; if (n>16384) n=16384;
                /* Map in bounded chunks (firmware helper caps table reads only). */
                bool mapped=true;
                for (uint64_t off=0;off<n*CM_PAGE_SIZE;off+=0x100000)
                    if (cm_limine_ensure_mapped(best+cm_hhdm()+off,
                        n*CM_PAGE_SIZE-off<0x100000 ? n*CM_PAGE_SIZE-off : 0x100000)) { mapped=false; break; }
                if (mapped && cm_buddy_add_region(node,best+cm_hhdm(),n)==CM_OK)
                    kinfo("numa: node=%u domain=%u memory phys=%llx pages=%llu online",node,domains[node],
                          (unsigned long)best,(unsigned long)n);
                else kinfo("numa: node=%u memory unavailable, allocation falls back",node);
            }
        }
        /* Extend backing without resetting existing charge or swap backing. */
        if (cm_mm_total_pages()>initial_total) {
            uint64_t added=cm_mm_total_pages()-initial_total;
            cm_commit_add_backing(added-added*CM_COMMIT_RESERVE_PCT/100);
        }
    }
#endif
    if (!discovered || nodes==1) kinfo("numa: single node0 fallback (SRAT absent/invalid or one domain), existing arena unchanged");
    else kinfo("numa: SRAT detected %u nodes; auto balance=round-robin with remote fallback; no page migration",nodes);
}
void cm_numa_update_cpus(void)
{
    memset(masks,0,sizeof(masks));
    for (int cpu=0;cpu<cm_cpu_count();cpu++) {
        int node=cm_numa_cpu_node(cpu); if (node>=0) masks[node]|=1ull<<cpu;
    }
    for (unsigned i=0;i<nodes;i++) {
        cm_numa_stats_t st; cm_numa_stats(i,&st);
        kinfo("numa: node=%u domain=%u cpu_mask=%llx total=%llu free=%llu",i,domains[i],
              (unsigned long)masks[i],(unsigned long)st.total_pages,(unsigned long)st.free_pages);
        for (unsigned j=0;j<nodes;j++)
            kinfo("numa: distance node%u->node%u=%u",i,j,cm_numa_distance(i,j));
    }
}
unsigned cm_numa_node_count(void) { return nodes; }
int cm_numa_cpu_node(int cpu)
{
    if (cpu<0 || cpu>=cm_cpu_count()) return -1;
    uint32_t id;
    if (discovered && !cm_cpu_apic_id(cpu,&id)) {
        for (unsigned i=0;i<ncpu;i++) if (cpus[i].apic==id) return node_for(cpus[i].domain);
    }
    return 0;
}
unsigned cm_numa_distance(unsigned from,unsigned to)
{
    return from<nodes && to<nodes ? (distances[from][to] ? distances[from][to] : from==to ? 10 : 20) : 0;
}
cm_error_t cm_numa_node_info(unsigned node,cm_numa_node_t *out)
{
    if (node>=nodes || !out) return CM_ERR_INVAL;
    out->node=node; out->domain=domains[node]; out->cpu_mask=masks[node];
    if (!discovered && node==0) out->cpu_mask=(1ull<<cm_cpu_count())-1;
    out->distance=cm_numa_distance(node,node); return CM_OK;
}
cm_error_t cm_numa_bind_task(struct cm_task *task,unsigned node)
{
    if (!task) return CM_ERR_INVAL;
    if (node>=nodes) return CM_ERR_NOTSUP;
    task->memory_node=node; return CM_OK;
}
