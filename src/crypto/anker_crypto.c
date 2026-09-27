#include "crypto/anker_crypto.h"
#include "crypto/sha256.h"

#include <string.h>

#include "aes.h"
#include "crypto/ghash.h"
#include "uECC.h"

/* --- P-256 ---------------------------------------------------------------- */

int anker_pubkey_xy(const uint8_t d[32], uint8_t xy[64]) {
    if (!d || !xy) { return -1; }
    return uECC_compute_public_key(d, xy) ? 0 : -1;
}

int anker_ecdh_z(const uint8_t d[32], const uint8_t *peer, size_t peer_len,
                 uint8_t Z[32]) {
    if (!d || !peer || !Z) { return -1; }
    /* An uncompressed point is 04||X||Y; the device sometimes hands us the
     * bare X||Y, and the TLV it arrives in may be longer than the key. Take
     * the 64 bytes that matter and ignore the rest rather than rejecting a
     * frame over a length we have no reason to trust. */
    const uint8_t *xy;
    if (peer_len >= 65 && peer[0] == 0x04) {
        xy = peer + 1;
    } else if (peer_len >= 64) {
        xy = peer;
    } else {
        return -1;
    }
    /* uECC_shared_secret returns the X-coordinate of d*Q, which is exactly the
     * Z the app's crypto.js uses -- no KDF, no hashing, despite uECC's header
     * recommending one. Matching the app is the requirement here. */
    return uECC_shared_secret(xy, d, Z) ? 0 : -1;
}

/* --- key derivation ------------------------------------------------------- */

int anker_derive_keys(const uint8_t Z[32], anker_mode_t mode, uint8_t key[32],
                      size_t *keylen, uint8_t iv[16], size_t *ivlen) {
    if (!Z || !key || !keylen || !iv || !ivlen) { return -1; }
    switch (mode) {
        case ANKER_MODE_ECDH_LEGACY:
            memcpy(key, Z, 16);
            *keylen = 16;
            memcpy(iv, Z + 16, 16);
            *ivlen = 16;
            return 0;
        case ANKER_MODE_GCM:
            memcpy(key, Z, 16);
            *keylen = 16;
            memcpy(iv, Z + 16, 12);
            *ivlen = 12;
            return 0;
        case ANKER_MODE_ECDH: {
            /* The C1000 path: 64 bytes of KDF output, of which the first 32
             * are an AES-256 key and the next 16 the IV. The last 16 are
             * unused by the cipher -- the app derives 64 and throws them
             * away, and matching that exactly is what the vectors pin. */
            uint8_t kdf[64];
            if (anker_kdf(Z, kdf, sizeof kdf) != 0) { return -1; }
            memcpy(key, kdf, 32);
            *keylen = 32;
            memcpy(iv, kdf + 32, 16);
            *ivlen = 16;
            return 0;
        }
        default:
            return -1;
    }
}

/* --- the ECIES KDF and the account key ------------------------------------ */

int anker_kdf(const uint8_t Z[32], uint8_t *out, size_t outlen) {
    static const char INFO[] = "ECIES";
    const size_t info_len = sizeof(INFO) - 1;
    if (!Z || !out || outlen == 0) { return -1; }

    uint8_t h[SHA256_DIGEST_LEN];
    hmac_sha256(Z, 32, INFO, info_len, h);

    size_t done = 0;
    while (done < outlen) {
        uint8_t msg[SHA256_DIGEST_LEN + 8];
        memcpy(msg, h, SHA256_DIGEST_LEN);
        memcpy(msg + SHA256_DIGEST_LEN, INFO, info_len);

        uint8_t blk[SHA256_DIGEST_LEN];
        hmac_sha256(Z, 32, msg, SHA256_DIGEST_LEN + info_len, blk);

        size_t take = outlen - done;
        if (take > SHA256_DIGEST_LEN) { take = SHA256_DIGEST_LEN; }
        memcpy(out + done, blk, take);
        done += take;

        if (done < outlen) {
            uint8_t next[SHA256_DIGEST_LEN];
            hmac_sha256(Z, 32, h, SHA256_DIGEST_LEN, next);
            memcpy(h, next, SHA256_DIGEST_LEN);
        }
    }
    return 0;
}

int anker_derive_account_key(const char *account_id, uint8_t key[32], uint8_t iv[16]) {
    if (!key || !iv) { return -1; }
    memset(key, 0, 32);
    memset(iv, 0, 16);
    if (!account_id) { return 0; }        /* no id == an all-zero key, as upstream */
    size_t n = strlen(account_id);
    if (n > 32) { n = 32; }
    memcpy(key, account_id, n);
    return 0;
}

/* --- AES-CBC -------------------------------------------------------------- */

