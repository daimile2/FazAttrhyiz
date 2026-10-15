#include "capmix/control.h"
#include "capmix/kernel.h"
#include "capmix/boot.h"
#include "capmix/uaccess.h"
#include "capmix/console.h"
#include "capmix/power.h"
#include "capmix/dhcp.h"
#include "capmix/fs.h"
#include "capmix/io.h"
#include <string.h>
extern const cm_fs_ops_t *cm_cowfs_ops(void);
static unsigned cpu_next,root_finalized;
static int equal(const char *a,const char *b){size_t n=strlen(a);return n==strlen(b) && !memcmp(a,b,n);}
static void text_copy(char *dst,size_t size,const char *src){size_t n=strlen(src);if(n>=size)n=size-1;memcpy(dst,src,n);dst[n]=0;}
static const char console_tag,control_tag,boot_tag;
struct child {cm_id_t owner,pid;};
static void child_destroy(void *p){cm_page_free(p,0);}
static int mint(cm_proc_t *p,cm_obj_type_t type,const void *tag,cm_rights_t rights)
{
    cm_object_t *o=cm_obj_mint(type,(void *)tag,rights);if(!o)return CM_ERR_NOSPC;
    int h=cm_handle_install(p->task,o,rights);cm_obj_release(o);return h;
}
int cm_control_seed(cm_proc_t *p)
{
    if(!p || !p->elf)return CM_ERR_INVAL;
    int a=mint(p,CM_OBJ_CONTROL,&control_tag,CM_RIGHT_EXEC|CM_RIGHT_WAIT|CM_RIGHT_DUPLICATE);
    int b=mint(p,CM_OBJ_CONSOLE,&console_tag,CM_RIGHT_READ|CM_RIGHT_WRITE|CM_RIGHT_DUPLICATE);
    int c=mint(p,CM_OBJ_BOOT,&boot_tag,CM_RIGHT_MAP|CM_RIGHT_WRITE|CM_RIGHT_DESTROY);
    if(a!=1 || b!=2 || c!=3){cm_control_process_exit(p);return CM_ERR_NOSPC;}
    return CM_OK;
}
static cm_error_t acquire(cm_proc_t *p,uint64_t h,cm_obj_type_t type,cm_rights_t right,cm_object_t **o)
{
    if(h>=CM_HANDLES_PER_TASK)return CM_ERR_DENIED;
    cm_error_t e=cm_handle_acquire(p->task,(int)h,type,right,o);
    if(e)return CM_ERR_DENIED;
    const void *tag=type==CM_OBJ_CONSOLE?&console_tag:type==CM_OBJ_CONTROL?&control_tag:&boot_tag;
    if(type!=CM_OBJ_CHILD && (*o)->payload!=tag){cm_obj_release(*o);return CM_ERR_DENIED;}
    return CM_OK;
}
static cm_error_t vector(cm_proc_t *p,uint64_t address,char strings[][256],const char **out)
{
    if(!address){out[0]=NULL;return CM_OK;}
    for(unsigned i=0;i<32;i++) {
        uint64_t ptr;if(address>UINT64_MAX-i*8 || cm_copyin(p->elf,&ptr,address+i*8,8))return CM_ERR_INVAL;
        if(!ptr){out[i]=NULL;return CM_OK;}
        unsigned j=0;
        for(;j<256;j++){if(ptr>UINT64_MAX-j || cm_copyin(p->elf,&strings[i][j],ptr+j,1))return CM_ERR_INVAL;if(!strings[i][j])break;}
        if(j==256)return CM_ERR_NOSPC;
        out[i]=strings[i];
    }
    uint64_t end;
    if(address>UINT64_MAX-256 || cm_copyin(p->elf,&end,address+256,8) || end)return CM_ERR_NOSPC;
    out[32]=NULL;return CM_OK;
}
static int64_t spawn(cm_proc_t *p,uint64_t hid,uint64_t address,uint64_t output,uint64_t io_address)
{
    cm_object_t *authority=NULL,*console=NULL,*stdio[2]={NULL,NULL};int e=acquire(p,hid,CM_OBJ_CONTROL,CM_RIGHT_EXEC,&authority);if(e)return e;
    cm_user_spawn_t req;char path[128];
    if(cm_copyin(p->elf,&req,address,sizeof(req)) || cm_copyinstr(p->elf,path,req.path,sizeof(path)) ||
       !cm_user_access(p->elf,output,sizeof(cm_user_child_t),1)){e=CM_ERR_INVAL;goto finish;}
    if(cm_cpu_id()!=0 || (req.flags!=0 && req.flags!=CM_SPAWN_SHELL && req.flags!=CM_SPAWN_CONSOLE) || req.cpu< -1 || req.cpu>=cm_cpu_count()){e=CM_ERR_INVAL;goto finish;}
    int shell=req.flags==CM_SPAWN_SHELL,interactive=req.flags==CM_SPAWN_CONSOLE;
    if(interactive) {
        e=acquire(p,2,CM_OBJ_CONSOLE,CM_RIGHT_READ|CM_RIGHT_WRITE|CM_RIGHT_DUPLICATE,&console);
        if(e)goto finish;
    }
    if(shell && (p->pid!=1 || !equal(path,"/bin/shell.elf") ||
       !(cm_handle_lookup(p->task,(int)hid)->rights&CM_RIGHT_DUPLICATE))){e=CM_ERR_DENIED;goto finish;}
    if(io_address) {
        cm_user_stdio_t io;
        if(req.flags || cm_copyin(p->elf,&io,io_address,sizeof(io)) ||
           io.size!=sizeof(io) || io.flags){e=CM_ERR_INVAL;goto finish;}
        int fds[2]={io.input_fd,io.output_fd};
        for(unsigned i=0;i<2;i++) {
            if(fds[i]==-1)continue;
            if(fds[i]<3 || fds[i]>=CM_FD_MAX){e=CM_ERR_INVAL;goto finish;}
            e=cm_handle_acquire(p->task,cm_fdio_lookup(p,fds[i]),CM_OBJ_FILE,
                CM_RIGHT_DUPLICATE|(i?CM_RIGHT_WRITE:CM_RIGHT_READ),&stdio[i]);
            if(e)goto finish;
        }
    }
    /* The snapshot is private kernel memory; never pass nested user pointers to spawn. */
    char (*strings)[256]=cm_page_alloc(3);if(!strings){e=CM_ERR_NOMEM;goto finish;}
    const char *argv[33],*envp[33];
    e=vector(p,req.argv,strings,argv);if(!e)e=vector(p,req.envp,strings+32,envp);
    if(e){cm_page_free(strings,3);goto finish;}
    struct child *record=cm_page_alloc(0);if(!record){e=CM_ERR_NOMEM;cm_page_free(strings,3);goto finish;}
    memset(record,0,CM_PAGE_SIZE);record->owner=p->pid;
    cm_object_t *obj=cm_obj_mint_owned(CM_OBJ_CHILD,record,CM_RIGHT_WAIT|CM_RIGHT_SIGNAL,child_destroy);
    if(!obj){cm_page_free(record,0);cm_page_free(strings,3);e=CM_ERR_NOSPC;goto finish;}
    int handle=cm_handle_install(p->task,obj,CM_RIGHT_WAIT|CM_RIGHT_SIGNAL);cm_obj_release(obj);
    if(handle<0){cm_page_free(strings,3);e=handle;goto finish;}
    cm_spawn_attr_t attr={.path=path,.argv=argv,.envp=envp,.qos=CM_QOS_USER_INTERACTIVE,
        .cpu_set=1,.cpu=(shell||interactive)?0:req.cpu<0?(int)(cpu_next%(unsigned)cm_cpu_count()):req.cpu,
        .stdio={stdio[0],stdio[1]}};
    cm_id_t pid=0;e=cm_posix_spawn(&attr,&pid);cm_page_free(strings,3);
    if(e){cm_handle_drop(p->task,handle);goto finish;}
    record->pid=pid;cm_proc_t *child=cm_proc_lookup(pid);child->parent_pid=p->pid;
    if(shell || interactive) {
        /* Both are pinned BSP: grant/rollback completes before child executes. */
        int granted;
        if(shell) {
            int c=mint(child,CM_OBJ_CONTROL,&control_tag,CM_RIGHT_EXEC|CM_RIGHT_WAIT);
            int t=mint(child,CM_OBJ_CONSOLE,&console_tag,CM_RIGHT_READ|CM_RIGHT_WRITE|CM_RIGHT_DUPLICATE);
            int b=mint(child,CM_OBJ_BOOT,&boot_tag,CM_RIGHT_DESTROY);
            granted=c==1 && t==2 && b==3;
        } else {
            int tty=-1;
            e=cm_handle_duplicate(p->task,2,child->task,&tty,CM_RIGHT_READ|CM_RIGHT_WRITE);
            granted=!e && tty==1;
        }
        if(!granted) {
            child->exited=1;child->exit_code=CM_ERR_NOSPC;child->main->state=CM_TH_DEAD;
            cm_thread_reap(child->main);cm_control_process_exit(child);
            (void)cm_proc_release_child(p->pid,pid);cm_handle_drop(p->task,handle);e=CM_ERR_NOSPC;goto finish;
        }
        if(interactive)kinfo("user console delegated: parent=%llu child=%llu handle=1 rights=READ/WRITE cpu=0",(unsigned long)p->pid,(unsigned long)pid);
    } else cpu_next++;
    cm_user_child_t out={.pid=pid,.handle=handle,.cpu=attr.cpu};
    e=cm_copyout(p->elf,output,&out,sizeof(out));
    kinfo("user spawn: parent=%llu child=%llu cpu=%d child-cap=%d",(unsigned long)p->pid,(unsigned long)pid,attr.cpu,handle);
finish:for(unsigned i=0;i<2;i++)if(stdio[i])cm_obj_release(stdio[i]);
    if(console)cm_obj_release(console);
    cm_obj_release(authority);return e;
}
static int child_done(cm_proc_t *p)
{
    return p && __atomic_load_n(&p->exited,__ATOMIC_ACQUIRE) &&
        (!p->main || __atomic_load_n(&p->main->id,__ATOMIC_ACQUIRE)!=p->elf_thread_id);
}
static int64_t wait_child(cm_proc_t *p,uint64_t hid,uint64_t address,uint64_t flags)
{
    cm_object_t *o=NULL;int e=acquire(p,hid,CM_OBJ_CHILD,CM_RIGHT_WAIT,&o);if(e)return e;
    struct child *r=o->payload;cm_proc_t *child=cm_proc_lookup(r->pid);
    if(r->owner!=p->pid || !child || child->parent_pid!=p->pid){e=CM_ERR_DENIED;goto finish;}
    if(flags&~CM_WAIT_NONBLOCK || !cm_user_access(p->elf,address,sizeof(cm_user_wait_t),1)){e=CM_ERR_INVAL;goto finish;}
    while(!child_done(child)) {
        if(flags&CM_WAIT_NONBLOCK){e=CM_ERR_AGAIN;goto finish;}
        cm_sched_yield();
    }
    cm_user_wait_t result={.pid=child->pid,.exit_code=child->exit_code,.preempts=child->elf_preempts,.cpu=child->elf_cpu};
    e=cm_proc_release_child(p->pid,r->pid);
    if(!e){e=cm_copyout(p->elf,address,&result,sizeof(result));cm_handle_drop(p->task,(int)hid);}
finish:cm_usermode_resume_entry();cm_obj_release(o);return e;
}
void cm_control_process_exit(cm_proc_t *p)
{
    if(!p || !p->task)return;
    for(int h=1;h<CM_HANDLES_PER_TASK;h++) {
        cm_handle_t *handle=cm_handle_lookup(p->task,h);
        if(handle && (handle->obj->type==CM_OBJ_CONSOLE || handle->obj->type==CM_OBJ_BOOT ||
                      handle->obj->type==CM_OBJ_CONTROL || handle->obj->type==CM_OBJ_CHILD))cm_handle_drop(p->task,h);
    }
}
int64_t cm_control_syscall(uint32_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
    cm_proc_t *p=cm_proc_current();if(!p || !p->elf)return CM_ERR_NOTSUP;
    if(nr==CM_SYS_SPAWN)return spawn(p,a,b,c,d);
    if(d)return CM_ERR_INVAL;
    if(nr==CM_SYS_CHILD_WAIT)return wait_child(p,a,b,c);
    if(nr==CM_SYS_TSC_HZ){if(a||b||c)return CM_ERR_INVAL;return (int64_t)cm_tsc_hz();}
    if(nr==CM_SYS_CHILD_KILL) {
        if(b||c)return CM_ERR_INVAL;
        cm_object_t *child=NULL;int e=acquire(p,a,CM_OBJ_CHILD,CM_RIGHT_SIGNAL,&child);if(e)return e;
        struct child *r=child->payload;
        e=r->owner==p->pid?cm_proc_request_kill(p->pid,r->pid):CM_ERR_DENIED;
        cm_obj_release(child);return e;
    }
    if(nr==CM_SYS_YIELD){if(a||b||c)return CM_ERR_INVAL;cm_sched_yield();cm_usermode_resume_entry();return CM_OK;}
    if(nr==CM_SYS_READDIR) {
        char path[128];cm_dirent_t out;
        if(b>UINT32_MAX || cm_copyinstr(p->elf,path,a,sizeof(path)) || !cm_user_access(p->elf,c,sizeof(out),1))return CM_ERR_INVAL;
        memset(&out,0,sizeof(out));int e=cm_vfs_readdir(path,(uint32_t)b,&out);return e?e:cm_copyout(p->elf,c,&out,sizeof(out));
    }
    cm_obj_type_t type=(nr==CM_SYS_CONSOLE_READ || nr==CM_SYS_CONSOLE_WRITE)?CM_OBJ_CONSOLE:CM_OBJ_BOOT;
    cm_rights_t right=nr==CM_SYS_CONSOLE_READ?CM_RIGHT_READ:nr==CM_SYS_CONSOLE_WRITE?CM_RIGHT_WRITE:
        nr==CM_SYS_POWEROFF?CM_RIGHT_DESTROY:b==0?CM_RIGHT_MAP:CM_RIGHT_WRITE;
    cm_object_t *o=NULL;int e=acquire(p,a,type,right,&o);if(e)return e;
    if(nr==CM_SYS_CONSOLE_READ) {
        char ch;if(c!=1 || !cm_user_access(p->elf,b,1,1))e=CM_ERR_INVAL;
        else {e=cm_ps2_read_char(&ch);if(!e)e=cm_copyout(p->elf,b,&ch,1);}
    } else if(nr==CM_SYS_CONSOLE_WRITE) {
        if(c>4096 || !cm_user_access(p->elf,b,c,0))e=CM_ERR_INVAL;
        else if(!cm_klog_lock())e=CM_ERR_AGAIN;
        else {char text[128];uint64_t done=0;while(done<c){uint64_t n=CM_MIN(c-done,sizeof(text));
            if(cm_copyin(p->elf,text,b+done,n)){e=CM_ERR_INVAL;break;}
            for(unsigned i=0;i<n;i++)cm_putchar(text[i]);
            done+=n;}cm_klog_unlock();if(!e)e=(int)c;}
    } else if(nr==CM_SYS_BOOT_CONTROL) {
        if(p->pid!=1 || cm_cpu_id()!=0 || b>1 || !cm_user_access(p->elf,c,sizeof(cm_user_boot_t),1))e=CM_ERR_INVAL;
        else if(b==0) {
            if(root_finalized)e=CM_ERR_EXIST;
            else if(cm_vfs_open_files())e=CM_ERR_BUSY;
            else {e=cm_vfs_sync();if(!e)e=cm_vfs_umount("/");if(!e)e=cm_cow_mount();
                if(!e)e=cm_vfs_mount("/",cm_cowfs_ops(),NULL);
                if(!e){root_finalized=1;kinfo("user init: root mount finalized backend=%s",cm_blk_backend_name());}}
        } else if(!root_finalized)e=CM_ERR_DENIED;
        else {cm_dhcp_boot_configure();cm_usermode_resume_entry();}
        if(!e){cm_iface_t *nic=cm_net_primary_iface();cm_user_boot_t out={.disk=cm_blk_is_disk(),.cpus=(uint32_t)cm_cpu_count(),.nic_up=nic&&nic->up,.version=1};
            text_copy(out.backend,sizeof(out.backend),cm_blk_backend_name());if(nic)text_copy(out.nic,sizeof(out.nic),nic->name);
            e=cm_copyout(p->elf,c,&out,sizeof(out));}
    } else if(nr==CM_SYS_POWEROFF) {
        if((b!=0 && b!=CM_POWEROFF_FORCE)||c)e=CM_ERR_INVAL;
        else if(cm_proc_children_pending(p->pid)) {
            if(!b)e=CM_ERR_BUSY;
            else {kwarn("user poweroff: timeout forced; live contexts retained; root sync skipped");cm_poweroff();}
        }
        else {e=cm_vfs_sync();if(!e){kinfo("user poweroff: capability authorized root sync=0");cm_poweroff();}}
    } else e=CM_ERR_NOTSUP;
    cm_obj_release(o);return e;
}
