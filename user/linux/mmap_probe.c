#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc,char **argv){
 if(argc<1 || !argv[0])return 1;
 unsigned char *a=malloc(2*1024*1024),*b=mmap(0,1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(!a || b==MAP_FAILED)return 2;
 for(unsigned i=0;i<2*1024*1024;i+=4096){if(a[i])return 3;a[i]=(unsigned char)(i/4096);}
 for(unsigned i=0;i<1024*1024;i+=4096){if(b[i])return 4;b[i]=42;}
 for(unsigned i=0;i<2*1024*1024;i+=4096)if(a[i]!=(unsigned char)(i/4096))return 5;
 free(a);if(munmap(b,1024*1024))return 6;
 const char m[]="linux L5 musl1.2.5: malloc=2MiB mmap=1MiB verified\n";
 return write(1,m,sizeof(m)-1)==sizeof(m)-1?0:7;
}
