/*
 * GHASH and the GF(2^128) multiply underneath it -- the authentication half of
 * GCM, with no cipher and no device knowledge attached.
 *
 * Ported from solix-esp32's anker_neg_cipher.c, which had it inlined next to
 * the Anker keystream. The algorithm is generic; the constants it was bolted
 * to are device data and now live in the Lua profile. That split is what keeps
 * src/crypto/ reusable for the next protocol.
 *
 * Two deliberate departures from the source it came from:
 *
 *  - It streams. The original assembled aad || pad || ct || pad || len_block
 *    into a fixed 256-byte stack buffer and, on overflow, returned a zeroed
 *    tag -- a silent wrong answer, and reachable: A2687 telemetry frames run
 *    to ~208 bytes before fragmentation and the C1000's are larger still.
 *    Here there is no assembly buffer and therefore no cap.
 *
 *  - The subkey H is a parameter, not a constant.
 */
#ifndef BTGW_GHASH_H
#define BTGW_GHASH_H

#include <stddef.h>
#include <stdint.h>

/* z = x * y in GF(2^128) with the GCM reduction polynomial. Bit-serial: 128
 * iterations, no tables. Slower than a 4-bit table method but ~4 KB smaller,
 * and at our frame rate (one telemetry report per second) invisible. */
void gf128_mul(const uint8_t x[16], const uint8_t y[16], uint8_t z[16]);

/* GHASH(H, aad || pad16 || ct || pad16 || [len(aad)*8 | len(ct)*8]) -> out.
 * This is the S value of GCM; the tag is S XOR E_K(J0), which the caller
 * supplies, because who computes E_K(J0) differs between the negotiation
 * cipher (a recovered constant) and the session cipher (a real AES call). */
void ghash_aad_ct(const uint8_t H[16], const uint8_t *aad, size_t aad_len,
                  const uint8_t *ct, size_t ct_len, uint8_t out[16]);

#endif
