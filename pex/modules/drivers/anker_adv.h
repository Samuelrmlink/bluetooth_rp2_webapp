/* Anker advertisement identify. Pure: no pex.h, so the host test links it.
 * Layout is lua/profiles/anker/adv.lua. mfg includes the 2-byte company id.
 * Read the version byte before the capability: a v2 sku is one byte longer,
 * and the last sku character of an A2687 is 0x36, which has the GCM bit set. */
#ifndef ANKER_ADV_H
#define ANKER_ADV_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int version;
    int bound;
    int gcm_hint;
    uint8_t capability;
    uint8_t mac[6];
    char product_type[5];
    char sku[5];
} anker_adv_t;

/* 1 and fills *out, or 0 and optionally *why (a static string). */
int anker_adv_identify(const uint8_t *mfg, size_t mfg_len, const uint8_t addr[6], int has_ff09,
                       anker_adv_t *out, const char **why);
#endif
