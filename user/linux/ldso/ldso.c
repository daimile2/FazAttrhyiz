/* Original CapMix bounded rtld, ELF64/x86-64 SysV ABI. No libc or TLS.
 * User-space relocation only; mmap remains eager anonymous PRIVATE RW.
 * Current kernel lacks full NX: DSO code is executable, without W^X/RELRO.
 */
#include <elf.h>
#include "runtime.h"
#define OBJECTS 4u
#define PHDRS 16u
#define SPAN (1024u*1024u)
#define FILE_MAX (512u*1024u)
#define PAGE 4096u
#define ROUND(x) (((x)+PAGE-1)&~(uint64_t)(PAGE-1))
typedef struct {
    char name[64];uint64_t base,span;void *map;
    Elf64_Phdr ph[PHDRS];unsigned nph;
    Elf64_Sym *symbols;const char *strings;uint64_t strsz,nsyms;
    Elf64_Rela *rela,*plt;uint64_t nrela,nplt;
    uint64_t needed[3];unsigned nneeded;
} object;
static object objects[OBJECTS];static unsigned count;
static void *buffer;static uint64_t buffer_size;static long opened=-1;
static unsigned relative_count,glob_count,jump_count,absolute_count;
static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    text("ldso: reject ");text(why);text("\n");
    if(opened>=0)(void)call6(3,opened,0,0,0,0,0);
    release(buffer,buffer_size);
    for(unsigned i=1;i<count;i++)release(objects[i].map,objects[i].span);
    (void)call6(231,126,0,0,0,0,0);for(;;)__asm__ volatile("ud2");
}
static int range(const object *o,uint64_t address,uint64_t size,unsigned flags)
{
    if(!size || address+size<address)return 0;
    for(unsigned i=0;i<o->nph;i++) {
        const Elf64_Phdr *p=&o->ph[i];if(p->p_type!=PT_LOAD || (p->p_flags&flags)!=flags)continue;
        uint64_t start=o->base+p->p_vaddr;
        if(start<o->base || address<start)continue;
        uint64_t off=address-start;if(off<=p->p_memsz && size<=p->p_memsz-off)return 1;
    }
    return 0;
}
static uint64_t pointer(const object *o,uint64_t value)
{if(value>UINT64_MAX-o->base)fail("dynamic pointer overflow");return o->base+value;}
static const char *string(const object *o,uint64_t offset)
{
    if(offset>=o->strsz)fail("string offset");
    for(uint64_t i=offset;i<o->strsz;i++)if(!o->strings[i])return o->strings+offset;
    fail("unterminated string");
}
static void dynamic(object *o,int self)
{
    const Elf64_Phdr *dp=NULL;
    for(unsigned i=0;i<o->nph;i++) {
        if(o->ph[i].p_type==PT_TLS)fail("TLS unsupported");
        if(o->ph[i].p_type==PT_DYNAMIC){if(dp)fail("duplicate PT_DYNAMIC");dp=&o->ph[i];}
    }
    if(!dp || !dp->p_filesz || dp->p_filesz%sizeof(Elf64_Dyn) || dp->p_filesz>4096)fail("PT_DYNAMIC size");
    uint64_t address=pointer(o,dp->p_vaddr);
    if(!range(o,address,dp->p_filesz,PF_R))fail("PT_DYNAMIC range");
    Elf64_Dyn *d=(void *)address;uint64_t hash=0,sym=0,str=0,rela=0,rsz=0,plt=0,psz=0;
    uint64_t syment=0,rent=0,ptype=0;int ended=0;
    /* Reject duplicate structural tags; repeated DT_NEEDED is intentional. */
    uint64_t seen=0;
    for(unsigned i=0;i<dp->p_filesz/sizeof(*d);i++) {
        uint64_t tag=d[i].d_tag,val=d[i].d_un.d_val;
        if(tag==DT_NULL){ended=1;break;}
        if(tag<64 && tag!=DT_NEEDED){uint64_t bit=1ull<<tag;if(seen&bit)fail("duplicate dynamic tag");seen|=bit;}
        switch(tag) {
        case DT_HASH:hash=val;break;case DT_SYMTAB:sym=val;break;case DT_STRTAB:str=val;break;
        case DT_STRSZ:o->strsz=val;break;case DT_SYMENT:syment=val;break;
        case DT_RELA:rela=val;break;case DT_RELASZ:rsz=val;break;case DT_RELAENT:rent=val;break;
        case DT_JMPREL:plt=val;break;case DT_PLTRELSZ:psz=val;break;case DT_PLTREL:ptype=val;break;
        case DT_NEEDED:
            if(self || o->nneeded==3)fail("dependency limit/self dependency");
            o->needed[o->nneeded++]=val;break;
        case DT_FLAGS:if(val&~(uint64_t)(DF_BIND_NOW|(self?DF_SYMBOLIC:0)))fail("DT_FLAGS unsupported");break;
        case DT_FLAGS_1:if(val&~(uint64_t)DF_1_NOW)fail("DT_FLAGS_1 unsupported");break;
        case DT_SYMBOLIC:if(!self)fail("DT_SYMBOLIC unsupported");break;
        case DT_RELACOUNT:if(val>rsz/sizeof(Elf64_Rela) && rsz)fail("RELACOUNT");break;
        case DT_PLTGOT:case DT_DEBUG:case DT_SONAME:case DT_BIND_NOW:break;
        default:text("ldso: unsupported dynamic tag=");number(tag);text("\n");fail("dynamic feature unsupported");
        }
    }
    if(!ended || !hash || !sym || !str || !o->strsz || o->strsz>65536 || syment!=sizeof(Elf64_Sym))fail("dynamic tables");
    uint64_t h=pointer(o,hash);if(!range(o,h,8,PF_R))fail("hash header");
    uint32_t *hp=(void *)h;if(!hp[0] || hp[0]>2048 || !hp[1] || hp[1]>2048 ||
        !range(o,h,8+4ull*(hp[0]+hp[1]),PF_R))fail("SysV hash bounds");
    o->nsyms=hp[1];o->symbols=(void *)pointer(o,sym);o->strings=(void *)pointer(o,str);
    if(!range(o,(uint64_t)o->symbols,o->nsyms*sizeof(Elf64_Sym),PF_R) || !range(o,(uint64_t)o->strings,o->strsz,PF_R))fail("symbol/string table range");
    if((rsz && (!rela || rent!=sizeof(Elf64_Rela))) || rsz%sizeof(Elf64_Rela) || rsz/sizeof(Elf64_Rela)>2048 ||
       (psz && (!plt || ptype!=DT_RELA || rent!=sizeof(Elf64_Rela))) || psz%sizeof(Elf64_Rela) || psz/sizeof(Elf64_Rela)>2048)fail("RELA table size/type");
    o->nrela=rsz/sizeof(Elf64_Rela);o->nplt=psz/sizeof(Elf64_Rela);
    o->rela=(void *)pointer(o,rela);o->plt=(void *)pointer(o,plt);
    if((rsz && !range(o,(uint64_t)o->rela,rsz,PF_R)) || (psz && !range(o,(uint64_t)o->plt,psz,PF_R)))fail("RELA range");
    if(self && (rsz || psz))fail("interpreter self relocation unsupported");
}
static void load(const char *name)
{
    size_t n=length(name);if(!n || n>=64 || equal(name,".") || equal(name,".."))fail("DT_NEEDED name");
    for(size_t i=0;i<n;i++) {
        char c=name[i];if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='.' || c=='_' || c=='-'))fail("DT_NEEDED path unsupported");
    }
    for(unsigned i=1;i<count;i++)if(equal(name,objects[i].name))return;
    if(count==OBJECTS)fail("object limit");
    object *o=&objects[count++];bytes(o->name,name,n+1);
    char path[80]="/lib/";bytes(path+5,name,n+1);
    opened=call6(2,(long)path,0,0,0,0,0);if(opened<0)fail("library open");
    long size=call6(8,opened,0,2,0,0,0);if(size<(long)sizeof(Elf64_Ehdr) || size>FILE_MAX)fail("library file size");
    buffer_size=ROUND((uint64_t)size);buffer=allocate(buffer_size);if(!buffer)fail("file buffer ENOMEM");
    if(call6(8,opened,0,0,0,0,0)!=0)fail("library seek");
    uint64_t done=0;while(done<(uint64_t)size){long r=call6(0,opened,(long)buffer+done,size-done,0,0,0);if(r<=0 || (uint64_t)r>(uint64_t)size-done)fail("library read");done+=r;}
    (void)call6(3,opened,0,0,0,0,0);opened=-1;
    const Elf64_Ehdr *eh=buffer;
    if(eh->e_ident[0]!=127 || eh->e_ident[1]!='E' || eh->e_ident[2]!='L' || eh->e_ident[3]!='F' ||
       eh->e_ident[4]!=ELFCLASS64 || eh->e_ident[5]!=ELFDATA2LSB || eh->e_ident[6]!=EV_CURRENT ||
       eh->e_type!=ET_DYN || eh->e_machine!=EM_X86_64 || eh->e_version!=EV_CURRENT || eh->e_ehsize!=sizeof(*eh) ||
       !eh->e_phnum || eh->e_phnum>PHDRS || eh->e_phentsize!=sizeof(Elf64_Phdr) ||
       eh->e_phoff>(uint64_t)size || eh->e_phnum*sizeof(Elf64_Phdr)>(uint64_t)size-eh->e_phoff)fail("library ELF header");
    o->nph=eh->e_phnum;bytes(o->ph,(char *)buffer+eh->e_phoff,o->nph*sizeof(Elf64_Phdr));
    int origin=0;
    for(unsigned i=0;i<o->nph;i++) {
        Elf64_Phdr *p=&o->ph[i];if(p->p_type==PT_TLS)fail("TLS unsupported");if(p->p_type==PT_INTERP)fail("recursive interpreter");
        if(p->p_type!=PT_LOAD)continue;
        if(p->p_filesz>p->p_memsz || p->p_offset>(uint64_t)size || p->p_filesz>(uint64_t)size-p->p_offset ||
           p->p_vaddr>=SPAN || p->p_memsz>SPAN-p->p_vaddr || !(p->p_flags&PF_R) || (p->p_flags&~7u) || (p->p_flags&3)==3 ||
           (p->p_align>1 && ((p->p_align&(p->p_align-1)) || p->p_align>PAGE || ((p->p_vaddr-p->p_offset)&(p->p_align-1)))))fail("library PT_LOAD bounds/flags");
        if(!p->p_memsz)continue;
        uint64_t start=p->p_vaddr&~4095ull,end=ROUND(p->p_vaddr+p->p_memsz);
        if(!start)origin=1;
        if(end>o->span)o->span=end;
        for(unsigned j=0;j<i;j++)if(o->ph[j].p_type==PT_LOAD && o->ph[j].p_memsz &&
            start<ROUND(o->ph[j].p_vaddr+o->ph[j].p_memsz) && (o->ph[j].p_vaddr&~4095ull)<end)fail("overlapping library PT_LOAD");
    }
    if(!origin || !o->span)fail("library origin");
    o->map=allocate(o->span);if(!o->map)fail("library map ENOMEM");o->base=(uint64_t)o->map;
    for(unsigned i=0;i<o->nph;i++)if(o->ph[i].p_type==PT_LOAD)
        bytes((void *)(o->base+o->ph[i].p_vaddr),(char *)buffer+o->ph[i].p_offset,o->ph[i].p_filesz);
    release(buffer,buffer_size);buffer=NULL;buffer_size=0;
    dynamic(o,0);text("ldso: loaded ");text(path);text("\n");
}
static uint64_t symbol(object *from,uint64_t index)
{
    if(!index || index>=from->nsyms)fail("relocation symbol index");
    Elf64_Sym *s=&from->symbols[index];const char *name=string(from,s->st_name);
    if((ELF64_ST_BIND(s->st_info)!=STB_GLOBAL && ELF64_ST_BIND(s->st_info)!=STB_WEAK) ||
       (ELF64_ST_TYPE(s->st_info)!=STT_FUNC && ELF64_ST_TYPE(s->st_info)!=STT_OBJECT && ELF64_ST_TYPE(s->st_info)!=STT_NOTYPE) || s->st_other!=STV_DEFAULT)fail("symbol type/visibility unsupported");
    for(unsigned i=0;i<count;i++)for(uint64_t j=1;j<objects[i].nsyms;j++) {
        object *o=&objects[i];Elf64_Sym *candidate=&o->symbols[j];
        if(candidate->st_shndx==SHN_UNDEF || (ELF64_ST_BIND(candidate->st_info)!=STB_GLOBAL && ELF64_ST_BIND(candidate->st_info)!=STB_WEAK))continue;
        if(!equal(name,string(o,candidate->st_name)))continue;
        unsigned type=ELF64_ST_TYPE(candidate->st_info);
        if(candidate->st_other!=STV_DEFAULT || (type!=STT_FUNC && type!=STT_OBJECT && type!=STT_NOTYPE) || candidate->st_shndx>=SHN_LORESERVE)fail("symbol definition unsupported");
        uint64_t value=pointer(o,candidate->st_value);
        if(!range(o,value,candidate->st_size?candidate->st_size:1,type==STT_FUNC?PF_X:PF_R))fail("symbol definition range");
        return value;
    }
    if(ELF64_ST_BIND(s->st_info)==STB_WEAK)return 0;
    text("ldso: unresolved symbol ");text(name);text("\n");fail("unresolved symbol");
}
static void relocate(object *o,Elf64_Rela *r,uint64_t n)
{
    for(uint64_t i=0;i<n;i++) {
        unsigned type=ELF64_R_TYPE(r[i].r_info);uint64_t index=ELF64_R_SYM(r[i].r_info),value;
        if(type!=R_X86_64_RELATIVE && type!=R_X86_64_GLOB_DAT && type!=R_X86_64_JUMP_SLOT && type!=R_X86_64_64) {
            text("ldso: unsupported relocation=");number(type);text("\n");fail("relocation unsupported");
        }
        uint64_t target=pointer(o,r[i].r_offset);
        if(!range(o,target,8,PF_W) || (target&7))fail("relocation target (no text relocations)");
        if(type==R_X86_64_RELATIVE){if(index)fail("RELATIVE symbol");value=o->base+(uint64_t)r[i].r_addend;relative_count++;}
        else {
            uint64_t s=symbol(o,index);
            if(type==R_X86_64_64){value=s+(uint64_t)r[i].r_addend;absolute_count++;}
            else {if(r[i].r_addend)fail("GLOB_DAT/JUMP_SLOT addend unsupported");value=s;if(type==R_X86_64_GLOB_DAT)glob_count++;else jump_count++;}
        }
        *(uint64_t *)target=value;
    }
}
__attribute__((visibility("hidden"))) uint64_t rtld_main(uint64_t *stack)
{
    uint64_t argc=*stack++;if(!argc || argc>32)fail("argc");stack+=argc;if(*stack++)fail("argv terminator");
    unsigned i;for(i=0;i<33 && *stack;i++)stack++;if(i==33)fail("envp limit");stack++;
    uint64_t phdr=0,phnum=0,phent=0,entry=0,base=0,pagesz=0;
    for(i=0;i<32;i++,stack+=2){if(!stack[0])break;switch(stack[0]) {
        case AT_PHDR:phdr=stack[1];break;case AT_PHNUM:phnum=stack[1];break;case AT_PHENT:phent=stack[1];break;
        case AT_ENTRY:entry=stack[1];break;case AT_BASE:base=stack[1];break;case AT_PAGESZ:pagesz=stack[1];break;
    }}
    if(i==32 || phdr<0x400000 || phdr>0x600000-16*sizeof(Elf64_Phdr) || !phnum || phnum>PHDRS || phent!=sizeof(Elf64_Phdr) || base!=0x600000 || pagesz!=PAGE)fail("auxv");
    object *main=&objects[0];count=1;main->nph=phnum;bytes(main->ph,(void *)phdr,phnum*sizeof(Elf64_Phdr));
    if(!range(main,entry,1,PF_X))fail("AT_ENTRY range");
    dynamic(main,0);
    const Elf64_Ehdr *eh=(void *)base;object self={0};self.base=base;
    if(eh->e_type!=ET_DYN || eh->e_phentsize!=sizeof(Elf64_Phdr) || !eh->e_phnum || eh->e_phnum>PHDRS || eh->e_phoff>SPAN-PHDRS*sizeof(Elf64_Phdr))fail("interpreter header");
    self.nph=eh->e_phnum;bytes(self.ph,(void *)(base+eh->e_phoff),self.nph*sizeof(Elf64_Phdr));dynamic(&self,1);
    /* Breadth-first dependency closure, at most main + three DSOs, cycles deduped. */
    for(unsigned k=0;k<count;k++)for(unsigned j=0;j<objects[k].nneeded;j++)load(string(&objects[k],objects[k].needed[j]));
    for(unsigned k=0;k<count;k++){relocate(&objects[k],objects[k].rela,objects[k].nrela);relocate(&objects[k],objects[k].plt,objects[k].nplt);}
    text("ldso: reloc RELATIVE=");number(relative_count);text(" GLOB_DAT=");number(glob_count);text(" JUMP_SLOT=");number(jump_count);text(" R64=");number(absolute_count);text("\n");
    return entry;
}
