#include "ota/cbor.h"
#include <string.h>

/* --- encoder ---------------------------------------------------------------- */

static void put(cbor_enc_t *e, const void *p, size_t n) {
    if (e->err || e->len + n > e->cap) { e->err = true; return; }
    memcpy(e->buf + e->len, p, n);
    e->len += n;
}

static void head(cbor_enc_t *e, uint8_t major, uint64_t v) {
    uint8_t h[9];
    size_t n;
    major <<= 5;
    if (v < 24)            { h[0] = major | (uint8_t)v; n = 1; }
    else if (v <= 0xff)    { h[0] = major | 24; h[1] = (uint8_t)v; n = 2; }
    else if (v <= 0xffff)  { h[0] = major | 25; h[1] = v >> 8; h[2] = v; n = 3; }
    else if (v <= 0xffffffffu) {
        h[0] = major | 26; h[1] = v >> 24; h[2] = v >> 16; h[3] = v >> 8; h[4] = v; n = 5;
    } else {
        h[0] = major | 27;
        for (int i = 0; i < 8; i++) h[1 + i] = (uint8_t)(v >> (56 - 8 * i));
        n = 9;
    }
    put(e, h, n);
}

void cbor_enc_init(cbor_enc_t *e, uint8_t *buf, size_t cap) {
    e->buf = buf; e->cap = cap; e->len = 0; e->err = false;
}
void cbor_map_open(cbor_enc_t *e)   { uint8_t b = 0xbf; put(e, &b, 1); }
void cbor_array_open(cbor_enc_t *e) { uint8_t b = 0x9f; put(e, &b, 1); }
void cbor_close(cbor_enc_t *e)      { uint8_t b = 0xff; put(e, &b, 1); }
void cbor_uint(cbor_enc_t *e, uint64_t v) { head(e, 0, v); }
void cbor_int(cbor_enc_t *e, int64_t v) {
    if (v < 0) head(e, 1, (uint64_t)(-1 - v)); else head(e, 0, (uint64_t)v);
}
void cbor_bool(cbor_enc_t *e, bool v) { uint8_t b = v ? 0xf5 : 0xf4; put(e, &b, 1); }
void cbor_nil(cbor_enc_t *e)          { uint8_t b = 0xf6; put(e, &b, 1); }
void cbor_tstr(cbor_enc_t *e, const char *s) { size_t n = strlen(s); head(e, 3, n); put(e, s, n); }
void cbor_bstr(cbor_enc_t *e, const void *p, size_t n) { head(e, 2, n); put(e, p, n); }
void cbor_kv_uint(cbor_enc_t *e, const char *k, uint64_t v) { cbor_tstr(e, k); cbor_uint(e, v); }
void cbor_kv_int(cbor_enc_t *e, const char *k, int64_t v)   { cbor_tstr(e, k); cbor_int(e, v); }
void cbor_kv_bool(cbor_enc_t *e, const char *k, bool v)     { cbor_tstr(e, k); cbor_bool(e, v); }
void cbor_kv_tstr(cbor_enc_t *e, const char *k, const char *v) { cbor_tstr(e, k); cbor_tstr(e, v); }
void cbor_kv_bstr(cbor_enc_t *e, const char *k, const void *p, size_t n) { cbor_tstr(e, k); cbor_bstr(e, p, n); }

/* --- decoder ---------------------------------------------------------------- */

/* Reads one item head. Returns bytes consumed, 0 on malformed/short input.
 * *indef is set for the indefinite-length forms (0x1f additional info). */
static size_t read_head(const uint8_t *p, size_t n, uint8_t *major, uint64_t *val, bool *indef) {
    if (n < 1) return 0;
    *major = p[0] >> 5;
    uint8_t ai = p[0] & 0x1f;
    *indef = false;
    if (ai < 24) { *val = ai; return 1; }
    size_t extra;
    switch (ai) {
    case 24: extra = 1; break;
    case 25: extra = 2; break;
    case 26: extra = 4; break;
    case 27: extra = 8; break;
    case 31: *indef = true; *val = 0; return 1;
    default: return 0;
    }
    if (n < 1 + extra) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < extra; i++) v = (v << 8) | p[1 + i];
    *val = v;
    return 1 + extra;
}

