#ifndef BTGW_SHA256_H
#define BTGW_SHA256_H

/*
 * SHA-256 and HMAC-SHA256, in software.
 *
 * The RP2350 HAS a SHA-256 accelerator, and the SDK wraps it (hardware_sha256
 * / pico_sha256). This does not use it, on purpose: the frozen protocol
 * vectors run on a host as well as on the board, and a hardware path on one
 * side with a software path on the other means the vectors stop testing what
 * actually ships. The whole of the Anker handshake needs a handful of
 * compression blocks, so the accelerator would save microseconds we do not
 * need against a charger that takes ~900 ms to answer its key exchange.
 *
 * Written here rather than vendored: it is 150 lines of FIPS 180-4 and that
 * is cheaper than another third_party/ entry with its own licence to track.
 */
#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32
#define SHA256_BLOCK_LEN  64

typedef struct {
    uint32_t h[8];
    uint64_t len;                    /* total message bytes */
    uint8_t  buf[SHA256_BLOCK_LEN];
    size_t   buf_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t n);
void sha256_final(sha256_ctx_t *c, uint8_t out[SHA256_DIGEST_LEN]);

void sha256(const void *data, size_t n, uint8_t out[SHA256_DIGEST_LEN]);

void hmac_sha256(const void *key, size_t keylen,
                 const void *msg, size_t msglen,
                 uint8_t out[SHA256_DIGEST_LEN]);

#endif
