#include "capmix/elf.h"
#include "capmix/fs.h"
#include "capmix/mm.h"
#include "capmix/arch.h"
#include "capmix/config.h"
#include "capmix/proc.h"
#include "capmix/klog.h"
#include <string.h>

#define PT_ADDR 0x000ffffffffff000ull
#define PT_P 1ull
#define PT_PS (1ull << 7)
_Static_assert(sizeof(cm_elf64_ehdr_t) == 64, "ELF64 header ABI");
_Static_assert(sizeof(cm_elf64_phdr_t) == 56, "ELF64 program header ABI");
_Static_assert(sizeof(cm_elf_image_t) <= CM_PAGE_SIZE, "ELF metadata page");
static uint64_t physical(void *p) { return (uint64_t)(uintptr_t)p - cm_hhdm(); }
static uint64_t *table(uint64_t p) { return (void *)(uintptr_t)(p + cm_hhdm()); }
/* L2 pages stay resident. A writable shared group with R owners owns R-1
 * reserved physical pages. Neither this pool nor its reserves enter anon/swap. */
#define ELF_REFS ((CM_MAX_PROCS+CM_MAX_CPUS)*CM_ELF_MAX_PAGES+1)
typedef struct {void *page,*reserve;unsigned refs,writable;} elf_ref_t;
static elf_ref_t refs[ELF_REFS];
static unsigned ref_lock;
static uint64_t split_count;
static uint64_t refs_lock(void){uint64_t f=cm_irq_save();while(__atomic_exchange_n(&ref_lock,1,__ATOMIC_ACQUIRE))__asm__ volatile("pause");return f;}
static void refs_unlock(uint64_t f){__atomic_store_n(&ref_lock,0,__ATOMIC_RELEASE);cm_irq_restore(f);}
static unsigned ref_new(void *page,unsigned writable)
{
    for(unsigned i=1;i<ELF_REFS;i++)if(!refs[i].refs){refs[i]=(elf_ref_t){page,NULL,1,writable};return i;}
    return 0;
}
static void page_drop(cm_elf_page_t *p)
{
    if(!p->shared){cm_page_free(p->backing,0);return;}
    uint64_t f=refs_lock();elf_ref_t *r=&refs[p->shared];
    if(r->refs>1 && r->writable){void *q=r->reserve;r->reserve=*(void **)q;cm_page_free(q,0);}
    if(!--r->refs){cm_page_free(r->page,0);memset(r,0,sizeof(*r));}
    refs_unlock(f);
}
static uint64_t *leaf(cm_elf_image_t *im,uint64_t va)
{
    uint64_t *t=im->root;
    for(unsigned l=cm_paging_levels();l>1;l--){uint64_t e=t[(va>>(12+9*(l-1)))&511];if(!(e&1) || (e&PT_PS))return NULL;t=table(e&PT_ADDR);}
    return &t[(va>>12)&511];
}
static void invalidate(uint64_t va)
{
#ifndef CAPMIX_HOSTED
    __asm__ volatile("invlpg (%0)"::"r"(va):"memory");
#else
    (void)va;
#endif
}
uint64_t cm_elf_cow_splits(void){return __atomic_load_n(&split_count,__ATOMIC_RELAXED);}
cm_error_t cm_elf_cow_write(cm_elf_image_t *im,uint64_t va)
{
    if(!im || im->abi!=CM_ABI_LINUX)return CM_ERR_INVAL;
    uint64_t f=refs_lock();cm_error_t e=CM_ERR_INVAL;
    for(unsigned i=0;i<im->npages;i++) {
        cm_elf_page_t *p=&im->pages[i];if(p->virt!=(va&~CM_PAGE_MASK) || !(p->flags&2) || !p->shared)continue;
        elf_ref_t *r=&refs[p->shared];uint64_t *pte=leaf(im,va);if(!pte || !(*pte&1))break;
        if(r->refs>1) {
            void *q=r->reserve;if(!q)break;
            unsigned id=ref_new(q,1);if(!id){e=CM_ERR_NOSPC;break;}
            r->reserve=*(void **)q;memcpy(q,r->page,CM_PAGE_SIZE);r->refs--;
            p->backing=q;p->shared=id;__atomic_add_fetch(&split_count,1,__ATOMIC_RELAXED);
        }
        *pte=physical(p->backing)|7;invalidate(va);e=CM_OK;break;
    }
    refs_unlock(f);return e;
}
static void free_tables(uint64_t *t, unsigned level)
{
    if (level > 1) for (unsigned i = 0; i < 512; i++)
        if ((t[i] & PT_P) && !(t[i] & PT_PS)) free_tables(table(t[i] & PT_ADDR), level - 1);
    cm_page_free(t, 0);
}
void cm_elf_destroy(cm_elf_image_t *im)
{
    if (!im) return;
    /* Shadow low half aliases full low half; kernel full high half is shared. */
    if (im->shadow && im->shadow != im->root) {
        for (unsigned i = 256; i < 512; i++)
            if (im->shadow[i] & PT_P) free_tables(table(im->shadow[i] & PT_ADDR), cm_paging_levels() - 1);
        cm_page_free(im->shadow, 0);
    }
    if (im->root) {
        for (unsigned i = 0; i < 256; i++)
            if (im->root[i] & PT_P) free_tables(table(im->root[i] & PT_ADDR), cm_paging_levels() - 1);
        cm_page_free(im->root, 0);
    }
    for (unsigned i = 0; i < im->npages; i++) page_drop(&im->pages[i]);
    cm_commit_uncharge(im->charged);
    cm_page_free(im, 0);
}
static int read_exact(cm_file_t *f, void *dst, uint64_t bytes, uint64_t offset)
{
    while (bytes) {
        int64_t n = cm_vfs_pread(f, dst, bytes, offset);
        if (n <= 0 || (uint64_t)n > bytes) return -1;
        dst = (uint8_t *)dst + n; offset += (uint64_t)n; bytes -= (uint64_t)n;
    }
    return 0;
}
void *cm_elf_pointer(const cm_elf_image_t *im, uint64_t addr)
{
    if (!im) return NULL;
    for (unsigned i = 0; i < im->npages; i++)
        if (im->pages[i].virt == (addr & ~CM_PAGE_MASK))
            return (uint8_t *)im->pages[i].backing + (addr & CM_PAGE_MASK);
    return NULL;
}
int cm_elf_user_range(const cm_elf_image_t *im, uint64_t addr, uint64_t bytes, uint32_t flags)
{
    if (!im || !bytes || addr < CM_ELF_USER_MIN || addr >= CM_ELF_USER_MAX ||
        bytes > CM_ELF_USER_MAX - addr) return 0;
    uint64_t end = addr + bytes;
    for (uint64_t page = addr & ~CM_PAGE_MASK; page < end; page += CM_PAGE_SIZE) {
        int found = 0;
        for (unsigned i = 0; i < im->npages; i++)
            if (im->pages[i].virt == page && (im->pages[i].flags & flags) == flags) { found = 1; break; }
        if (!found) return 0;
    }
    return 1;
}
/* Set only PF_W on a leaf of this inactive, private image. No edits to
 * existing KPTI/VMO mappings; executable-data NX enforcement is deferred. */