/* Length of the whole item at p (recursing into containers). 0 = malformed. */
static size_t item_len(const uint8_t *p, size_t n) {
    uint8_t major; uint64_t v; bool indef;
    size_t h = read_head(p, n, &major, &v, &indef);
    if (!h) return 0;
    if (p[0] == 0xff) return 1;                 /* a stray break: caller decides */
    switch (major) {
    case 0: case 1: case 7:
        return h;                               /* ints, simple values, floats */
    case 2: case 3:
        if (indef) {                            /* chunked string: items until break */
            size_t off = h;
            while (off < n && p[off] != 0xff) {
                size_t l = item_len(p + off, n - off);
                if (!l) return 0;
                off += l;
            }
            return off < n ? off + 1 : 0;
        }
        if (v > n - h) return 0;
        return h + (size_t)v;
    case 6: {                                   /* tag: one item follows */
        size_t l = item_len(p + h, n - h);
        return l ? h + l : 0;
    }
    case 4: case 5: {
        size_t off = h;
        uint64_t count = indef ? UINT64_MAX : v * (major == 5 ? 2 : 1);
        for (uint64_t i = 0; i < count; i++) {
            if (off >= n) return 0;
            if (indef && p[off] == 0xff) return off + 1;
            size_t l = item_len(p + off, n - off);
            if (!l) return 0;
            off += l;
        }
        return off;
    }
    }
    return 0;
}

static bool decode_val(const uint8_t *p, size_t n, cbor_val_t *out) {
    uint8_t major; uint64_t v; bool indef;
    size_t h = read_head(p, n, &major, &v, &indef);
    if (!h) return false;
    size_t total = item_len(p, n);
    if (!total) return false;
    memset(out, 0, sizeof *out);
    out->raw = p; out->raw_len = total;
    switch (major) {
    case 0: out->type = CBOR_UINT; out->u = v; break;
    case 1: out->type = CBOR_NINT; out->u = v; break;
    case 2: case 3:
        if (indef) return false;                /* chunked strings: no client sends them */
        out->type = major == 2 ? CBOR_BSTR : CBOR_TSTR;
        out->bytes = p + h; out->len = (size_t)v;
        break;
    case 4: out->type = CBOR_ARRAY; break;
    case 5: out->type = CBOR_MAP; break;
    case 7:
        if (p[0] == 0xf4 || p[0] == 0xf5) { out->type = CBOR_BOOL; out->b = p[0] == 0xf5; }
        else if (p[0] == 0xf6) out->type = CBOR_NULL;
        else out->type = CBOR_OTHER;
        break;
    default: out->type = CBOR_OTHER; break;
    }
    return true;
}

bool cbor_map_find(const uint8_t *p, size_t n, const char *key, cbor_val_t *out) {
    uint8_t major; uint64_t v; bool indef;
    size_t off = read_head(p, n, &major, &v, &indef);
    if (!off || major != 5) return false;
    size_t klen = strlen(key);
    uint64_t pairs = indef ? UINT64_MAX : v;
    for (uint64_t i = 0; i < pairs; i++) {
        if (off >= n) return false;
        if (indef && p[off] == 0xff) return false;
        cbor_val_t k;
        if (!decode_val(p + off, n - off, &k)) return false;
        off += k.raw_len;
        if (off >= n) return false;
        size_t vlen = item_len(p + off, n - off);
        if (!vlen) return false;
        if (k.type == CBOR_TSTR && k.len == klen && memcmp(k.bytes, key, klen) == 0) {
            return decode_val(p + off, n - off, out);
        }
        off += vlen;
    }
    return false;
}

bool cbor_get_uint(const uint8_t *p, size_t n, const char *key, uint64_t *out) {
    cbor_val_t v;
    if (!cbor_map_find(p, n, key, &v) || v.type != CBOR_UINT) return false;
    *out = v.u; return true;
}
bool cbor_get_bool(const uint8_t *p, size_t n, const char *key, bool *out) {
    cbor_val_t v;
    if (!cbor_map_find(p, n, key, &v) || v.type != CBOR_BOOL) return false;
    *out = v.b; return true;
}
bool cbor_get_tstr(const uint8_t *p, size_t n, const char *key, char *dst, size_t cap) {
    cbor_val_t v;
    if (!cbor_map_find(p, n, key, &v) || v.type != CBOR_TSTR || v.len >= cap) return false;
    memcpy(dst, v.bytes, v.len); dst[v.len] = 0; return true;
}
bool cbor_get_bstr(const uint8_t *p, size_t n, const char *key, const uint8_t **out, size_t *len) {
    cbor_val_t v;
    if (!cbor_map_find(p, n, key, &v) || v.type != CBOR_BSTR) return false;
    *out = v.bytes; *len = v.len; return true;
}
