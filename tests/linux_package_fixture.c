/* Integration fixture on a disposable, initialized installed disk only. */
#define main package_tool_main
#include "../tools/linux-package.c"
#undef main
static int contents(const char *path,const char *expected)
{
    char buf[128]={0};cm_file_t *f=NULL;int e=cm_vfs_open(path,CM_O_RDONLY,&f);if(e)return e;
    int64_t n=cm_vfs_read(f,buf,sizeof(buf));cm_vfs_close(f);
    return n==(int64_t)strlen(expected) && !memcmp(buf,expected,(size_t)n)?0:CM_ERR_IO;
}
static int seed(const char *path,const char *text)
{
    cm_file_t *f=NULL;int e=cm_vfs_open(path,CM_O_CREAT|CM_O_TRUNC|CM_O_WRONLY,&f);if(e)return e;
    if(cm_vfs_write(f,text,strlen(text))!=(int64_t)strlen(text))e=CM_ERR_IO;
    if(!e)e=cm_vfs_fsync(f);
    cm_vfs_close(f);return e;
}
int main(int argc,char **argv)
{
    if(argc!=3)return 1;
    diskfd=open(argv[1],O_RDWR|O_NOFOLLOW);unsigned char b[512];struct stat st;
    if(diskfd<0 || fstat(diskfd,&st) || !S_ISREG(st.st_mode) || st.st_nlink!=1 || st.st_size%512 || flock(diskfd,LOCK_EX|LOCK_NB))return 1;
    struct flock l={.l_type=F_WRLCK,.l_whence=SEEK_SET};if(fcntl(diskfd,F_SETLK,&l))return 1;
    int e=transfer(diskfd,b,512,0,0)?CM_ERR_IO:cm_disk_layout_parse(b,(uint64_t)st.st_size/512,&root);
    if(!e)e=cm_cow_mount();
    if(!e)e=cm_vfs_mount("/",cm_cowfs_ops(),NULL);
    if(!e && !strcmp(argv[2],"seed-deep")) {
        e=directory("/data");if(!e)e=directory("/data/linux");
        if(!e)e=seed("/data/linux/first","staged deletion must roll back");
        char path[128]="/data/linux";
        for(unsigned i=0;i<33 && !e;i++){strcat(path,"/d");e=directory(path);}
    } else if(!e && !strcmp(argv[2],"seed")) {
        e=directory("/data");if(!e)e=directory("/data/linux");
        if(!e)e=directory("/data/linux/nested");
        if(!e)e=seed("/data/linux/nested/note","nested-linux-data");
        if(!e)e=seed("/data/outside","outside-data");
    } else if(!e) {
        e=contents("/data/outside","outside-data");
        if(!e && !strcmp(argv[2],"retained"))e=contents("/data/linux/nested/note","nested-linux-data");
        else if(!e && !strcmp(argv[2],"purged")) {cm_stat_t s;e=cm_vfs_stat("/data/linux",&s)==CM_ERR_NOENT?0:CM_ERR_IO;}
        else if(!e)e=CM_ERR_INVAL;
    }
    if(!e)e=cm_vfs_sync();
    close(diskfd);
    printf("linux package fixture %s: %s\n",argv[2],e?"FAIL":"PASS");return e?1:0;
}
