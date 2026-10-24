#include "anker_wire.h"
#include <string.h>

/* Bytes from lua/profiles/anker/cipher.lua. test/host/neg_fixture.h and
 * tools/run_tests.py keep the Lua copy from drifting; this copy has to match
 * that file. A plaintext 0x0021 is trap 1, and there is no keystream past 67. */
static const uint8_t KEYSTREAM[ANKER_KEYSTREAM_LEN] = {
    0xab, 0x86, 0x3f, 0xd2, 0xe6, 0x3a, 0x29, 0xb2, 0xfe, 0xb4, 0x08, 0xc9, 0x4b, 0x6f, 0x66, 0xd5,
    0x83, 0x79, 0x83, 0xd0, 0x3a, 0xe4, 0xc0, 0x82, 0xf6, 0xef, 0xe0, 0x56, 0x34, 0x51, 0xfb, 0x3e,
    0xd0, 0x9a, 0xcc, 0x47, 0x7e, 0x96, 0xb3, 0x3f, 0xb9, 0x6c, 0x16, 0x09, 0xab, 0xdd, 0xcb, 0x5c,
    0x53, 0x51, 0x2e, 0x51, 0x08, 0x94, 0x4f, 0x9e, 0xab, 0x52, 0x97, 0x5c, 0xed, 0x6d, 0x8c, 0x49,
    0x0c, 0x73, 0x6e
};
static const uint8_t GHASH_H[16] = {
    0xc8, 0xad, 0xd2, 0xd2, 0xfc, 0x09, 0x9e, 0xe3, 0xff, 0x84, 0x8b, 0x0f, 0xc8, 0xcd, 0xc7, 0x38
};
static const uint8_t EJ0[16] = {
    0xbe, 0xfd, 0xec, 0xb8, 0xa8, 0x64, 0x65, 0x30, 0x40, 0xb7, 0x4b, 0xc0, 0x94, 0x1c, 0xac, 0xa2
};
static const uint8_t AAD[16] = {
    0x33, 0x22, 0x11, 0x00, 0x77, 0x66, 0x55, 0x44, 0xbb, 0xaa, 0x99, 0x88, 0xff, 0xee, 0xdd, 0xcc
};
static const uint8_t POISON_A[4] = { 0xb0, 0x73, 0x3c, 0xc3 };
static const uint8_t POISON_B[4] = { 0x4c, 0x4d, 0x80, 0x7e };

static anker_ghash_fn s_ghash;

void anker_set_ghash(anker_ghash_fn fn) { s_ghash = fn; }
const uint8_t *anker_keystream(void) { return KEYSTREAM; }
const uint8_t *anker_ghash_h(void) { return GHASH_H; }
const uint8_t *anker_ej0(void) { return EJ0; }
const uint8_t *anker_aad(void) { return AAD; }

uint32_t anker_epoch(uint32_t ms) { return 1700000000u + ms / 1000u; }

void anker_le32(uint32_t v, uint8_t d[4]) {
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16);
    d[3] = (uint8_t)(v >> 24);
}

static void xor_n(uint8_t *d, const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) { d[i] = (uint8_t)(a[i] ^ b[i]); }
}

int anker_neg_encrypt(const uint8_t *pt, size_t n, uint8_t *out, size_t cap) {
    if (!pt || !out || !s_ghash || n > ANKER_KEYSTREAM_LEN || cap < n + 16) { return -1; }
    xor_n(out, pt, KEYSTREAM, n);
    uint8_t tag[16];
    s_ghash(GHASH_H, AAD, 16, out, n, tag);
    xor_n(out + n, tag, EJ0, 16);
    return (int)(n + 16);
}

int anker_neg_decrypt(const uint8_t *body, size_t n, uint8_t *out, size_t cap) {
    if (!body || !out) { return -1; }
    size_t keep = n >= 16 ? n - 16 : n;
    if (cap < keep) { return -1; }
    size_t m = keep < ANKER_KEYSTREAM_LEN ? keep : ANKER_KEYSTREAM_LEN;
    xor_n(out, body, KEYSTREAM, m);
    if (keep > m) { memcpy(out + m, body + m, keep - m); }
    return (int)keep;
}

int anker_nak_poison(const uint8_t *body, size_t n) {
    if (!body || n < 4) { return 0; }
    return memcmp(body, POISON_A, 4) == 0 || memcmp(body, POISON_B, 4) == 0;
}

static int emit_items(anker_item *it, size_t n, uint8_t *out, size_t cap) {
    return anker_tlv_emit(it, n, out, cap);
}

