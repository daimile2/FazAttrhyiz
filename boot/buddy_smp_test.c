#include "capmix/mm.h"
#include "capmix/numa.h"
#include "capmix/arch.h"
#include "capmix/klog.h"
#include <string.h>
#define ROUNDS 10000u
#define BATCH 4u
/* Test metadata is BSS, never allocated from the allocator under test.
 * Current boot pools: <=64MiB/node, <=8 nodes. Check capacity before launch. */
#define OWNER_PAGES (CM_NUMA_MAX_NODES*16384u)
static unsigned owners[OWNER_PAGES];
static cm_buddy_arena_info_t arena[CM_NUMA_MAX_RANGES];
static uint64_t offsets[CM_NUMA_MAX_RANGES],pages;
static unsigned arenas,nodes[CM_NUMA_MAX_NODES],node_count,arrived,participants,abort_test;
static uint64_t allocations[CM_MAX_CPUS],rounds_done[CM_MAX_CPUS];
static unsigned duplicates,corruptions,invalid,nomem;
static uint64_t index_of(void *p)
{
    uint64_t v=(uint64_t)(uintptr_t)p;
    for(unsigned i=0;i<arenas;i++)if(v>=arena[i].base && v-arena[i].base<arena[i].pages*CM_PAGE_SIZE)
        return offsets[i]+(v-arena[i].base)/CM_PAGE_SIZE;
    return UINT64_MAX;
}
static void fail(unsigned *counter)
{__atomic_fetch_add(counter,1,__ATOMIC_RELAXED);__atomic_store_n(&abort_test,1,__ATOMIC_RELEASE);}
static void worker(unsigned cpu)
{
    __atomic_fetch_add(&arrived,1,__ATOMIC_ACQ_REL);
    uint64_t hz=cm_tsc_hz(),deadline=hz?cm_rdtsc()+hz*5:UINT64_MAX;unsigned spins=100000000;
    while(__atomic_load_n(&arrived,__ATOMIC_ACQUIRE)!=participants) {
        if(!spins-- || cm_rdtsc()>deadline){fail(&invalid);return;}
        __asm__ volatile("pause");
    }
    for(unsigned r=0;r<ROUNDS && !__atomic_load_n(&abort_test,__ATOMIC_ACQUIRE);r++) {
        void *held[BATCH]={0};unsigned order[BATCH]={0};uint64_t token=((uint64_t)(r+1)<<8)|(cpu+1);
        for(unsigned k=0;k<BATCH;k++) {
            unsigned o=(r+k+cpu)%4;order[k]=o;
            void *p=(k&1)?cm_page_alloc_node(nodes[cpu%node_count],o):cm_page_alloc(o);
            if(!p){fail(&nomem);break;}
            uint64_t idx=index_of(p),n=1ull<<o;
            if(idx==UINT64_MAX || idx+n>pages || ((uintptr_t)p&((n*CM_PAGE_SIZE)-1))) {
                fail(&invalid);break; /* do not free an untrusted overlapping block */
            }
            int safe=1;
            for(uint64_t j=0;j<n;j++) {
                unsigned empty=0;
                if(!__atomic_compare_exchange_n(owners+idx+j,&empty,(unsigned)token,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
                    fail(&duplicates);safe=0;break;
                }
            }
            if(!safe)break; /* fail-stop after join; never overwrite the victim */
            held[k]=p;allocations[cpu]++;
            for(uint64_t j=0;j<n;j++) {
                volatile uint64_t *first=(void *)((uint8_t *)p+j*CM_PAGE_SIZE+64);
                volatile uint64_t *last=(void *)((uint8_t *)p+(j+1)*CM_PAGE_SIZE-8);
                *first=token;*last=~token;
            }
        }
        for(unsigned k=BATCH;k>0;k--)if(held[k-1]) {
            void *p=held[k-1];uint64_t idx=index_of(p),n=1ull<<order[k-1];
            for(uint64_t j=0;j<n;j++) {
                volatile uint64_t *first=(void *)((uint8_t *)p+j*CM_PAGE_SIZE+64);
                volatile uint64_t *last=(void *)((uint8_t *)p+(j+1)*CM_PAGE_SIZE-8);
                if(*first!=token || *last!=~token)fail(&corruptions);
                unsigned expected=(unsigned)token;
                if(!__atomic_compare_exchange_n(owners+idx+j,&expected,0,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED))fail(&invalid);
            }
            /* Unclaim before free; after free the next legal allocation may reuse it. */
            cm_page_free(p,order[k-1]);
        }
        if(cm_mm_free_pages()>cm_mm_total_pages())fail(&invalid);
        rounds_done[cpu]++;
    }
}
int cm_buddy_smp_selftest(void)
{
    pages=0;arenas=0;node_count=0;arrived=abort_test=0;
    duplicates=corruptions=invalid=nomem=0;
    memset(owners,0,sizeof(owners));memset(allocations,0,sizeof(allocations));memset(rounds_done,0,sizeof(rounds_done));
    while(arenas<CM_NUMA_MAX_RANGES && cm_buddy_arena_snapshot(arenas,&arena[arenas])==CM_OK) {
        offsets[arenas]=pages;pages+=arena[arenas].pages;arenas++;
    }
    if(pages>OWNER_PAGES){kerr("buddy SMP soak: test bitmap capacity exceeded");return 1;}
    cm_numa_stats_t before[CM_NUMA_MAX_NODES],after;
    for(unsigned n=0;n<cm_numa_node_count();n++) {
        if(cm_numa_stats(n,before+n))return 1;
        if(before[n].total_pages)nodes[node_count++]=n;
    }
    if(!node_count)return 1;
#ifdef CAPMIX_HOSTED
    participants=1;
#else
    participants=(unsigned)cm_cpu_count();
#endif
    uint64_t free_before=cm_mm_free_pages();
    kinfo("buddy SMP soak start: cpus=%u rounds/cpu=%u batch=%u orders=0..3",participants,ROUNDS,BATCH);
    int joined=cm_smp_buddy_test(worker);
    uint64_t ops=0,rounds=0;int fails=joined!=(int)participants;
    for(unsigned c=0;c<participants;c++){ops+=allocations[c];rounds+=rounds_done[c];if(rounds_done[c]!=ROUNDS)fails++;}
    uint64_t retained=0;
    for(uint64_t i=0;i<pages;i++)if(__atomic_load_n(owners+i,__ATOMIC_ACQUIRE))retained++;
    if(retained || duplicates || corruptions || invalid || nomem || cm_mm_free_pages()!=free_before)fails++;
    for(unsigned n=0;n<cm_numa_node_count();n++) {
        if(cm_numa_stats(n,&after) || memcmp(before+n,&after,sizeof(after)))fails++;
    }
    kinfo("buddy SMP soak: %s cpus=%u rounds=%llu alloc/free=%llu duplicate=%u corrupt=%u invalid=%u nomem=%u retained=%llu free=%llu/%llu",
          fails?"FAIL":"PASS",participants,(unsigned long)rounds,(unsigned long)ops,duplicates,corruptions,invalid,nomem,
          (unsigned long)retained,(unsigned long)cm_mm_free_pages(),(unsigned long)free_before);
    if(participants<4)kinfo("buddy SMP soak: >=4 CPU coverage skipped; %s path verified",participants==1?"single-CPU":"limited-CPU");
    if(duplicates || corruptions || invalid)cm_panic("buddy ownership corruption; refusing further allocator use");
    return fails;
}
