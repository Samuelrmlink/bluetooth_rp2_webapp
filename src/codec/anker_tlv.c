#include "codec/anker_tlv.h"

#include <math.h>
#include <string.h>

const char *anker_tlv_type_name(anker_tlv_type_t t) {
    switch (t) {
    case ANKER_TLV_STRING: return "string";
    case ANKER_TLV_U8: return "u8";
    case ANKER_TLV_U16: return "u16";
    case ANKER_TLV_U32: return "u32";
    case ANKER_TLV_F32: return "f32";
    case ANKER_TLV_BYTES: return "bytes";
    default: return "unknown";
    }
}

static bool all_printable(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] > 0x7e) return false;
    return true;
}

static uint64_t le_uint(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static double f32_le_rounded(const uint8_t *p) {
    uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                    ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &bits, 4);
    // telemetry.js: Math.round(x * 1000) / 1000
    return round((double)f * 1000.0) / 1000.0;
}

// charging-tlv.js decodeValue + telemetry.js promotion, for one item at `off`.
static void decode_item(const uint8_t *body, uint16_t off, uint8_t n,
                        anker_tlv_item_t *it) {
    const uint8_t *region = body + off + 2; // value region, incl. the type byte
    const uint8_t *val = body + off + 3;    // bytes after the type byte
    size_t vlen = n >= 1 ? (size_t)(n - 1) : 0;
    int tb = n >= 1 ? region[0] : -1;

    it->off = off;
    it->len = n;
    it->type_byte = (int16_t)tb;
    it->promoted = false;
    it->ival = 0;
    it->fval = 0;
    it->val_off = (uint16_t)(off + 2);
    it->val_len = n;

    if (n == 0) {
        it->type = ANKER_TLV_UNKNOWN; // JS kind "raw", value []
        return;
    }

    switch (tb) {
    case 0x00: {
        // ASCII string, trailing NUL trimmed
        size_t t = vlen;
        while (t > 0 && val[t - 1] == 0x00) t--;
        it->type = ANKER_TLV_STRING;
        it->val_off = (uint16_t)(off + 3);
        it->val_len = (uint16_t)t;
        return;
    }
    case 0x01:
    case 0x02:
    case 0x03:
        it->type = (tb == 0x01) ? ANKER_TLV_U8 : (tb == 0x02) ? ANKER_TLV_U16
                                                              : ANKER_TLV_U32;
        it->ival = (int64_t)le_uint(val, vlen);
        return;
    case 0x04:
    case 0x06:
        it->type = ANKER_TLV_BYTES;
        it->val_off = (uint16_t)(off + 3);
        it->val_len = (uint16_t)vlen;
        return;
    case 0x05:
        it->type = ANKER_TLV_F32;
        it->fval = vlen >= 4 ? f32_le_rounded(val) : 0;
        return;
    default:
        break;
    }

    // Unknown type byte -> raw (value region incl. the type byte). Promote to a
    // string when the whole region (>= 2 bytes) is printable ASCII, matching
    // telemetry.js (it tests e.rawHex, i.e. the region *with* the type byte).
    if (n >= 2 && all_printable(region, n)) {
        it->type = ANKER_TLV_STRING;
        it->promoted = true;
        it->val_off = (uint16_t)(off + 2);
        it->val_len = n;
    } else {
        it->type = ANKER_TLV_UNKNOWN;
    }
}

int anker_tlv_parse(const uint8_t *body, size_t len, anker_tlv_item_t *items,
                    size_t cap, size_t *n_out) {
    if (n_out) *n_out = 0;
    if (!body || !items) return -1;

    size_t i = 0, n = 0;
    while (i + 1 < len) {
        uint8_t tag = body[i];
        uint8_t nn = body[i + 1];
        if (i + 2 + (size_t)nn > len) break; // truncated / not TLV from here
        if (n < cap) {
            anker_tlv_item_t *it = &items[n];
            decode_item(body, (uint16_t)i, nn, it);
            it->tag = tag;
            n++;
        }
        i += (size_t)nn + 2;
    }
    if (n_out) *n_out = n;
    return (int)i; // JS `consumed`
}

bool anker_tlv_find_tag(const uint8_t *body, size_t len, uint8_t tag,
                        anker_tlv_item_t *out) {
    if (!body || !out) return false;
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t t = body[i];
        uint8_t n = body[i + 1];
        if (i + 2 + (size_t)n > len) break;
        if (t == tag) {
            decode_item(body, (uint16_t)i, n, out);
            out->tag = t;
            return true;
        }
        i += (size_t)n + 2;
    }
    return false;
}

int anker_tlv_parse_negotiation(const uint8_t *body, size_t len,
                                anker_tlv_item_t *items, size_t cap,
                                size_t *n_out) {
    if (n_out) *n_out = 0;
    if (!body || !items) return -1;

    size_t i = 0, n = 0;
    while (i + 1 < len) {
        uint8_t tag = body[i];
        uint8_t nn = body[i + 1];
        if (i + 2 + (size_t)nn > len) break;
        if (n < cap) {
            anker_tlv_item_t *it = &items[n];
            memset(it, 0, sizeof(*it));
            it->off = (uint16_t)i;
            it->tag = tag;
            it->len = nn;
            it->type_byte = -1;
            it->val_off = (uint16_t)(i + 2);
            it->val_len = nn;
            const uint8_t *val = body + i + 2;
            if (nn >= 1 && all_printable(val, nn)) {
                it->type = ANKER_TLV_STRING;
            } else if (nn == 1) {
                it->type = ANKER_TLV_U8;
                it->ival = val[0];
            } else if (nn == 2) {
                it->type = ANKER_TLV_U16;
                it->ival = (int64_t)le_uint(val, 2);
            } else if (nn == 4) {
                it->type = ANKER_TLV_U32;
                it->ival = (int64_t)le_uint(val, 4);
            } else {
                it->type = ANKER_TLV_BYTES;
            }
            n++;
        }
        i += (size_t)nn + 2;
    }
    if (n_out) *n_out = n;
    return (int)i;
}

