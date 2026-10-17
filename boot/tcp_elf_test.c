#include "capmix/kernel.h"
#include "capmix/tcp.h"
#include "capmix/boot.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/tcp_elf.inc"
#define CHECK(x) do{if(!(x)){kerr("tcp ELF line %d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
int cm_tcp_elf_selftest(void)
{
    int failed=0,background=0;cm_file_t *file=NULL;cm_id_t pid=0;cm_proc_t *p=NULL;
    unsigned live_before=cm_tcp_live();uint64_t free_before=cm_mm_free_pages();
    cm_iface_t *nic=cm_net_primary_iface();
    if(!nic)kinfo("tcp ELF wire SKIP: no link-up NIC; exercising no-NIC syscall/exit/reclaim path");
    if(nic && !cm_boot_has_option("capmix.tcp_echo=1")) {
        kinfo("tcp ELF echo skipped: enable capmix.tcp_echo=1 + host echo");return 0;
    }
    CHECK(cm_vfs_open("/bin/tcpwire.elf",CM_O_WRONLY|CM_O_CREAT|CM_O_TRUNC,&file)==CM_OK);
    CHECK(cm_vfs_write(file,cm_tcp_elf,sizeof(cm_tcp_elf))==(int64_t)sizeof(cm_tcp_elf));
    CHECK(cm_vfs_fsync(file)==CM_OK);cm_vfs_close(file);file=NULL;
    free_before=cm_mm_free_pages(); /* fixture/cache pages persist intentionally */
    if(nic){CHECK(cm_net_bg_start()==CM_OK);background=1;}
    cm_spawn_attr_t attr={.path="/bin/tcpwire.elf",.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&pid)==CM_OK);p=cm_proc_lookup(pid);CHECK(p && p->elf);
    *(uint64_t *)cm_elf_pointer(p->elf,0x401010)=cm_tsc_hz()*3;
    int original=-1,readonly=-1;
    CHECK(cm_tcp_open(p->task,&original)==CM_OK);
    CHECK(cm_handle_duplicate(p->task,original,p->task,&readonly,CM_RIGHT_READ)==CM_OK);
    CHECK(cm_handle_drop(p->task,original)==CM_OK);
    *(uint32_t *)cm_elf_pointer(p->elf,0x401030)=(uint32_t)readonly;
#ifdef CAPMIX_HOSTED
    /* Exercise image-backed copies and real dispatcher without pretending CPL=3. */
    cm_sched_set_current(p->main);
    int64_t hid=cm_syscall(CM_SYS_TCP_OPEN,0,0,0,0);
    CHECK(hid>0);
    CHECK(cm_syscall(CM_SYS_TCP_CONNECT,(uint64_t)hid,UINT64_MAX,0,0)==CM_ERR_INVAL);
    CHECK(cm_syscall(CM_SYS_TCP_LISTEN,(uint64_t)hid,UINT64_MAX,0,0)==CM_ERR_INVAL);
    CHECK(cm_syscall(CM_SYS_TCP_ACCEPT,(uint64_t)readonly,0,0,0)==CM_ERR_DENIED);
    CHECK(cm_syscall(CM_SYS_TCP_ACCEPT,(uint64_t)hid,0,0,0)==CM_ERR_INVAL);
    CHECK(cm_syscall(CM_SYS_TCP_ACCEPT,(uint64_t)hid,1,0,0)==CM_ERR_INVAL);
    CHECK(cm_syscall(CM_SYS_TCP_SEND,(uint64_t)readonly,0x402000,1,100)==CM_ERR_DENIED);
    CHECK(cm_syscall(CM_SYS_TCP_SEND,(uint64_t)hid,0x402000,4097,100)==CM_ERR_INVAL);
    CHECK(cm_syscall(CM_SYS_TCP_RECV,(uint64_t)hid,0x400000,1,0)==CM_ERR_INVAL);
    CHECK(cm_syscall(CM_SYS_TCP_CLOSE,(uint64_t)hid,100,0,0)==CM_OK);
    cm_sched_set_current(NULL);
#endif
#ifndef CAPMIX_HOSTED
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*15;
#endif
    for(unsigned step=0;step<1000000 && (!p->exited || (p->main && p->main->id));step++) {
#ifndef CAPMIX_HOSTED
        if(cm_rdtsc()>=end)break;
#endif
        (void)cm_sched_run_until_idle(1);
    }
#ifndef CAPMIX_HOSTED
    CHECK(p->exited && p->elf_started && p->exit_code==0);
    CHECK(*(uint32_t *)cm_elf_pointer(p->elf,0x401018)==3);
    CHECK(*(uint32_t *)cm_elf_pointer(p->elf,0x401034)==1);
    uint32_t report=*(uint32_t *)cm_elf_pointer(p->elf,0x40101c);
    CHECK(report==(nic?1u:2u));
    if(nic) {
        CHECK(*(uint32_t *)cm_elf_pointer(p->elf,0x401020)==2049);
        CHECK(*(uint32_t *)cm_elf_pointer(p->elf,0x401024)==2049);
        kinfo("tcp ELF syscall verified: CPL=3 send/recv=2049 invalid-pointer/readonly-right/typed-cap close/exit=0");
    } else kinfo("tcp ELF no NIC verified: connect=NOENT skip exit=0 rights denied");
#else
    CHECK(p->exited && !p->elf_started && p->exit_code==CM_ERR_NOTSUP);
    cm_tcp_process_exit(p->task);
    kinfo("tcp ELF hosted: syscall ABI/copyin/out/rights verified; CPL=3 execution skipped");
#endif
    CHECK(cm_tcp_live()==live_before);
    CHECK(cm_proc_reap_elf(pid)==CM_OK);pid=0;
cleanup:
#ifdef CAPMIX_HOSTED
    cm_sched_set_current(NULL);
#endif
    if(file)cm_vfs_close(file);
    if(pid && p) {
        cm_tcp_process_exit(p->task);
        if(p->main && p->main->id){p->exited=1;p->main->state=CM_TH_DEAD;cm_thread_reap(p->main);}
        (void)cm_proc_reap_elf(pid);
    }
    if(background)cm_net_bg_stop();
    if(cm_tcp_live()!=live_before || cm_mm_free_pages()!=free_before){kerr("tcp ELF lifecycle: live=%u/%u free=%llu/%llu",cm_tcp_live(),live_before,(unsigned long)cm_mm_free_pages(),(unsigned long)free_before);failed++;}
    kinfo("tcp ELF selftest: %s (%d failures), bounce/image pages reclaimed",failed?"FAIL":"PASS",failed);return failed;
}
