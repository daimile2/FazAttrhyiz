#include "capmix/security.h"
#include "capmix/klog.h"
#include <string.h>
/* NIST SHA256 worked examples (abc and 56-byte message), CAVP ShortMsg Len=0.
 * Source links and trust limitations: docs/PHASE6.md. */
static const uint8_t abc_digest[32]={
 0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
 0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
static uint8_t hex_digit(char c) { return c>='a' ? (uint8_t)(c-'a'+10) : (uint8_t)(c-'0'); }
static int vector(const char *message, const char *hex)
{
    uint8_t expected[32], actual[32];
    for (unsigned i=0;i<32;i++) expected[i]=(hex_digit(hex[2*i])<<4)|hex_digit(hex[2*i+1]);
    size_t len=strlen(message);
    if (cm_sha256(message,len,actual)!=CM_OK || memcmp(expected,actual,32)) return 1;
    cm_sha256_t ctx;
    cm_sha256_init(&ctx);
    for (size_t i=0;i<len;i++) if (cm_sha256_update(&ctx,message+i,1)!=CM_OK) return 1;
    if (cm_sha256_update(&ctx,NULL,0)!=CM_OK || cm_sha256_final(&ctx,actual)!=CM_OK ||
        memcmp(expected,actual,32)) return 1;
    return cm_sha256_update(&ctx,message,1)!=CM_ERR_INVAL ||
           cm_sha256_final(&ctx,actual)!=CM_ERR_INVAL;
}
int cm_phase6_selftest(void)
{
    int fails=0;
    fails+=vector("","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    fails+=vector("abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    fails+=vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    if (!fails) kinfo("phase6: sha256 known vectors verified (empty/abc/two-block/incremental)");
    cm_sign_t good={.content="abc",.content_size=3,.expected_digest=abc_digest,
        .digest_size=32,.flags=CM_SIGN_SIGNED|CM_SIGN_NOTARIZED|CM_SIGN_MODULE,.id="ok.kext"};
    if (cm_sign_verify(&good,1)!=CM_OK || !cm_sip_allows_kext(&good)) fails++;
    cm_sign_t bad=good;
    bad.flags=CM_SIGN_NONE;
    if (cm_sign_verify(&bad,1)!=CM_ERR_DENIED) fails++;
    bad=good; bad.flags=CM_SIGN_SIGNED|CM_SIGN_MODULE;
    if (cm_sign_verify(&bad,1)!=CM_ERR_DENIED || cm_sip_allows_kext(&bad)) fails++;
    bad=good; bad.flags=CM_SIGN_SIGNED|CM_SIGN_NOTARIZED;
    if (cm_sign_verify(&bad,1)!=CM_ERR_DENIED) fails++;
    bad=good; bad.expected_digest=NULL;
    if (cm_sign_verify(&bad,1)!=CM_ERR_DENIED || cm_sip_allows_kext(&bad)) fails++;
    bad=good; bad.digest_size=31;
    if (cm_sign_verify(&bad,0)!=CM_ERR_DENIED) fails++;
    bad=good; bad.content=NULL;
    if (cm_sign_verify(&bad,0)!=CM_ERR_DENIED) fails++;
    bad=good; bad.content="abd"; bad.id="tampered.kext";
    if (cm_sign_verify(&bad,1)!=CM_ERR_DENIED || cm_sip_allows_kext(&bad)) fails++;
    uint8_t wrong[32]; memcpy(wrong,abc_digest,32);
    bad=good; bad.expected_digest=wrong; bad.id="wrong-digest.kext";
    /* Exercise every comparison position against a trusted known digest. */
    for (unsigned i=0;i<32;i++) {
        wrong[i]^=1;
        if (cm_sign_verify(&bad,0)!=CM_ERR_DENIED) fails++;
        wrong[i]^=1;
    }
    cm_sha256_t ctx; uint8_t out[32]; cm_sha256_init(&ctx);
    if (cm_sha256_update(&ctx,NULL,1)!=CM_ERR_INVAL ||
        cm_sha256_final(&ctx,NULL)!=CM_ERR_INVAL || cm_sha256(NULL,1,out)!=CM_ERR_INVAL) fails++;
    ctx.bytes=UINT64_MAX/8;
    if (cm_sha256_update(&ctx,"a",1)!=CM_ERR_INVAL) fails++;
    if (!fails) kinfo("phase6: sign accept/reject verified (wrong digest/tamper/flags-only/SIP)");
    kinfo("phase6 selftest: %s (%d failures)",fails ? "FAIL" : "PASS",fails);
    return fails;
}
