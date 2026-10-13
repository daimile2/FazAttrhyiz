#include "runtime.h"
int main(int argc,char **argv,char **envp)
{
    (void)argc;(void)argv;(void)envp;
    uputs("init ELF: pid=1 CPL=");unum(ucpl());uputs("\n");
    cm_user_boot_t info;long e=ucall(36,uboot,0,(uintptr_t)&info,0);
    if(e){uputs("init root mount failed=");unum(e);uputs("\n");return 1;}
    e=ucall(36,uboot,1,(uintptr_t)&info,0);
    if(e){uputs("init network config failed=");unum(e);uputs(" (continuing)\n");}
    uputs("init: CapMix product root=");uputs(info.disk?"disk":"ram");uputs(" backend=");uputs(info.backend);
    uputs(" nic=");uputs(info.nic[0]?info.nic:"none");uputs(info.nic_up?"/up\n":"/down\n");
    const char *args[]={"/bin/shell.elf",NULL};
    const char *env[]={"PATH=/bin","CAPMIX=product",NULL};
    cm_user_child_t shell;e=uspawn(args[0],args,env,CM_SPAWN_SHELL,&shell);
    if(e){uputs("init shell spawn failed=");unum(e);uputs("\n");return 1;}
    uputs("init: shell pid=");unum(shell.pid);uputs(" user ELF launched\n");
    cm_user_wait_t exit;e=uwait(shell.handle,&exit,0);
    uputs("init: shell reaped status=");unum(e?e:exit.exit_code);uputs("\n");
    return e?1:(int)exit.exit_code;
}