static void finalize_write(cm_elf_image_t *im, uint64_t virt, int writable)
{
    uint64_t *t = im->root;
    for (unsigned level = cm_paging_levels(); level > 1; level--)
        t = table(t[(virt >> (12 + 9 * (level - 1))) & 511] & PT_ADDR);
    uint64_t *leaf = &t[(virt >> 12) & 511];
    if (!writable) *leaf &= ~2ull;
}
static cm_error_t add_page(cm_elf_image_t *im, uint64_t virt, uint32_t flags)
{
    if (im->npages == CM_ELF_MAX_PAGES) return CM_ERR_NOSPC;
    void *p = cm_page_alloc(0);
    if (!p) return CM_ERR_NOMEM;
    memset(p, 0, CM_PAGE_SIZE);
    unsigned id=0;
    if(im->abi==CM_ABI_LINUX){uint64_t f=refs_lock();id=ref_new(p,!!(flags&2));refs_unlock(f);if(!id){cm_page_free(p,0);return CM_ERR_NOSPC;}}
    im->pages[im->npages++] = (cm_elf_page_t){.virt=virt,.backing=p,.shared=id,.flags=flags};
    return cm_space_map(im->root, virt, physical(p), 1) ? CM_ERR_NOMEM : CM_OK;
}
static cm_error_t build_shadow(cm_elf_image_t *im)
{
#if !CM_KPTI
    im->shadow = im->root; im->shadow_cr3 = im->cr3;
#else
    /* Same stage-0 entry window policy. Own the root before mapping so a
     * partially allocated window is reclaimable on loader failure. */
    im->shadow = cm_page_alloc(0);
    if (!im->shadow) return CM_ERR_NOMEM;
    memset(im->shadow, 0, CM_PAGE_SIZE);
    im->shadow_cr3 = physical(im->shadow);
    for (unsigned i = 0; i < 256; i++) im->shadow[i] = im->root[i];
    extern char __entry_text_start[] __attribute__((weak)), __entry_text_end[] __attribute__((weak));
    extern char __entry_data_start[] __attribute__((weak)), __entry_data_end[] __attribute__((weak));
    uint64_t starts[] = {(uint64_t)(uintptr_t)__entry_text_start, (uint64_t)(uintptr_t)__entry_data_start};
    uint64_t ends[] = {(uint64_t)(uintptr_t)__entry_text_end, (uint64_t)(uintptr_t)__entry_data_end};
    for (unsigned r = 0; r < 2; r++)
        for (uint64_t v = starts[r]; v < ends[r]; v += CM_PAGE_SIZE) {
            uint64_t pte = cm_space_lookup(cm_paging_pml4(), v);
            if (!(pte & PT_P) || cm_space_map(im->shadow, v, pte & PT_ADDR, 0)) return CM_ERR_NOMEM;
        }
#endif
    return CM_OK;
}
/* Only explicit ABI evidence changes persona; SYSV=0 alone stays legacy native.
 * Notes are bounded and streamed, never trusted as an in-memory pointer. */
