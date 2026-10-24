#include "anker_adv.h"
#include <string.h>

static void hex2(char *d, uint8_t b) {
    static const char *h = "0123456789ABCDEF";
    d[0] = h[b >> 4];
    d[1] = h[b & 0xf];
}

int anker_adv_identify(const uint8_t *mfg, size_t mfg_len, const uint8_t addr[6], int has_ff09,
                       anker_adv_t *out, const char **why) {
    if (why) { *why = NULL; }
    if (!mfg || !addr || !out) {
        if (why) { *why = "not a device"; }
        return 0;
    }
    if (!has_ff09) {
        if (why) { *why = "no ff09 service uuid"; }
        return 0;
    }
    if (mfg_len < 2 || mfg[0] != 0xff || mfg[1] != 0xff) {
        if (why) { *why = "not an Anker company id"; }
        return 0;
    }
    const uint8_t *d = mfg + 2;
    size_t n = mfg_len - 2;
    if (n < 10) {
        if (why) { *why = "payload too short to identify"; }
        return 0;
    }
    int version = d[0];
    if (version != 1 && version != 2) {
        if (why) { *why = "unknown advertisement version"; }
        return 0;
    }
    if (memcmp(d + 1, addr, 6) != 0) {
        if (why) { *why = "mac in the payload is not the advertised address"; }
        return 0;
    }
    memset(out, 0, sizeof *out);
    out->version = version;
    memcpy(out->mac, addr, 6);
    out->bound = d[7] == 2;
    hex2(out->product_type, d[8]);
    hex2(out->product_type + 2, d[9]);
    out->product_type[4] = 0;
    int sku_len = version == 2 ? 4 : 3;
    if (n >= (size_t)(10 + sku_len)) {
        memcpy(out->sku, d + 10, (size_t)sku_len);
        out->sku[sku_len] = 0;
    }
    size_t cap_at = (size_t)(10 + sku_len); /* 0-based index of the capability */
    out->capability = n > cap_at ? d[cap_at] : 0;
    out->gcm_hint = (out->capability & 0x04) != 0;
    return 1;
}
