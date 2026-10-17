/* CapMix-specific Linux-user port, MIT; upstream VM/GC/parser unchanged.
 * Fixed 256KiB GC heap, 8KiB source, no host filesystem/import/REPL runtime. */
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "py/compile.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/cstack.h"
#include "shared/runtime/gchelper.h"
static unsigned char heap[256*1024] __attribute__((aligned(16)));
static char source[8193];
static void output(const char *s,size_t n) {
    while(n) {
        ssize_t r=write(1,s,n);
        if(r<0 && errno==EINTR)continue;
        if(r<=0)_exit(74);
        s+=r;n-=(size_t)r;
    }
}
#define SAY(s) output(s,sizeof(s)-1)
mp_uint_t mp_hal_stdout_tx_strn(const char *s,size_t n) {output(s,n);return n;}
void mp_hal_stdout_tx_strn_cooked(const char *s,size_t n) {output(s,n);}
void gc_collect(void) {
    gc_collect_start();gc_helper_collect_regs_and_stack();gc_collect_end();
}
void nlr_jump_fail(void *p) {(void)p;SAY("micropython fatal\n");_exit(70);}
int main(int argc,char **argv) {
    const char *code=NULL;
    if(argc==3 && !strcmp(argv[1],"-c")) {
        if(strlen(argv[2])>8192){SAY("script too large\n");return 65;}
        code=argv[2];
    } else if(argc==2) {
        int fd=open(argv[1],O_RDONLY);
        if(fd<0){SAY("script open failed\n");return 66;}
        size_t n=0;
        while(n<sizeof(source)) {
            ssize_t r=read(fd,source+n,sizeof(source)-n);
            if(r<0 && errno==EINTR)continue;
            if(r<0){close(fd);SAY("script read failed\n");return 74;}
            if(!r)break;
            n+=(size_t)r;
        }
        close(fd);
        if(n>8192){SAY("script too large\n");return 65;}
        if(memchr(source,0,n)){SAY("script contains NUL\n");return 65;}
        source[n]=0;code=source;
    } else {SAY("usage: micropython.elf [-c code | script.py]\n");return 64;}
    /* Existing user stack is 16KiB. Reserve room for libc/exception printing. */
    mp_cstack_init_with_sp_here(8192);
    gc_init(heap,heap+sizeof(heap));mp_init();int status=0;
    nlr_buf_t nlr;
    if(nlr_push(&nlr)==0) {
        mp_lexer_t *lex=mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,code,strlen(code),false);
        mp_parse_tree_t tree=mp_parse(lex,MP_PARSE_FILE_INPUT);
        mp_obj_t fun=mp_compile(&tree,MP_QSTR__lt_stdin_gt_,false);
        mp_call_function_0(fun);nlr_pop();
    } else {
        mp_obj_t exc=MP_OBJ_FROM_PTR(nlr.ret_val);
        if(mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(((mp_obj_base_t *)nlr.ret_val)->type),MP_OBJ_FROM_PTR(&mp_type_SystemExit))) {
            mp_obj_t value=mp_obj_exception_get_value(exc);mp_int_t result=0;
            status=value==mp_const_none?0:mp_obj_get_int_maybe(value,&result)?(int)(result&255):1;
        } else {mp_obj_print_exception(&mp_plat_print,exc);status=1;}
    }
    mp_deinit();return status;
}