static cm_error_t persona(cm_file_t *file,const cm_elf64_ehdr_t *eh,
                          const cm_elf64_phdr_t *ph,uint64_t size,cm_abi_t *abi)
{
    int linux_tag=eh->ident[7]==3,native_tag=0;
    if(eh->ident[7]!=0 && eh->ident[7]!=3)return CM_ERR_NOTSUP;
    for(unsigned i=0;i<eh->phnum;i++) {
        const cm_elf64_phdr_t *p=&ph[i];
        if(p->type!=4)continue;
        if(p->filesz>4096 || p->offset>size || p->filesz>size-p->offset)return CM_ERR_INVAL;
        uint64_t at=0;
        while(at<p->filesz) {
            uint32_t h[3];
            if(p->filesz-at<sizeof(h) || read_exact(file,h,sizeof(h),p->offset+at))return CM_ERR_INVAL;
            at+=sizeof(h);
            uint64_t names=CM_ALIGN_UP((uint64_t)h[0],4),desc=CM_ALIGN_UP((uint64_t)h[1],4);
            if(names>p->filesz-at || desc>p->filesz-at-names)return CM_ERR_INVAL;
            char name[8]={0};
            if(h[0]<=sizeof(name) && read_exact(file,name,h[0],p->offset+at))return CM_ERR_IO;
            if(h[0]==4 && !memcmp(name,"GNU",4) && h[2]==1) {
                uint32_t os;
                if(h[1]!=16 || read_exact(file,&os,sizeof(os),p->offset+at+names))return CM_ERR_INVAL;
                if(os)return CM_ERR_NOTSUP;
                linux_tag=1;
            }
            if(h[0]==7 && !memcmp(name,"CapMix",7) && h[2]==1) {
                uint32_t version;
                if(h[1]!=4 || read_exact(file,&version,4,p->offset+at+names) || version!=0)return CM_ERR_INVAL;
                native_tag=1;
            }
            at+=names+desc;
        }
    }
    if(linux_tag && native_tag){kinfo("elf rejected: conflicting CapMix/Linux ABI markers");return CM_ERR_INVAL;}
    *abi=linux_tag?CM_ABI_LINUX:CM_ABI_CAPMIX;
    if(linux_tag && !cm_linux_enabled()) {
        kinfo("linux persona disabled: static Linux ELF refused; native ABI unchanged");return CM_ERR_NOTSUP;
    }
    return CM_OK;
}
/* L4a interpreter only: PIC ET_DYN with no dynamic dependencies, TLS or
 * recursive interpreter. Fixed bias is not ASLR. All pages belong to im. */
