#include "crypto/ghash.h"

#include <string.h>

void gf128_mul(const uint8_t x[16], const uint8_t y[16], uint8_t z[16]) {
    uint8_t v[16];
    memcpy(v, y, 16);
    memset(z, 0, 16);
    for (int i = 0; i < 128; i++) {
        if ((x[i >> 3] >> (7 - (i & 7))) & 1) {
            for (int k = 0; k < 16; k++) { z[k] ^= v[k]; }
        }
        uint8_t lsb = v[15] & 1;
        for (int k = 15; k > 0; k--) {
            v[k] = (uint8_t)((v[k] >> 1) | ((v[k - 1] & 1) << 7));
        }
        v[0] >>= 1;
        if (lsb) { v[0] ^= 0xe1; }
    }
}

/* Absorb len bytes, zero-padding the final partial block to 16. This is what
 * replaces the old assemble-then-hash: the padding never has to exist as
 * memory, so neither does a bound on the input. */
static void absorb(const uint8_t H[16], const uint8_t *p, size_t len, uint8_t y[16]) {
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > 16) { n = 16; }
        uint8_t blk[16];
        memset(blk, 0, 16);         /* the short-block pad, never assembled */
        memcpy(blk, p + off, n);
        uint8_t t[16];
        for (int k = 0; k < 16; k++) { t[k] = y[k] ^ blk[k]; }
        gf128_mul(t, H, y);
        off += n;
    }
}

static void be64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
}

void ghash_aad_ct(const uint8_t H[16], const uint8_t *aad, size_t aad_len,
                  const uint8_t *ct, size_t ct_len, uint8_t out[16]) {
    memset(out, 0, 16);
    if (aad && aad_len) { absorb(H, aad, aad_len, out); }
    if (ct && ct_len) { absorb(H, ct, ct_len, out); }

    uint8_t lenblk[16];
    be64(lenblk, (uint64_t)aad_len * 8);
    be64(lenblk + 8, (uint64_t)ct_len * 8);
    uint8_t t[16];
    for (int k = 0; k < 16; k++) { t[k] = out[k] ^ lenblk[k]; }
    gf128_mul(t, H, out);
}