/* encode() then wrap as a bare bytes item: the type byte lives in the value. */
static int put_region(anker_item *it, uint8_t tag, const uint8_t *region, size_t n) {
    it->tag = tag;
    it->enc = ANKER_ENC_BYTES;
    it->type_byte = -2;
    it->ival = 0;
    it->data = region;
    it->data_len = (uint16_t)n;
    return 0;
}

static size_t encode_region(uint8_t enc, int32_t v, const uint8_t *extra, size_t extra_n,
                            uint8_t *d, size_t cap) {
    if (enc == ANKER_V_U32 || enc == ANKER_V_U32B) {
        if (cap < 5) { return 0; }
        d[0] = enc == ANKER_V_U32B ? 0x04 : 0x03;
        anker_le32((uint32_t)v, d + 1);
        return 5;
    }
    if (enc == ANKER_V_U16) {
        if (cap < 3) { return 0; }
        d[0] = 0x02;
        d[1] = (uint8_t)v;
        d[2] = (uint8_t)((uint32_t)v >> 8);
        return 3;
    }
    if (enc == ANKER_V_U8) {
        if (cap < 2) { return 0; }
        d[0] = 0x01;
        d[1] = (uint8_t)v;
        return 2;
    }
    if (cap < 1 + extra_n) { return 0; }
    d[0] = 0x04;
    if (extra_n && extra) { memcpy(d + 1, extra, extra_n); }
    return 1 + extra_n;
}

int anker_setter_payload(const anker_job *j, uint32_t epoch, uint8_t *out, size_t cap) {
    if (!j || !out) { return -1; }
    uint8_t reg[8][8];
    size_t rn[8];
    anker_item items[8];
    size_t n = 0;
    static const uint8_t sel[] = { 0x21 };
    put_region(&items[n++], 0xa1, sel, 1);

    if (j->kind == ANKER_JOB_CUSTOM) {
        rn[0] = encode_region(ANKER_V_U8, 2, NULL, 0, reg[0], sizeof reg[0]);
        uint8_t blk[5] = {
            (uint8_t)j->extra[0], (uint8_t)j->extra[1], (uint8_t)j->extra[2],
            (uint8_t)j->extra[3], (uint8_t)j->extra[4]
        };
        rn[1] = encode_region(0, 0, blk, 5, reg[1], sizeof reg[1]);
        put_region(&items[n++], 0xa2, reg[0], rn[0]);
        put_region(&items[n++], 0xa3, reg[1], rn[1]);
    } else if (j->kind == ANKER_JOB_PROTO) {
        uint8_t blk[6];
        for (int i = 0; i < 3; i++) {
            uint32_t v = (uint32_t)j->extra[i];
            blk[i * 2] = (uint8_t)v;
            blk[i * 2 + 1] = (uint8_t)(v >> 8);
        }
        rn[0] = encode_region(0, 0, blk, 6, reg[0], sizeof reg[0]);
        put_region(&items[n++], 0xa2, reg[0], rn[0]);
    } else if (j->kind == ANKER_JOB_ITEMS) {
        for (uint8_t i = 0; i < j->nitems && i < 3; i++) {
            int32_t v = j->items[i].has_const ? j->items[i].konst : j->value;
            rn[i] = encode_region(j->items[i].enc, v, NULL, 0, reg[i], sizeof reg[i]);
            put_region(&items[n++], j->items[i].tag, reg[i], rn[i]);
        }
    } else {
        rn[0] = encode_region(j->enc, j->value, NULL, 0, reg[0], sizeof reg[0]);
        put_region(&items[n++], 0xa2, reg[0], rn[0]);
    }

    uint8_t fe[5];
    if (j->fe == ANKER_FE_TYPED) {
        fe[0] = 0x03;
        anker_le32(epoch, fe + 1);
        put_region(&items[n++], 0xfe, fe, 5);
    } else if (j->fe == ANKER_FE_BARE) {
        anker_le32(epoch, fe);
        put_region(&items[n++], 0xfe, fe, 4);
    }
    return emit_items(items, n, out, cap);
}

int anker_history_request(int more, uint32_t epoch, uint8_t *out, size_t cap) {
    anker_job j;
    memset(&j, 0, sizeof j);
    j.kind = ANKER_JOB_ITEMS;
    j.fe = ANKER_FE_BARE;
    j.nitems = 1;
    j.items[0].tag = 0xa2;
    j.items[0].enc = ANKER_V_U8;
    j.value = more ? 1 : 0;
    return anker_setter_payload(&j, epoch, out, cap);
}