static cm_error_t load_interp(cm_elf_image_t *im)
{
    cm_file_t *f=NULL;cm_stat_t st;cm_elf64_ehdr_t eh;
    cm_elf64_phdr_t ph[CM_ELF_MAX_PHDRS];
    cm_error_t e=cm_vfs_stat(CM_ELF_INTERP_PATH,&st);if(e)return e;
    if(st.kind!=CM_INO_FILE || st.size<sizeof(eh))return CM_ERR_INVAL;
    e=cm_vfs_open(CM_ELF_INTERP_PATH,CM_O_RDONLY,&f);if(e)return e;
    if(read_exact(f,&eh,sizeof(eh),0)){e=CM_ERR_IO;goto done;}
    if(memcmp(eh.ident,"\177ELF",4) || eh.ident[4]!=2 || eh.ident[5]!=1 || eh.ident[6]!=1 ||
       eh.type!=3 || eh.machine!=62 || eh.version!=1 || eh.ehsize!=sizeof(eh) ||
       eh.phentsize!=sizeof(ph[0]) || !eh.phnum || eh.phnum>CM_ELF_MAX_PHDRS ||
       eh.phoff>st.size || (uint64_t)eh.phnum*sizeof(ph[0])>st.size-eh.phoff) {e=CM_ERR_INVAL;goto done;}
    if(read_exact(f,ph,eh.phnum*sizeof(ph[0]),eh.phoff)){e=CM_ERR_IO;goto done;}
    cm_abi_t abi;e=persona(f,&eh,ph,st.size,&abi);if(e)goto done;
    if(abi!=CM_ABI_LINUX){e=CM_ERR_NOTSUP;goto done;}
    uint64_t pages=0;int entry=0,loads=0;
    for(unsigned i=0;i<eh.phnum;i++) {
        cm_elf64_phdr_t *p=&ph[i];
        if(p->type==2 || p->type==3 || p->type==7){e=CM_ERR_NOTSUP;goto done;}
        if(p->type!=1)continue;
        if(p->filesz>p->memsz || p->offset>st.size || p->filesz>st.size-p->offset ||
           p->vaddr>=CM_ELF_INTERP_END-CM_ELF_INTERP_BASE ||
           p->memsz>CM_ELF_INTERP_END-CM_ELF_INTERP_BASE-p->vaddr ||
           (p->flags&~7u) || !(p->flags&4) || (p->flags&3)==3 ||
           (p->align>1 && ((p->align&(p->align-1)) || ((p->vaddr-p->offset)&(p->align-1)) || (CM_ELF_INTERP_BASE&(p->align-1))))) {e=CM_ERR_INVAL;goto done;}
        if(!p->memsz)continue;
        uint64_t start=p->vaddr&~CM_PAGE_MASK,end=CM_ALIGN_UP(p->vaddr+p->memsz,CM_PAGE_SIZE);
        for(unsigned j=0;j<i;j++)if(ph[j].type==1 && ph[j].memsz &&
            start<CM_ALIGN_UP(ph[j].vaddr+ph[j].memsz,CM_PAGE_SIZE) && (ph[j].vaddr&~CM_PAGE_MASK)<end){e=CM_ERR_INVAL;goto done;}
        for(unsigned j=0;j<im->npages;j++)if(im->pages[j].virt>=CM_ELF_INTERP_BASE+start && im->pages[j].virt<CM_ELF_INTERP_BASE+end){e=CM_ERR_INVAL;goto done;}
        pages+=(end-start)/CM_PAGE_SIZE;loads++;
        if((p->flags&1) && eh.entry>=p->vaddr && eh.entry-p->vaddr<p->filesz)entry=1;
    }
    if(!loads || !entry){e=CM_ERR_INVAL;goto done;}
    if(pages>CM_ELF_MAX_PAGES-im->npages-CM_ELF_STACK_PAGES){e=CM_ERR_NOSPC;goto done;}
    e=cm_commit_charge(pages);if(e)goto done;im->charged+=pages;
    for(unsigned i=0;i<eh.phnum;i++) {
        cm_elf64_phdr_t *p=&ph[i];if(p->type!=1 || !p->memsz)continue;
        uint64_t base=CM_ELF_INTERP_BASE+p->vaddr;
        for(uint64_t va=base&~CM_PAGE_MASK;va<CM_ALIGN_UP(base+p->memsz,CM_PAGE_SIZE);va+=CM_PAGE_SIZE)
            if((e=add_page(im,va,p->flags)))goto done;
        for(uint64_t off=0;off<p->filesz;) {
            uint64_t va=base+off,n=CM_MIN(p->filesz-off,CM_PAGE_SIZE-(va&CM_PAGE_MASK));
            if(read_exact(f,cm_elf_pointer(im,va),n,p->offset+off)){e=CM_ERR_IO;goto done;}off+=n;
        }
    }
    im->interp_base=CM_ELF_INTERP_BASE;im->entry=CM_ELF_INTERP_BASE+eh.entry;
    im->brk_limit=CM_ELF_INTERP_BASE;
    kinfo("linux L4a interp mapped: base=%llx entry=%llx pages=%llu fixed-bias no-relocations",
          (unsigned long)im->interp_base,(unsigned long)im->entry,(unsigned long)pages);
done:cm_vfs_close(f);return e;
}
cm_error_t cm_elf_load(const char *path, cm_elf_image_t **out)
{
    if (!path || !out) return CM_ERR_INVAL;
    size_t path_len = 0;
    while (path_len < CM_ELF_PATH_MAX && path[path_len]) path_len++;
    if (!path_len || path_len == CM_ELF_PATH_MAX || path[0] != '/') return CM_ERR_INVAL;
    cm_file_t *file = NULL;
    cm_elf_image_t *im = NULL;
    cm_error_t err;
    cm_elf64_ehdr_t eh;
    cm_elf64_phdr_t ph[CM_ELF_MAX_PHDRS];
    cm_stat_t st;
    cm_abi_t abi=CM_ABI_CAPMIX;
    uint64_t user_phdr=0;
    uint64_t pages = CM_ELF_STACK_PAGES;
    int executable_entry = 0, loads = 0, interpreted=0;
    /* Pin file contents against concurrent VFS mutations for the whole load. */
    cm_vfs_lock();
    err = cm_vfs_stat(path, &st);
    if (err) goto done;
    if (st.kind != CM_INO_FILE || st.size < sizeof(eh)) { err = CM_ERR_INVAL; goto done; }
    err = cm_vfs_open(path, CM_O_RDONLY, &file);
    if (err) goto done;
    if (read_exact(file, &eh, sizeof(eh), 0)) { err = CM_ERR_IO; goto done; }
    if (memcmp(eh.ident, "\177ELF", 4) || eh.ident[4] != 2 || eh.ident[5] != 1 || eh.ident[6] != 1 ||
        eh.machine != 62 || eh.version != 1 || eh.ehsize != sizeof(eh) ||
        eh.phentsize != sizeof(ph[0]) || !eh.phnum || eh.phnum > CM_ELF_MAX_PHDRS ||
        eh.phoff > st.size || (uint64_t)eh.phnum * sizeof(ph[0]) > st.size - eh.phoff) {
        err = CM_ERR_INVAL; goto done;
    }
    if (read_exact(file, ph, eh.phnum * sizeof(ph[0]), eh.phoff)) { err = CM_ERR_IO; goto done; }
    for(unsigned i=0;i<eh.phnum;i++)if(ph[i].type==3) {
        if(!cm_linux_interp_enabled()) {kinfo("elf rejected: PT_INTERP dynamic loader unsupported (L4a gate off)");err=CM_ERR_NOTSUP;goto done;}
        if(interpreted++ || ph[i].filesz<2 || ph[i].filesz>CM_ELF_PATH_MAX || ph[i].offset>st.size || ph[i].filesz>st.size-ph[i].offset){kinfo("elf rejected: malformed PT_INTERP");err=CM_ERR_INVAL;goto done;}
        char name[CM_ELF_PATH_MAX];
        if(read_exact(file,name,ph[i].filesz,ph[i].offset)){err=CM_ERR_IO;goto done;}
        if(name[ph[i].filesz-1] || ph[i].filesz!=sizeof(CM_ELF_INTERP_PATH) || memcmp(name,CM_ELF_INTERP_PATH,sizeof(CM_ELF_INTERP_PATH))){err=CM_ERR_NOTSUP;goto done;}
    }
    if (eh.type != 2) { kinfo("elf rejected: only static ET_EXEC supported (no ET_DYN)");err = CM_ERR_NOTSUP; goto done; }
    err=persona(file,&eh,ph,st.size,&abi);if(err)goto done;
    if(interpreted && abi!=CM_ABI_LINUX){err=CM_ERR_NOTSUP;goto done;}
    for (unsigned i = 0; i < eh.phnum; i++) {
        cm_elf64_phdr_t *p = &ph[i];
        if (p->type == 2 || (p->type == 3 && !interpreted) || p->type == 7) { err = CM_ERR_NOTSUP; goto done; } /* DYNAMIC/INTERP/TLS */
        if (p->type != 1) continue;
        if (p->filesz > p->memsz || p->offset > st.size || p->filesz > st.size - p->offset ||
            p->vaddr < CM_ELF_USER_MIN || p->vaddr >= CM_ELF_STACK_TOP - CM_ELF_STACK_PAGES * CM_PAGE_SIZE ||
            p->memsz > CM_ELF_STACK_TOP - CM_ELF_STACK_PAGES * CM_PAGE_SIZE - p->vaddr ||
            (p->flags & ~7u) || !(p->flags & 4) || (p->flags & 3) == 3 ||
            (p->align > 1 && ((p->align & (p->align - 1)) || ((p->vaddr - p->offset) & (p->align - 1))))) {
            err = CM_ERR_INVAL; goto done;
        }
        if(interpreted && (p->vaddr>=CM_ELF_INTERP_BASE || p->memsz>CM_ELF_INTERP_BASE-p->vaddr)){err=CM_ERR_INVAL;goto done;}
        if (!p->memsz) continue;
        uint64_t start = p->vaddr & ~CM_PAGE_MASK;
        uint64_t end = CM_ALIGN_UP(p->vaddr + p->memsz, CM_PAGE_SIZE);
        for (unsigned j = 0; j < i; j++) if (ph[j].type == 1 && ph[j].memsz) {
            uint64_t js = ph[j].vaddr & ~CM_PAGE_MASK;
            uint64_t je = CM_ALIGN_UP(ph[j].vaddr + ph[j].memsz, CM_PAGE_SIZE);
            if (start < je && js < end) { err = CM_ERR_INVAL; goto done; }
        }
        pages += (end - start) / CM_PAGE_SIZE;
        if (pages > CM_ELF_MAX_PAGES) { err = CM_ERR_NOSPC; goto done; }
        if ((p->flags & 1) && eh.entry >= p->vaddr && eh.entry - p->vaddr < p->filesz) executable_entry = 1;
        loads++;
    }
    if (!loads || !executable_entry) { err = CM_ERR_INVAL; goto done; }
    for(unsigned i=0;i<eh.phnum;i++)if(ph[i].type==1 && eh.phoff>=ph[i].offset &&
        eh.phoff-ph[i].offset<=ph[i].filesz &&
        (uint64_t)eh.phnum*eh.phentsize<=ph[i].filesz-(eh.phoff-ph[i].offset)) {
        user_phdr=ph[i].vaddr+(eh.phoff-ph[i].offset);break;
    }
    if(abi==CM_ABI_LINUX && !user_phdr) {
        kinfo("linux ELF rejected: PHDR must be contained in a readable PT_LOAD");err=CM_ERR_NOTSUP;goto done;
    }
    im = cm_page_alloc(0);
    if (!im) { err = CM_ERR_NOMEM; goto done; }
    memset(im, 0, CM_PAGE_SIZE);
    memcpy(im->path, path, path_len + 1);
    im->abi=abi;im->phdr=user_phdr;im->phnum=eh.phnum;im->phent=eh.phentsize;
    err = cm_commit_charge(pages);
    if (err) goto done;
    im->charged = pages; im->entry = im->main_entry = eh.entry;
    im->brk_limit=CM_ELF_STACK_TOP-CM_ELF_STACK_PAGES*CM_PAGE_SIZE; im->stack = CM_ELF_STACK_TOP;
    im->root = cm_space_create(&im->cr3);
    if (!im->root) { err = CM_ERR_NOMEM; goto done; }
    for (unsigned i = 0; i < eh.phnum; i++) {
        cm_elf64_phdr_t *p = &ph[i];
        if (p->type != 1 || !p->memsz) continue;
        uint64_t start = p->vaddr & ~CM_PAGE_MASK;
        uint64_t end = CM_ALIGN_UP(p->vaddr + p->memsz, CM_PAGE_SIZE);
        for (uint64_t va = start; va < end; va += CM_PAGE_SIZE) {
            err = add_page(im, va, p->flags);
            if (err) goto done;
        }
        for (uint64_t off = 0; off < p->filesz;) {
            uint64_t va = p->vaddr + off;
            uint64_t n = CM_MIN(p->filesz - off, CM_PAGE_SIZE - (va & CM_PAGE_MASK));
            if (read_exact(file, cm_elf_pointer(im, va), n, p->offset + off)) { err = CM_ERR_IO; goto done; }
            off += n;
        }
    }
    for(unsigned i=0;i<im->npages;i++)if(im->pages[i].virt+CM_PAGE_SIZE>im->brk_base)im->brk_base=im->pages[i].virt+CM_PAGE_SIZE;
    if(interpreted){err=load_interp(im);if(err)goto done;}
    for (uint64_t va = CM_ELF_STACK_TOP - CM_ELF_STACK_PAGES * CM_PAGE_SIZE; va < CM_ELF_STACK_TOP; va += CM_PAGE_SIZE) {
        err = add_page(im, va, 6);
        if (err) goto done;
    }
    for (unsigned i = 0; i < im->npages; i++)
        finalize_write(im, im->pages[i].virt, im->pages[i].flags & 2);
    im->brk_end=im->brk_base;
    err = cm_elf_set_args(im,NULL,NULL);
    if(err)goto done;
    err = build_shadow(im);
    if (err) goto done;
    *out = im;
    kinfo("elf load: path=%s ET_EXEC entry=%llx PT_LOAD=%d pages=%u BSS zeroed",
          im->path, (unsigned long)im->entry, loads, im->npages);
    if(im->interp_base)kinfo("linux L4a image ready: main_entry=%llx initial_rip=%llx AT_BASE=%llx handoff-only",
        (unsigned long)im->main_entry,(unsigned long)im->entry,(unsigned long)im->interp_base);
    if(im->abi==CM_ABI_LINUX && !im->interp_base)kinfo("linux static ELF: persona=LINUX phdr=%llx phnum=%u",(unsigned long)im->phdr,im->phnum);
    im = NULL;
done:
    if (file) cm_vfs_close(file);
    cm_vfs_unlock();
    cm_elf_destroy(im);
    return err;
}

