#include "capmix/kernel.h"
#include "capmix/boot.h"
#include "capmix/linux_socket.h"
#include "capmix/linux_uapi.h"
#include "capmix/uaccess.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include "capmix/tcp.h"
#include "../build/linux_l3.inc"
#include <string.h>
#define CHECK(x) do{if(!(x)){kerr("linux L3 line=%d: %s",__LINE__,#x);failed++;goto cleanup;}}while(0)
#define SCRATCH 0x7fc000ull
static int stage(void)
{
    cm_file_t *f=NULL;int e=cm_vfs_open("/bin/linux-net.elf",CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f);if(e)return e;
    if(cm_vfs_write(f,cm_linux_l3_elf,sizeof(cm_linux_l3_elf))!=sizeof(cm_linux_l3_elf))e=CM_ERR_IO;
    if(!e)e=cm_vfs_fsync(f);
    cm_vfs_close(f);return e;
}
static int unit(void)
{
    int failed=0;cm_id_t pid=0,child_id=0;cm_proc_t *p=NULL,*child=NULL;cm_object_t *o=NULL;
    unsigned sockets=cm_linux_socket_live(),tcp=cm_tcp_live(),files=cm_vfs_open_files();
    uint64_t pages=cm_mm_free_pages()+cm_file_cache_pages(),committed,limit;cm_commit_stats(&committed,&limit);
    cm_spawn_attr_t attr={.path="/bin/linux-net.elf",.qos=CM_QOS_USER_INTERACTIVE};
    uint64_t irq=cm_irq_save();
    CHECK(cm_posix_spawn(&attr,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    cm_sched_set_current(p->main);
    CHECK(cm_syscall(41,1,1,0,0)==-97);
    int64_t fd=cm_syscall(41,2,2|0x800,0,0);CHECK(fd>=3);
    cm_linux_sockaddr_in_t a={.family=2,.port={0x4a,0x93},.address={127,0,0,1}}; /* 19091 */
    CHECK(cm_copyout(p->elf,SCRATCH,&a,16)==0);
    CHECK(cm_syscall(49,(uint64_t)fd,UINT64_MAX,16,0)==-14);
    CHECK(cm_syscall(49,(uint64_t)fd,SCRATCH,16,0)==0);
    CHECK(cm_syscall(8,(uint64_t)fd,0,0,0)==-29);
    CHECK(cm_syscall(5,(uint64_t)fd,SCRATCH+128,0,0)==0);
    CHECK((((cm_linux_stat_t *)cm_elf_pointer(p->elf,SCRATCH+128))->mode&0170000)==0140000);
    CHECK(cm_handle_acquire(p->task,p->fd[fd],CM_OBJ_LINUX_SOCKET,CM_RIGHT_READ,&o)==0);
    int restricted=cm_handle_install(p->task,o,CM_RIGHT_READ|CM_RIGHT_DUPLICATE);cm_obj_release(o);o=NULL;CHECK(restricted>0);p->fd[10]=restricted;
    static const char payload[]="linux L3 UDP alias payload";
    CHECK(cm_copyout(p->elf,SCRATCH+512,payload,sizeof(payload))==0);
    CHECK(cm_syscall(1,10,SCRATCH+512,sizeof(payload),0)==-13);
    CHECK(cm_syscall(33,10,11,0,0)==11 && cm_syscall(1,11,SCRATCH+512,sizeof(payload),0)==-13);
    CHECK(cm_syscall(3,10,0,0,0)==0 && cm_syscall(3,11,0,0,0)==0);
    CHECK(cm_syscall(33,(uint64_t)fd,20,0,0)==20 && cm_syscall(3,(uint64_t)fd,0,0,0)==0);fd=20;
    /* Fork -> child exit must not unbind or destroy the parent's endpoint. */
    cm_linux_regs_t frame={0};p->linux_frame=&frame;
    int64_t forked=cm_linux_fork();CHECK(forked>0);child_id=(cm_id_t)forked;child=cm_proc_lookup(child_id);CHECK(child);
    cm_proc_finish_exit(child,0);cm_thread_reap(child->main);CHECK(cm_proc_release_child(pid,child_id)==0);child=NULL;child_id=0;p->linux_frame=NULL;
    p->linux_regs.r8=SCRATCH;p->linux_regs.r9=16;
    CHECK(cm_syscall(44,(uint64_t)fd,SCRATCH+512,sizeof(payload),0)==sizeof(payload));
    CHECK(cm_syscall(0,(uint64_t)fd,1,sizeof(payload),0)==-14);
    uint32_t size=16;CHECK(cm_copyout(p->elf,SCRATCH+32,&size,4)==0);
    p->linux_regs.r8=SCRATCH+64;p->linux_regs.r9=SCRATCH+32;
    CHECK(cm_syscall(45,(uint64_t)fd,SCRATCH+768,1024,0x40)==sizeof(payload));
    CHECK(!memcmp(cm_elf_pointer(p->elf,SCRATCH+768),payload,sizeof(payload)) && !memcmp(cm_elf_pointer(p->elf,SCRATCH+64),&a,8));
    CHECK(cm_syscall(0,(uint64_t)fd,SCRATCH+768,1,0)==-11);
    CHECK(cm_syscall(42,(uint64_t)fd,SCRATCH,16,0)==0);
    CHECK(cm_syscall(1,(uint64_t)fd,SCRATCH+512,sizeof(payload),0)==sizeof(payload));
    CHECK(cm_syscall(0,(uint64_t)fd,SCRATCH+768,1024,0)==sizeof(payload));
    CHECK(cm_syscall(54,(uint64_t)fd,1,2,0)==-38 && cm_syscall(55,(uint64_t)fd,1,2,0)==-38 && p->linux_enosys==2);
    int64_t handles[CM_TCP_CONNECTIONS];int native_fill[CM_TCP_CONNECTIONS];unsigned count=0,native_count=0,fd_room=0;
    /* Global capacity now exceeds one Linux process's fd budget. Keep one fd
     * free to distinguish backend ENOBUFS from per-process EMFILE. */
    for(unsigned i=0;i<CM_FD_MAX;i++)fd_room+=p->fd[i]<0;
    CHECK(fd_room>2);
    unsigned linux_count=CM_MIN(CM_TCP_CONNECTIONS-tcp,fd_room-1);
    for(;count<linux_count;count++){handles[count]=cm_syscall(41,2,1|0x800,0,0);CHECK(handles[count]>=0);}
    while(tcp+count+native_count<CM_TCP_CONNECTIONS){CHECK(!cm_tcp_open(p->task,&native_fill[native_count]));native_count++;}
    CHECK(cm_syscall(41,2,1,0,0)==-105);
    cm_iface_t *nic=NULL;
    for(unsigned i=0;i<2;i++){cm_iface_t *n=cm_if_get(cm_if_find(i?"enet0":"vnet0"));if(n && n->up){nic=n;break;}}
    if(nic && count>1) {
        cm_linux_sockaddr_in_t local={.family=2,.port={0x46,0xaa}}; /* 18090 */
        CHECK(cm_copyout(p->elf,SCRATCH+256,&local,16)==0);
        CHECK(cm_syscall(49,(uint64_t)handles[0],SCRATCH+256,16,0)==0);
        CHECK(cm_syscall(49,(uint64_t)handles[1],SCRATCH+256,16,0)==-98);
        CHECK(cm_syscall(50,(uint64_t)handles[0],100,0,0)==0);
        CHECK(cm_syscall(43,(uint64_t)handles[0],0,0,0)==-11);
    }
    for(unsigned i=0;i<count;i++)CHECK(cm_syscall(3,(uint64_t)handles[i],0,0,0)==0);
    for(unsigned i=0;i<native_count;i++)CHECK(!cm_handle_drop(p->task,native_fill[i]));
    kinfo("linux TCP capacity verified: backend=%u linux_fds=%u native_fill=%u ENOBUFS distinct from EMFILE",CM_TCP_CONNECTIONS,count,native_count);
    /* A full fd table must reject before consuming a network slot. */
    for(unsigned i=3;i<CM_FD_MAX;i++)if(p->fd[i]<0)CHECK(cm_syscall(33,(uint64_t)fd,i,0,0)==i);
    CHECK(cm_syscall(41,2,1,0,0)==-24);
    kinfo("linux L3 fd verified: typed rights/copy faults/dup/fork exit isolation/UDP payload; ENOSYS=2 observed; fd/backend capacity bounded");
cleanup:
    if(o)cm_obj_release(o);
    if(child){cm_proc_finish_exit(child,137);cm_thread_reap(child->main);if(cm_proc_release_child(pid,child_id))failed++;}
    cm_sched_set_current(NULL);
    if(p){p->linux_frame=NULL;cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
    cm_irq_restore(irq);
    uint64_t after;cm_commit_stats(&after,&limit);
    if(after!=committed || cm_mm_free_pages()+cm_file_cache_pages()!=pages || cm_linux_socket_live()!=sockets || cm_tcp_live()!=tcp || cm_vfs_open_files()!=files){kerr("linux L3 unit reclaim mismatch");failed++;}
    return failed;
}
#ifndef CAPMIX_HOSTED
static int run(const char *mode)
{
    int failed=0;cm_id_t pid=0;cm_proc_t *p=NULL;
    uint64_t before,limit,pages=cm_mm_free_pages()+cm_file_cache_pages();cm_commit_stats(&before,&limit);
    unsigned sockets=cm_linux_socket_live(),tcp=cm_tcp_live(),files=cm_vfs_open_files();
    const char *argv[]={"/bin/linux-net.elf",mode,NULL};
    cm_spawn_attr_t attr={.path=argv[0],.argv=argv,.qos=CM_QOS_USER_INTERACTIVE};
    CHECK(cm_posix_spawn(&attr,&pid)==0);p=cm_proc_lookup(pid);CHECK(p);
    uint64_t end=cm_rdtsc()+cm_tsc_hz()*12;
    while((!p->exited || p->main->id) && cm_rdtsc()<end)(void)cm_sched_run_until_idle(1);
    CHECK(p->exited && !p->main->id && p->exit_code==0 && p->linux_enosys==0);
    CHECK(cm_proc_release_elf(pid)==0);pid=0;p=NULL;
    uint64_t after;cm_commit_stats(&after,&limit);
    CHECK(after==before && cm_mm_free_pages()+cm_file_cache_pages()==pages && cm_linux_socket_live()==sockets && cm_tcp_live()==tcp && cm_vfs_open_files()==files);
    kinfo("linux L3 CPL3 %s verified: exit=0 ENOSYS=0 socket/commit/pages reclaimed",mode);
cleanup:
    if(p){cm_proc_finish_exit(p,137);if(p->main && p->main->id)cm_thread_reap(p->main);if(cm_proc_release_elf(pid))failed++;}
    return failed;
}
#endif
int cm_linux_l3_selftest(void)
{
    if(!cm_linux_enabled()){kinfo("linux L3 skipped: persona disabled; native networking retained");return 0;}
    int failed=0,background=0;
    CHECK(stage()==0);failed+=unit();
#ifndef CAPMIX_HOSTED
    failed+=run("udp");
    cm_iface_t *n=NULL;for(unsigned i=0;i<2;i++){cm_iface_t *x=cm_if_get(cm_if_find(i?"enet0":"vnet0"));if(x && x->up){n=x;break;}}
    if(n && cm_boot_has_option("capmix.tcp_echo=1")){CHECK(cm_net_bg_start()==CM_OK);background=1;failed+=run("echo");failed+=run("inherit");}
    else if(!n)failed+=run("nonet");
    else kinfo("linux L3 wire TCP skipped: start host echo and use capmix.tcp_echo=1");
#else
    kinfo("linux L3 hosted: fd/cap/UDP/lifecycle verified; CPL3 and NIC wire execution skipped");
#endif
cleanup:
    if(background)cm_net_bg_stop();
    kinfo("linux L3 selftest: %s (%d failures)",failed?"FAIL":"PASS",failed);return failed;
}
