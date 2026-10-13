#include "capmix/elf.h"
#include "capmix/fs.h"
#include "capmix/mm.h"
#include "capmix/arch.h"
#include "capmix/config.h"
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
    for (unsigned i = 0; i < im->npages; i++) cm_page_free(im->pages[i].backing, 0);
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
    im->pages[im->npages++] = (cm_elf_page_t){virt, p, flags};
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
    uint64_t pages = CM_ELF_STACK_PAGES;
    int executable_entry = 0, loads = 0;
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
    if (eh.type != 2) { err = CM_ERR_NOTSUP; goto done; }
    if (read_exact(file, ph, eh.phnum * sizeof(ph[0]), eh.phoff)) { err = CM_ERR_IO; goto done; }
    for (unsigned i = 0; i < eh.phnum; i++) {
        cm_elf64_phdr_t *p = &ph[i];
        if (p->type == 2 || p->type == 3 || p->type == 7) { err = CM_ERR_NOTSUP; goto done; } /* DYNAMIC/INTERP/TLS */
        if (p->type != 1) continue;
        if (p->filesz > p->memsz || p->offset > st.size || p->filesz > st.size - p->offset ||
            p->vaddr < CM_ELF_USER_MIN || p->vaddr >= CM_ELF_STACK_TOP - CM_ELF_STACK_PAGES * CM_PAGE_SIZE ||
            p->memsz > CM_ELF_STACK_TOP - CM_ELF_STACK_PAGES * CM_PAGE_SIZE - p->vaddr ||
            (p->flags & ~7u) || !(p->flags & 4) || (p->flags & 3) == 3 ||
            (p->align > 1 && ((p->align & (p->align - 1)) || ((p->vaddr - p->offset) & (p->align - 1))))) {
            err = CM_ERR_INVAL; goto done;
        }
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
    im = cm_page_alloc(0);
    if (!im) { err = CM_ERR_NOMEM; goto done; }
    memset(im, 0, CM_PAGE_SIZE);
    memcpy(im->path, path, path_len + 1);
    err = cm_commit_charge(pages);
    if (err) goto done;
    im->charged = pages; im->entry = eh.entry; im->stack = CM_ELF_STACK_TOP;
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
    for (uint64_t va = CM_ELF_STACK_TOP - CM_ELF_STACK_PAGES * CM_PAGE_SIZE; va < CM_ELF_STACK_TOP; va += CM_PAGE_SIZE) {
        err = add_page(im, va, 6);
        if (err) goto done;
    }
    for (unsigned i = 0; i < im->npages; i++)
        finalize_write(im, im->pages[i].virt, im->pages[i].flags & 2);
    err = cm_elf_set_args(im,NULL,NULL);
    if(err)goto done;
    err = build_shadow(im);
    if (err) goto done;
    *out = im;
    kinfo("elf load: path=%s ET_EXEC entry=%llx PT_LOAD=%d pages=%u BSS zeroed",
          im->path, (unsigned long)im->entry, loads, im->npages);
    im = NULL;
done:
    if (file) cm_vfs_close(file);
    cm_vfs_unlock();
    cm_elf_destroy(im);
    return err;
}