cm_error_t cm_elf_clone_cow(cm_elf_image_t *src,cm_elf_image_t **out)
{
    if(!src || !out || src->abi!=CM_ABI_LINUX)return CM_ERR_INVAL;
    cm_elf_image_t *dst=cm_page_alloc(0);if(!dst)return CM_ERR_NOMEM;
    memcpy(dst,src,sizeof(*dst));dst->root=dst->shadow=NULL;dst->npages=0;dst->charged=0;
    cm_error_t e=cm_commit_charge(src->charged);if(e)goto fail;dst->charged=src->charged;
    dst->root=cm_space_create(&dst->cr3);if(!dst->root){e=CM_ERR_NOMEM;goto fail;}
    for(unsigned i=0;i<src->npages;i++) {
        cm_elf_page_t p=src->pages[i];void *reserve=NULL;
        if(p.flags&2){reserve=cm_page_alloc(0);if(!reserve){e=CM_ERR_NOMEM;goto fail;}}
        uint64_t f=refs_lock();elf_ref_t *r=&refs[p.shared];
        if(reserve){*(void **)reserve=r->reserve;r->reserve=reserve;}r->refs++;
        refs_unlock(f);dst->pages[dst->npages++]=p;
        if(cm_space_map(dst->root,p.virt,physical(p.backing),1)){e=CM_ERR_NOMEM;goto fail;}
        *leaf(dst,p.virt)&=~2ull;
    }
    e=build_shadow(dst);if(e)goto fail;
    /* Parent PTEs change only once every child resource is owned. */
    for(unsigned i=0;i<src->npages;i++)if(src->pages[i].flags&2){*leaf(src,src->pages[i].virt)&=~2ull;invalidate(src->pages[i].virt);}
    *out=dst;return CM_OK;
fail:cm_elf_destroy(dst);return e;
}
uint64_t cm_elf_brk(cm_elf_image_t *im,uint64_t wanted)
{
    if(!im || im->abi!=CM_ABI_LINUX)return 0;
    uint64_t old=im->brk_end;if(!wanted)return old;
    if(wanted<im->brk_base || wanted>im->brk_limit)return old;
    uint64_t from=CM_ALIGN_UP(old,CM_PAGE_SIZE),to=CM_ALIGN_UP(wanted,CM_PAGE_SIZE);
    if(to>from) {
        uint64_t n=(to-from)/CM_PAGE_SIZE;if(n>CM_ELF_MAX_PAGES-im->npages || cm_commit_charge(n))return old;
        unsigned before=im->npages;cm_error_t e=0;
        for(uint64_t va=from;va<to;va+=CM_PAGE_SIZE)if((e=add_page(im,va,6)))break;
        if(e){while(im->npages>before){cm_elf_page_t *p=&im->pages[--im->npages];uint64_t *pte=leaf(im,p->virt);if(pte)*pte=0;invalidate(p->virt);page_drop(p);}cm_commit_uncharge(n);return old;}
        im->charged+=n;
    } else if(to<from) {
        for(unsigned i=0;i<im->npages;) {
            cm_elf_page_t *p=&im->pages[i];
            if(p->virt>=to && p->virt<from){*leaf(im,p->virt)=0;invalidate(p->virt);page_drop(p);*p=im->pages[--im->npages];im->charged--;cm_commit_uncharge(1);}else i++;
        }
    }
    im->brk_end=wanted;return wanted;
}
