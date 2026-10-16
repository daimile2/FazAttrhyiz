#ifndef CAPMIX_ELF_H
#define CAPMIX_ELF_H
#include "types.h"
#include "abi.h"

#define CM_ELF_MAX_PHDRS 16u
#define CM_ELF_MAX_PAGES 128u
#define CM_LINUX_MAX_PAGES 2048u /* 8MiB mapped payload, including image/stack/brk/mmap */
#define CM_LINUX_USER_MAX 0x1400000ull /* 16MiB VA span from 0x400000 */
#define CM_LINUX_MMAP_MIN 0x800000ull
#define CM_ELF_EXTRA_CHUNKS ((CM_LINUX_MAX_PAGES-CM_ELF_MAX_PAGES)/CM_ELF_MAX_PAGES)
#define CM_ELF_USER_MIN 0x400000ull
#define CM_ELF_USER_MAX 0x800000ull
#define CM_ELF_STACK_TOP 0x800000ull
#define CM_ELF_STACK_PAGES 4u
#define CM_ELF_INTERP_BASE 0x600000ull
#define CM_ELF_INTERP_END  0x700000ull
#define CM_ELF_INTERP_PATH "/lib/ld-capmix-l4a.so"
#define CM_ELF_PATH_MAX 128u
#define CM_ELF_ARG_MAX 32u
#define CM_ELF_ENV_MAX 32u
#define CM_ELF_ARG_STR_MAX 256u
#define CM_ELF_ARG_BYTES 4096u

typedef struct {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} cm_elf64_ehdr_t;
typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} cm_elf64_phdr_t;
typedef struct {
    uint64_t virt;
    void *backing;
    uint32_t shared; /* L2 resident backing reference, zero for native */
    uint32_t flags; /* ELF PF_R=4 PF_W=2 PF_X=1; internal anonymous tag above bit 7 */
} cm_elf_page_t;
typedef struct cm_elf_image {
    uint64_t *root, *shadow;
    uint64_t cr3, shadow_cr3, entry, stack;
    cm_abi_t abi;
    uint64_t main_entry,interp_base; /* entry is actual initial RIP; AT_ENTRY is main_entry */
    uint64_t phdr;
    uint16_t phnum,phent;
    unsigned npages,argc,envc;
    uint64_t pending; /* globally charged, not yet adopted by backing/reserve records */
    uint64_t charged; /* logical payload pages; Linux physical charges belong to refs */
    uint64_t brk_base,brk_end,brk_limit;
    char path[CM_ELF_PATH_MAX];
    cm_elf_page_t *extra[CM_ELF_EXTRA_CHUNKS];
    cm_elf_page_t pages[CM_ELF_MAX_PAGES]; /* native/first Linux chunk */
} cm_elf_image_t;

cm_elf_page_t *cm_elf_page_at(const cm_elf_image_t *,unsigned);
unsigned cm_elf_page_limit(const cm_elf_image_t *);
void cm_elf_cow_stats(uint64_t *backings,uint64_t *reserves);
cm_error_t cm_elf_load(const char *path, cm_elf_image_t **out);
/* Inactive/private image only. NULL/empty argv defaults to {image path,NULL}. */
cm_error_t cm_elf_set_args(cm_elf_image_t *,const char *const *,const char *const *);
/* Caller must ensure no CPU is using either root or a pointer into the image. */
void cm_elf_destroy(cm_elf_image_t *image);
void *cm_elf_pointer(const cm_elf_image_t *image, uint64_t addr);
int cm_elf_user_range(const cm_elf_image_t *image, uint64_t addr, uint64_t bytes, uint32_t flags);
cm_error_t cm_elf_clone_cow(cm_elf_image_t *,cm_elf_image_t **);
cm_error_t cm_elf_cow_write(cm_elf_image_t *,uint64_t);
uint64_t cm_elf_brk(cm_elf_image_t *,uint64_t);
int64_t cm_elf_mmap(cm_elf_image_t *,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t cm_elf_munmap(cm_elf_image_t *,uint64_t,uint64_t);
uint64_t cm_elf_cow_splits(void);
#endif
