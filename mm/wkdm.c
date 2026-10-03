#include "capmix/compress.h"
#include "capmix/mm.h"
#include "capmix/klog.h"

#include <string.h>

/* Encoding per word:
 *   0x00          — zero word
 *   0x80 | idx    — dictionary hit (idx 0..15)
 *   0x40          — literal, next 4 bytes little-endian, inserted into dict
 */

size_t cm_wkdm_compress(const void *src, size_t slen, void *dst, size_t dlen)
{
    if (!src || !dst || slen % 4) return 0;
    const uint32_t *in = (const uint32_t *)src;
    uint8_t *out = (uint8_t *)dst;
    size_t nw = slen / 4;
    uint32_t dict[CM_WKDM_DICT];
    memset(dict, 0, sizeof(dict));
    int dnext = 0;
    size_t o = 0;

    for (size_t i = 0; i < nw; i++) {
        uint32_t w = in[i];
        if (o + 5 > dlen) return 0;
        if (w == 0) {
            out[o++] = 0x00;
            continue;
        }
        int hit = -1;
        for (int d = 0; d < CM_WKDM_DICT; d++) {
            if (dict[d] == w) { hit = d; break; }
        }
        if (hit >= 0) {
            out[o++] = (uint8_t)(0x80 | hit);
        } else {
            out[o++] = 0x40;
            out[o++] = (uint8_t)w;
            out[o++] = (uint8_t)(w >> 8);
            out[o++] = (uint8_t)(w >> 16);
            out[o++] = (uint8_t)(w >> 24);
            dict[dnext] = w;
            dnext = (dnext + 1) % CM_WKDM_DICT;
        }
    }
    return o;
}

size_t cm_wkdm_decompress(const void *src, size_t slen, void *dst, size_t dlen)
{
    if (!src || !dst) return 0;
    const uint8_t *in = (const uint8_t *)src;
    uint32_t *out = (uint32_t *)dst;
    size_t maxw = dlen / 4;
    uint32_t dict[CM_WKDM_DICT];
    memset(dict, 0, sizeof(dict));
    int dnext = 0;
    size_t i = 0, w = 0;

    while (i < slen && w < maxw) {
        uint8_t tag = in[i++];
        if (tag == 0x00) {
            out[w++] = 0;
        } else if ((tag & 0x80) && (tag & 0x0F) < CM_WKDM_DICT) {
            out[w++] = dict[tag & 0x0F];
        } else if (tag == 0x40) {
            if (i + 4 > slen) return 0;
            uint32_t v = (uint32_t)in[i] | ((uint32_t)in[i + 1] << 8) |
                         ((uint32_t)in[i + 2] << 16) | ((uint32_t)in[i + 3] << 24);
            i += 4;
            out[w++] = v;
            dict[dnext] = v;
            dnext = (dnext + 1) % CM_WKDM_DICT;
        } else {
            return 0;
        }
    }
    return w * 4;
}

int cm_wkdm_selftest(void)
{
    uint8_t page[CM_PAGE_SIZE];
    uint8_t back[CM_PAGE_SIZE];
    uint8_t blob[CM_WKDM_MAX];
    memset(page, 0, sizeof(page));
    for (int i = 0; i < 64; i++)
        ((uint32_t *)page)[i] = 0x11111111u;
    for (int i = 64; i < 80; i++)
        ((uint32_t *)page)[i] = (uint32_t)(0xA5000000u + (unsigned)i);

    size_t cl = cm_wkdm_compress(page, CM_PAGE_SIZE, blob, sizeof(blob));
    if (cl == 0 || cl >= CM_PAGE_SIZE) {
        kerr("wkdm: compress failed or no gain cl=%llu", (unsigned long)cl);
        return -1;
    }
    memset(back, 0x5A, sizeof(back));
    size_t dl = cm_wkdm_decompress(blob, cl, back, sizeof(back));
    if (dl != CM_PAGE_SIZE || memcmp(page, back, CM_PAGE_SIZE) != 0) {
        kerr("wkdm: roundtrip mismatch dl=%llu", (unsigned long)dl);
        return -1;
    }
    kinfo("wkdm: 4096 → %llu bytes, roundtrip ok", (unsigned long)cl);
    return 0;
}
