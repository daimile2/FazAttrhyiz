#ifndef CM_HOSTS_H
#define CM_HOSTS_H
#include <stddef.h>
#include <stdint.h>
/* Shared, allocation-free hosts-only resolver. Reader: bytes, 0 EOF, <0 error.
 * Open/close and capability authorization belong to the caller. */
#define CM_HOSTS_NAME_MAX 253u
#define CM_HOSTS_LINE_MAX 1024u
#define CM_HOSTS_FILE_MAX 16384u
#define CM_HOSTS_NOTFOUND (-1)
#define CM_HOSTS_INVALID (-2)
#define CM_HOSTS_IO (-3)
#define CM_HOSTS_LIMIT (-4)
typedef long (*cm_hosts_reader)(void *, void *, size_t);
static inline int cm_hosts_ipv4(const char *s,uint32_t *out)
{
    uint32_t ip=0;
    for(unsigned i=0;i<4;i++) {
        unsigned v=0,n=0;
        while(*s>='0' && *s<='9'){v=v*10+(unsigned)(*s++-'0');if(++n>3 || v>255)return 0;}
        if(!n)return 0;
        ip=(ip<<8)|v;
        if(i<3){if(*s++!='.')return 0;}else if(*s)return 0;
    }
    /* Match native dial's existing unicast-address policy. */
    if(!ip || ip==UINT32_MAX)return 0;
    *out=ip;return 1;
}
static inline unsigned cm_hosts_fold(unsigned c){return c>='A' && c<='Z'?c+32:c;}
static inline int cm_hosts_name(const char *s)
{
    unsigned n=0;
    if(!s || !*s)return 0;
    for(;*s;s++) {
        unsigned c=(unsigned char)*s;
        if(++n>CM_HOSTS_NAME_MAX || !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
           (c>='0'&&c<='9')||c=='-'||c=='.'||c=='_'))return 0;
    }
    return 1;
}
static inline int cm_hosts_space(char c){return c==' '||c=='\t'||c=='\r';}
static inline int cm_hosts_line(char *line,const char *name,uint32_t *out)
{
    char *p=line;
    while(cm_hosts_space(*p))p++;
    char *addr=p;
    while(*p && !cm_hosts_space(*p) && *p!='#')p++;
    if(!*p || *p=='#')return 0;
    *p++=0;uint32_t ip;
    if(!cm_hosts_ipv4(addr,&ip))return 0;
    while(*p) {
        while(cm_hosts_space(*p))p++;
        if(!*p || *p=='#')break;
        char *token=p;while(*p && !cm_hosts_space(*p) && *p!='#')p++;
        char end=*p;*p=0;const char *a=token,*b=name;
        while(*a && *b && cm_hosts_fold((unsigned char)*a)==cm_hosts_fold((unsigned char)*b)){a++;b++;}
        if(!*a && !*b){*out=ip;return 1;}
        if(!end || end=='#')break;
        p++;
    }
    return 0;
}
static inline int cm_hosts_resolve(const char *name,uint32_t *out,cm_hosts_reader read,void *ctx)
{
    if(!out || !cm_hosts_name(name))return CM_HOSTS_INVALID;
    /* Hosts take precedence, including a numeric-looking alias. Stream in bounded chunks. */
    char line[CM_HOSTS_LINE_MAX+1],chunk[128];unsigned used=0,total=0;
    if(read)for(;;) {
        long n=read(ctx,chunk,sizeof(chunk));
        if(n<0 || n>(long)sizeof(chunk))return CM_HOSTS_IO;
        if(!n){line[used]=0;if(cm_hosts_line(line,name,out))return 0;break;}
        for(long i=0;i<n;i++) {
            if(++total>CM_HOSTS_FILE_MAX)return CM_HOSTS_LIMIT;
            char c=chunk[i];
            if(!c)return CM_HOSTS_INVALID;
            if(c=='\n'){line[used]=0;if(cm_hosts_line(line,name,out))return 0;used=0;}
            else {if(used==CM_HOSTS_LINE_MAX)return CM_HOSTS_LIMIT;line[used++]=c;}
        }
    }
    return cm_hosts_ipv4(name,out)?0:CM_HOSTS_NOTFOUND;
}
static inline const char *cm_hosts_error(int e)
{
    return e==CM_HOSTS_NOTFOUND?"resolve: name not found (hosts only)\n":e==CM_HOSTS_INVALID?
        "resolve: invalid name/hosts data\n":e==CM_HOSTS_LIMIT?"resolve: hosts size limit\n": "resolve: hosts I/O denied/error\n";
}
#endif