int anker_aes_cbc(bool encrypt, const uint8_t *key, size_t keylen,
                  const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out,
                  size_t out_cap, size_t *outlen) {
    if (!key || !iv || !out || !outlen) { return -1; }
    if (n && !in) { return -1; }
    if (keylen != 16 && keylen != 24 && keylen != 32) { return -1; }

    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, keylen, iv);

    if (encrypt) {
        /* PKCS7: a whole block of padding when the input is already aligned,
         * which is what the app does and what the device expects. */
        size_t pad = 16 - (n % 16);
        size_t total = n + pad;
        if (total > out_cap) { return -1; }
        if (n) { memcpy(out, in, n); }
        memset(out + n, (int)pad, pad);
        AES_CBC_encrypt_buffer(&ctx, out, total);
        *outlen = total;
        return 0;
    }

    if (n % 16 || n == 0) { return -1; }
    if (n > out_cap) { return -1; }
    memcpy(out, in, n);
    AES_CBC_decrypt_buffer(&ctx, out, n);

    /* Lenient unpad. Anything the least bit wrong and the buffer is returned
     * as-is. A strict unpad here is a regression that silently stops the
     * device talking, which is why vector case 74 exists. */
    uint8_t pad = out[n - 1];
    bool ok = pad >= 1 && pad <= 16 && (size_t)pad <= n;
    if (ok) {
        for (size_t i = 0; i < pad; i++) {
            if (out[n - 1 - i] != pad) { ok = false; break; }
        }
    }
    *outlen = ok ? n - pad : n;
    return 0;
}

/* --- AES-GCM -------------------------------------------------------------- */

/* GCM's counter is the low 32 bits of the block, big-endian. */
static void ctr_inc(uint8_t blk[16]) {
    for (int i = 15; i >= 12; i--) {
        if (++blk[i] != 0) { break; }
    }
}

/* Keystream from counter block `ctr`, XORed over buf in place. */
static void ctr_xor(const struct AES_ctx *ctx, uint8_t ctr[16], uint8_t *buf,
                    size_t len) {
    size_t off = 0;
    while (off < len) {
        uint8_t ks[16];
        memcpy(ks, ctr, 16);
        AES_ECB_encrypt(ctx, ks);
        size_t n = len - off;
        if (n > 16) { n = 16; }
        for (size_t i = 0; i < n; i++) { buf[off + i] ^= ks[i]; }
        ctr_inc(ctr);
        off += n;
    }
}

/* H = E_K(0^16); ej0 = E_K(J0) where J0 = iv12 || 00 00 00 01. */
static void gcm_setup(const struct AES_ctx *ctx, const uint8_t iv12[12],
                      uint8_t H[16], uint8_t ej0[16], uint8_t ctr1[16]) {
    memset(H, 0, 16);
    AES_ECB_encrypt(ctx, H);

    uint8_t j0[16];
    memcpy(j0, iv12, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

    memcpy(ej0, j0, 16);
    AES_ECB_encrypt(ctx, ej0);

    /* Data starts at counter block 2. */
    memcpy(ctr1, j0, 16);
    ctr_inc(ctr1);
}

void anker_aes_gcm_tag(const uint8_t *key, size_t keylen, const uint8_t iv12[12],
                       const uint8_t *aad, size_t aad_len, const uint8_t *ct,
                       size_t ct_len, uint8_t tag[16]) {
    struct AES_ctx ctx;
    AES_init_ctx(&ctx, key, keylen);
    uint8_t H[16], ej0[16], ctr[16];
    gcm_setup(&ctx, iv12, H, ej0, ctr);
    ghash_aad_ct(H, aad, aad_len, ct, ct_len, tag);
    for (int i = 0; i < 16; i++) { tag[i] ^= ej0[i]; }
}

int anker_aes_gcm_encrypt(const uint8_t *key, size_t keylen, const uint8_t iv12[12],
                          const uint8_t *aad, size_t aad_len, const uint8_t *pt,
                          size_t n, uint8_t *out, size_t out_cap) {
    if (!key || !iv12 || !out) { return -1; }
    if (n && !pt) { return -1; }
    if (keylen != 16 && keylen != 24 && keylen != 32) { return -1; }
    if (n + 16 > out_cap) { return -1; }

    struct AES_ctx ctx;
    AES_init_ctx(&ctx, key, keylen);
    uint8_t H[16], ej0[16], ctr[16];
    gcm_setup(&ctx, iv12, H, ej0, ctr);

    if (n) { memcpy(out, pt, n); }
    ctr_xor(&ctx, ctr, out, n);

    uint8_t s[16];
    ghash_aad_ct(H, aad, aad_len, out, n, s);
    for (int i = 0; i < 16; i++) { out[n + i] = (uint8_t)(s[i] ^ ej0[i]); }
    return (int)(n + 16);
}

int anker_aes_gcm_decrypt(const uint8_t *key, size_t keylen, const uint8_t iv12[12],
                          const uint8_t *body, size_t n, uint8_t *out,
                          size_t out_cap) {
    if (!key || !iv12 || !body || !out) { return -1; }
    if (keylen != 16 && keylen != 24 && keylen != 32) { return -1; }

    size_t pt_len = n >= 16 ? n - 16 : n;
    if (pt_len > out_cap) { return -1; }

    struct AES_ctx ctx;
    AES_init_ctx(&ctx, key, keylen);
    uint8_t H[16], ej0[16], ctr[16];
    gcm_setup(&ctx, iv12, H, ej0, ctr);
    (void)H; (void)ej0;   /* no tag check here -- see the header note */

    if (pt_len) { memcpy(out, body, pt_len); }
    ctr_xor(&ctx, ctr, out, pt_len);
    return (int)pt_len;
}