int anker_poll_payload(uint32_t epoch, uint8_t *out, size_t cap) {
    uint8_t fe[5];
    fe[0] = 0x03;
    anker_le32(epoch, fe + 1);
    static const uint8_t sel[] = { 0x21 };
    anker_item items[2];
    put_region(&items[0], 0xa1, sel, 1);
    put_region(&items[1], 0xfe, fe, 5);
    return emit_items(items, 2, out, cap);
}

static void le16(uint8_t *d, uint16_t v) {
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
}

int anker_ss_select(uint32_t id, uint32_t crc, uint8_t mode, uint32_t epoch, uint8_t *out, size_t cap) {
    uint8_t a3[2] = { 0x01, mode };
    uint8_t a4[5] = { 0x04, 0, 0, 0, 0 };
    uint8_t a5[5] = { 0x04, 0, 0, 0, 0 };
    uint8_t fe[5];
    static const uint8_t sel[] = { 0x21 };
    static const uint8_t fd[] = { 0x00, 'S', 'm', 'a', 'l', 'l', 'C', 'h', 'a', 'r', 'g', 'i', 'n', 'g', 'U', 'r', 'l' };
    anker_le32(id, a4 + 1);
    anker_le32(crc, a5 + 1);
    fe[0] = 0x03;
    anker_le32(epoch, fe + 1);
    anker_item it[6];
    put_region(&it[0], 0xa1, sel, 1);
    put_region(&it[1], 0xa3, a3, 2);
    put_region(&it[2], 0xa4, a4, 5);
    put_region(&it[3], 0xa5, a5, 5);
    put_region(&it[4], 0xfd, fd, sizeof fd);
    put_region(&it[5], 0xfe, fe, 5);
    return emit_items(it, 6, out, cap);
}

int anker_ss_begin(uint32_t id, uint32_t crc, uint32_t size, uint16_t count, uint16_t plen,
                   uint8_t window, uint32_t epoch, uint8_t *out, size_t cap) {
    static const uint8_t sel[] = { 0x21 };
    static const uint8_t a2[] = { 0x01, 0x01 };
    uint8_t a3[5] = { 0x04 }, a4[5] = { 0x04 }, a5[5] = { 0x03 };
    uint8_t a6[2] = { 0x01, window };
    uint8_t a7[3] = { 0x02, 0, 0 }, a8[3] = { 0x02, 0, 0 }, fe[5];
    anker_le32(id, a3 + 1);
    anker_le32(crc, a4 + 1);
    anker_le32(size, a5 + 1);
    le16(a7 + 1, plen);
    le16(a8 + 1, count);
    fe[0] = 0x03;
    anker_le32(epoch, fe + 1);
    anker_item it[9];
    put_region(&it[0], 0xa1, sel, 1);
    put_region(&it[1], 0xa2, a2, 2);
    put_region(&it[2], 0xa3, a3, 5);
    put_region(&it[3], 0xa4, a4, 5);
    put_region(&it[4], 0xa5, a5, 5);
    put_region(&it[5], 0xa6, a6, 2);
    put_region(&it[6], 0xa7, a7, 3);
    put_region(&it[7], 0xa8, a8, 3);
    put_region(&it[8], 0xfe, fe, 5);
    return emit_items(it, 9, out, cap);
}

int anker_ss_data(uint16_t npack, const uint8_t *data, size_t dlen, uint8_t *out, size_t cap) {
    static const uint8_t sel[] = { 0x21 };
    uint8_t a2[3] = { 0x02, 0, 0 };
    le16(a2 + 1, npack);
    /* a3's value region is 04 || packet. Cap is the caller's. Build via emit
     * with the type byte already in a side buffer only when the packet is
     * small; larger packets are prefixed here into a 202-byte local. */
    uint8_t a3[1 + 200];
    if (dlen > 200) { return -1; }
    a3[0] = 0x04;
    if (dlen && data) { memcpy(a3 + 1, data, dlen); }
    anker_item it[3];
    put_region(&it[0], 0xa1, sel, 1);
    put_region(&it[1], 0xa2, a2, 3);
    put_region(&it[2], 0xa3, a3, 1 + dlen);
    return emit_items(it, 3, out, cap);
}

int anker_ss_received(const uint8_t *body, size_t len) {
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t tag = body[i], n = body[i + 1];
        if (i + 2 + n > len) { break; }
        if (tag == 0xa2 && n == 3) { return (int)body[i + 3] | ((int)body[i + 4] << 8); }
        i += (size_t)n + 2;
    }
    return -1;
}
