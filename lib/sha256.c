/* Original implementation of FIPS 180-4 sections 4.1.2, 4.2.2, 5, 6.2.
 * Specification: https://doi.org/10.6028/NIST.FIPS.180-4
 * No third-party implementation code is incorporated. */
#include "capmix/sha256.h"
#include <string.h>
static const uint32_t k[64] = {
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32-n)); }
static void compress(cm_sha256_t *c)
{
    uint32_t w[64];
    for (unsigned i=0;i<16;i++) {
        const uint8_t *p=c->block+4*i;
        w[i]=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
    }
    for (unsigned i=16;i<64;i++) {
        uint32_t x=w[i-15], y=w[i-2];
        w[i]=w[i-16]+(rotr(x,7)^rotr(x,18)^(x>>3))+w[i-7]+(rotr(y,17)^rotr(y,19)^(y>>10));
    }
    uint32_t a=c->state[0],b=c->state[1],d=c->state[3],e=c->state[4];
    uint32_t cc=c->state[2],f=c->state[5],g=c->state[6],h=c->state[7];
    for (unsigned i=0;i<64;i++) {
        uint32_t t1=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&cc)^(b&cc));
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=d;
    c->state[4]+=e; c->state[5]+=f; c->state[6]+=g; c->state[7]+=h;
}
cm_error_t cm_sha256_init(cm_sha256_t *c)
{
    static const uint32_t initial[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    if (!c) return CM_ERR_INVAL;
    memset(c,0,sizeof(*c)); memcpy(c->state,initial,sizeof(initial)); return CM_OK;
}
cm_error_t cm_sha256_update(cm_sha256_t *c,const void *data,size_t len)
{
    if (!c || c->finalized || c->used>=64 || (!data && len) ||
        c->bytes>UINT64_MAX/8 || len>UINT64_MAX/8-c->bytes) return CM_ERR_INVAL;
    const uint8_t *p=data;
    c->bytes+=len;
    while (len) {
        size_t n=64-c->used; if (n>len) n=len;
        memcpy(c->block+c->used,p,n); c->used+=n; p+=n; len-=n;
        if (c->used==64) { compress(c); c->used=0; }
    }
    return CM_OK;
}
cm_error_t cm_sha256_final(cm_sha256_t *c,uint8_t out[CM_SHA256_SIZE])
{
    if (!c || !out || c->finalized || c->used>=64 || c->bytes>UINT64_MAX/8) return CM_ERR_INVAL;
    uint64_t bits=c->bytes*8;
    c->block[c->used++]=0x80;
    if (c->used>56) { memset(c->block+c->used,0,64-c->used); compress(c); c->used=0; }
    memset(c->block+c->used,0,56-c->used);
    for (unsigned i=0;i<8;i++) c->block[63-i]=(uint8_t)(bits>>(8*i));
    compress(c);
    for (unsigned i=0;i<32;i++) out[i]=(uint8_t)(c->state[i/4]>>(24-8*(i%4)));
    c->finalized=true; return CM_OK;
}
cm_error_t cm_sha256(const void *data,size_t len,uint8_t out[CM_SHA256_SIZE])
{
    cm_sha256_t c;
    cm_sha256_init(&c);
    cm_error_t r=cm_sha256_update(&c,data,len);
    return r==CM_OK ? cm_sha256_final(&c,out) : r;
}
