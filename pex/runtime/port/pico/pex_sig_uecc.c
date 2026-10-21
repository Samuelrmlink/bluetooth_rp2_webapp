/* PEX signature verification with micro-ecc (ECDSA P-256 over SHA-256, the
 * gateway's OTA scheme, spec 10). Keys are raw X||Y (64 bytes), as
 * tools/ota_keygen.py writes them; key_id n is keys[n-1].
 * The pico-sdk's micro-ecc (lib/btstack/3rd-party) is the old single-curve
 * API: build uECC.c with uECC_CURVE=uECC_secp256r1 (BTstack does). */
#include <stddef.h>
#include "uECC.h"
#include "pex_rt.h"

static const uint8_t (*s_keys)[64];
static uint32_t s_nkeys;

void pex_sig_uecc_set_keys(const uint8_t (*keys)[64], uint32_t n) {
    s_keys = keys;
    s_nkeys = n;
}

int pex_sig_uecc_verify(uint16_t alg, uint16_t key_id, const uint8_t digest[32], const uint8_t *sig,
                        uint32_t sig_len) {
    if (alg != PEX_SIG_ECDSA_P256_SHA256 || sig_len != 64 || key_id == 0 || key_id > s_nkeys) { return PEX_ESIG; }
    return uECC_verify(s_keys[key_id - 1], digest, sig) == 1 ? 0 : PEX_ESIG;
}
