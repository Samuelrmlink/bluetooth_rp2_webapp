/*
 * The Anker session crypto, on uECC + tiny-AES. No mbedTLS.
 *
 * Why not mbedTLS, which the SDK ships: it is built for picotool, not for the
 * firmware, so linking it in costs roughly 80 KB. uECC is already in our image
 * -- BTstack pulls it in for LE Secure Connections, and `curve_G` is 64 bytes,
 * which is how we know it is secp256r1 -- and tiny-AES is 572 lines. The whole
 * surface below costs about 4 KB on top of what was already linked.
 *
 * WHAT IS *NOT* HERE, DELIBERATELY:
 *
 *   The private scalar. solix-esp32 hardcoded the recovered app key; this does
 *   not, and will not. `d` is an argument, read at runtime from cfg/anker.json,
 *   so no secret is ever compiled into a firmware image or committed. Nothing
 *   requires us to impersonate the app's keypair -- the charger does ECDH with
 *   whatever public key arrives in 0x0021 -- so a per-deployment key is both
 *   safer and no harder.
 *
 *   (HMAC-SHA256 and the "ECIES" KDF used to be missing here. They serve the
 *   C1000's AES-256-CBC path, which the A2687 needs not at all, and they
 *   arrived with the C1000 as promised -- see anker_kdf below.)
 *
 * TWO BEHAVIOURS THAT LOOK LIKE BUGS AND ARE NOT. Both are pinned by frozen
 * vectors (test/host/vectors_data.h cases 74 and 79) and "fixing" either one
 * stops the device talking:
 *
 *   1. The PKCS7 unpad is lenient. On any malformed padding it returns the
 *      buffer UNCHANGED rather than an error.
 *   2. GCM decrypt does not verify the tag, and is plain AES-CTR from counter
 *      block 2. The device reuses one nonce for every frame of a session, so
 *      the tag carries no security anyway -- and it is that reuse which let
 *      the negotiation keystream be recovered in the first place.
 */
#ifndef BTGW_ANKER_CRYPTO_H
#define BTGW_ANKER_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- P-256 ---------------------------------------------------------------- */

/* d * G -> xy = X(32 BE) || Y(32 BE). 0 on success. */
int anker_pubkey_xy(const uint8_t d[32], uint8_t xy[64]);

/* Z = X-coordinate of (d * Qpeer), 32 bytes BE. `peer` is `04||X||Y` (65) or a
 * bare `X||Y` (64); trailing bytes are tolerated, because the device's 0x0821
 * reply carries the key inside a TLV whose length we do not want to trust.
 * 0 on success.
 *
 * On the RP2350 this runs in software and is the slowest thing in the
 * handshake; the charger's own side of it takes ~900 ms. */
int anker_ecdh_z(const uint8_t d[32], const uint8_t *peer, size_t peer_len,
                 uint8_t Z[32]);

/* --- key derivation ------------------------------------------------------- */
typedef enum {
    ANKER_MODE_ECDH_LEGACY = 1, /* AES-128-CBC, key = Z[0:16], iv = Z[16:32] */
    ANKER_MODE_GCM = 2,         /* AES-128-GCM, key = Z[0:16], nonce = Z[16:28] */
    ANKER_MODE_ECDH = 3,        /* AES-256-CBC; key/iv from anker_kdf(Z, 64).
                                 * The C1000 (product type B105), and what the
                                 * current Anker app does. */
    ANKER_MODE_ACCOUNT = 4,     /* AES-256-CBC, key = utf8(accountId[:32]),
                                 * iv = 16 zero bytes. AesConferV1. */
} anker_mode_t;

/*
 * The Anker "ECIES" KDF: a counter-free HMAC-SHA256 chain keyed by Z, with
 * the literal info string "ECIES" both mixed into every block and used to
 * seed the chain. Not X9.63, not HKDF -- it is its own thing, and the frozen
 * vectors (KDF_CASES) are the specification, not this comment.
 *
 *   h = HMAC(Z, "ECIES")
 *   block i = HMAC(Z, h || "ECIES");  h = HMAC(Z, h) between blocks
 *
 * outlen may be any length; the stream is prefix-stable, so asking for 48
 * bytes gives the first 48 of what 64 would give. 0 on success. */
int anker_kdf(const uint8_t Z[32], uint8_t *out, size_t outlen);

/* AesConferV1 / account-key mode: key = the account id's UTF-8 bytes,
 * truncated or zero-padded to 32, and an all-zero IV. No hashing at all --
 * which is worth stating plainly, because it looks like an omission. */
int anker_derive_account_key(const char *account_id, uint8_t key[32], uint8_t iv[16]);

/* Slice Z into an AES key and an IV per `mode`. 0 on success. */
int anker_derive_keys(const uint8_t Z[32], anker_mode_t mode, uint8_t key[32],
                      size_t *keylen, uint8_t iv[16], size_t *ivlen);

/* --- AES ------------------------------------------------------------------ */

/* AES-CBC. Encrypt PKCS7-pads, adding a whole block when already aligned.
 * Decrypt unpads leniently (see the header note). *outlen is out-only.
 * Returns 0, or -1 on bad arguments / insufficient capacity. */
int anker_aes_cbc(bool encrypt, const uint8_t *key, size_t keylen,
                  const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out,
                  size_t out_cap, size_t *outlen);

/* AES-GCM encrypt. out = ciphertext(n) || tag(16). The AAD is a parameter
 * rather than a constant: the 16-byte block Anker uses is device data and
 * lives in the Lua profile, not here. Returns n + 16, or -1. */
int anker_aes_gcm_encrypt(const uint8_t *key, size_t keylen, const uint8_t iv12[12],
                          const uint8_t *aad, size_t aad_len, const uint8_t *pt,
                          size_t n, uint8_t *out, size_t out_cap);

/* AES-GCM decrypt WITHOUT tag verification. `body` is ciphertext || tag(16).
 * Returns the plaintext length (n - 16, or n when n < 16), or -1. */
int anker_aes_gcm_decrypt(const uint8_t *key, size_t keylen, const uint8_t iv12[12],
                          const uint8_t *body, size_t n, uint8_t *out, size_t out_cap);

/* Recompute the tag a well-behaved implementation would have produced, so a
 * caller that wants to check one can, without this module ever refusing a
 * frame on its own. Used by the parity test and by the profile's
 * "did the device adopt our key?" probe. */
void anker_aes_gcm_tag(const uint8_t *key, size_t keylen, const uint8_t iv12[12],
                       const uint8_t *aad, size_t aad_len, const uint8_t *ct,
                       size_t ct_len, uint8_t tag[16]);

#endif
