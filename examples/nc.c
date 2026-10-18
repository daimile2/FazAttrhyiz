#include "cm_resolve.h"
/* One foreground interactive stream. No signals, listen, pipes or TLS. */
int main(int argc,char **argv,char **envp)
{
    (void)envp;uint32_t ip;uint16_t port;
    if(argc!=3 || !cm_parse_port(argv[2],&port)) {
        cm_print("usage: run -i /bin/nc.elf HOST PORT; /quit closes\n");return 2;
    }
    int resolved=cm_resolve_ipv4(argv[1],&ip);
    if(resolved){cm_print(cm_hosts_error(resolved));return 2;}
    long e=cm_console_probe();if(e<0){cm_print("nc: console denied; use foreground run -i\n");return 3;}
    uint64_t hz=cm_hz();if(!hz){cm_print("nc: clock unavailable\n");return 1;}
    long h=cm_tcp_dial(ip,port);if(h<0){cm_report("nc connect error",h);return 1;}
    cm_print("nc connected: console+TCP; /quit closes; idle-timeout=30s\n");
    char line[256],packet[256];unsigned used=0;int status=0;uint64_t last=cm_now();
    for(;;) {
        long n=cm_tcp_read(h,packet,sizeof(packet));
        if(n>0){if(cm_console_write(packet,(size_t)n)<0){status=1;break;}
            cm_report("nc rx bytes",n);last=cm_now();}
        else if(n==0){cm_print("nc peer EOF\n");break;}
        else if(n!=CM_EAGAIN){cm_report("nc recv error",n);status=1;break;}
        char ch;e=cm_console_read(&ch);
        if(!e) {
            last=cm_now();if(ch=='\r')ch='\n';
            if(ch=='\b'){if(used){used--;(void)cm_console_write("\b \b",3);}continue;}
            if(ch!='\n' && ((unsigned char)ch<32 || (unsigned char)ch>126))continue;
            if(ch=='\n') {
                line[used]=0;(void)cm_console_write("\n",1);
                if(cm_equal(line,"/quit"))break;
                line[used++]='\n';e=cm_tcp_write(h,line,used);if(e<0){cm_report("nc send error",e);status=1;break;}
                cm_report("nc tx bytes",used);used=0;
            } else if(used<sizeof(line)-2){line[used++]=ch;(void)cm_console_write(&ch,1);}
        } else if(e!=CM_EAGAIN){cm_report("nc console error",e);status=1;break;}
        if(cm_now()-last>=hz*30){cm_print("nc idle timeout\n");status=124;break;}
        cm_yield();
    }
    e=cm_tcp_hangup(h);if(e<0){cm_report("nc close error",e);status=1;}
    cm_print("nc closed\n");return status;
}