int anker_tlv_status_strip(const uint8_t **body, size_t *len, int mode) {
    if (!body || !*body || !len) return 0;
    if (*len == 0) return 0;
    bool drop;
    if (mode > 0)
        drop = true;
    else if (mode == 0)
        drop = false;
    else
        drop = ((*body)[0] == 0x00);
    if (drop) {
        *body += 1;
        *len -= 1;
        return 1;
    }
    return 0;
}

bool anker_tlv_looks_anomalous(const anker_tlv_item_t *items, size_t n) {
    if (!items || n < 5) return true;
    size_t unknown = 0;
    for (size_t i = 0; i < n; i++)
        if (items[i].type == ANKER_TLV_UNKNOWN) unknown++;
    double threshold = (double)n / 3.0;
    if (threshold < 2.0) threshold = 2.0;
    return (double)unknown > threshold;
}

// ---- encode ------------------------------------------------------------------

int anker_enc_default_type_byte(anker_enc_t enc) {
    switch (enc) {
    case ANKER_ENC_STRING: return 0x00;
    case ANKER_ENC_U8: return 0x01;
    case ANKER_ENC_U16: return 0x02;
    case ANKER_ENC_U32: return 0x03;
    case ANKER_ENC_F32: return 0x05;
    case ANKER_ENC_BYTES: return 0x04;
    default: return -1;
    }
}

// charging-tlv.js encodeChargingValue, simple subset. Writes into v[cap], returns
// length or -1.
static int encode_value(const anker_tlv_spec_t *s, uint8_t *v, size_t cap) {
    switch (s->enc) {
    case ANKER_ENC_STRING:
    case ANKER_ENC_BYTES:
        if (s->data_len > cap) return -1;
        if (s->data_len && s->data) memcpy(v, s->data, s->data_len);
        return (int)s->data_len;
    case ANKER_ENC_U8:
    case ANKER_ENC_U16:
    case ANKER_ENC_U32: {
        size_t w = s->enc == ANKER_ENC_U8 ? 1 : s->enc == ANKER_ENC_U16 ? 2 : 4;
        if (w > cap) return -1;
        // encNum: Math.trunc(v); if (n < 0) n = n >>> 0 (32-bit wrap); LE bytes.
        unsigned long long n = s->ival < 0 ? (unsigned long long)(uint32_t)(int32_t)s->ival
                                           : (unsigned long long)s->ival;
        for (size_t i = 0; i < w; i++) v[i] = (uint8_t)((n >> (8 * i)) & 0xff);
        return (int)w;
    }
    case ANKER_ENC_F32: {
        if (cap < 4) return -1;
        float f = (float)s->fval;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        v[0] = (uint8_t)(bits & 0xff);
        v[1] = (uint8_t)((bits >> 8) & 0xff);
        v[2] = (uint8_t)((bits >> 16) & 0xff);
        v[3] = (uint8_t)((bits >> 24) & 0xff);
        return 4;
    }
    default:
        return -1;
    }
}

int anker_tlv_build_item(const anker_tlv_spec_t *spec, uint8_t *out, size_t cap) {
    if (!spec || !out) return -1;

    uint8_t vbuf[256];
    int vlen = encode_value(spec, vbuf, sizeof vbuf);
    if (vlen < 0) return -1;

    int tb = spec->type_byte;
    if (tb == -1) tb = anker_enc_default_type_byte(spec->enc);
    bool have_tb = tb >= 0; // -2 (or a bad enc default) -> omit the type byte

    size_t body_len = (size_t)vlen + (have_tb ? 1 : 0);
    if (body_len > 0xff) return -1; // simple path: one-byte length only
    size_t total = 2 + body_len;
    if (total > cap) return -1;

    out[0] = spec->tag;
    out[1] = (uint8_t)body_len;
    size_t o = 2;
    if (have_tb) out[o++] = (uint8_t)(tb & 0xff);
    if (vlen) memcpy(out + o, vbuf, (size_t)vlen);
    return (int)total;
}

int anker_tlv_build_payload(const anker_tlv_spec_t *specs, size_t n, uint8_t *out,
                            size_t cap) {
    if (!specs || !out) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int w = anker_tlv_build_item(&specs[i], out + o, cap - o);
        if (w < 0) return -1;
        o += (size_t)w;
    }
    return (int)o;
}

int anker_tlv_stamp_fe(uint8_t *body, size_t len, uint32_t epoch) {
    if (!body) return 0;
    int stamped = 0;
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t n = body[i + 1];
        if (i + 2 + (size_t)n > len) break;
        if (body[i] == 0xFE) {
            size_t vo = 0;
            if (n == 5) vo = i + 3;      // fe 05 03 <e32>
            else if (n == 4) vo = i + 2; // fe 04 <e32>
            if (vo && vo + 4 <= len) {
                body[vo + 0] = (uint8_t)(epoch & 0xff);
                body[vo + 1] = (uint8_t)((epoch >> 8) & 0xff);
                body[vo + 2] = (uint8_t)((epoch >> 16) & 0xff);
                body[vo + 3] = (uint8_t)((epoch >> 24) & 0xff);
                stamped++;
            }
        }
        i += (size_t)n + 2;
    }
    return stamped;
}
