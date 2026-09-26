#include "codec/anker_frame.h"

#include <string.h>

static uint8_t xor_bytes(const uint8_t *b, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) c ^= b[i];
    return c;
}

bool anker_frame_parse(const uint8_t *buf, size_t len, anker_frame_t *out) {
    if (!buf || !out || len < ANKER_FRAME_OVERHEAD) return false;
    if (buf[0] != 0xFF || buf[1] != 0x09) return false;

    memset(out, 0, sizeof(*out));
    uint16_t length_field = (uint16_t)(buf[2] | (buf[3] << 8));
    out->version = buf[4];
    out->slave_id = buf[5];
    out->function_type = buf[6];

    uint8_t b7 = buf[7];
    out->feature_byte = b7 & 0xF0;
    out->fragmented = (b7 & ANKER_FEAT_FRAGMENT) != 0;
    out->encrypted = (b7 & ANKER_FEAT_ENCRYPTED) != 0;
    out->opcode_hi = b7 & ANKER_OPCODE_HI_MASK;
    out->opcode_lo = buf[8];
    out->opcode = (uint16_t)((out->opcode_hi << 8) | out->opcode_lo);
    out->is_response = (out->opcode_hi & ANKER_RESPONSE_HI_BIT) != 0;

    out->length_ok = (length_field == len);
    out->checksum_ok = (xor_bytes(buf, len - 1) == buf[len - 1]);

    const uint8_t *body = buf + 9;
    uint16_t body_len = (uint16_t)(len - 10); // minus 9-byte header, minus checksum

    if (out->fragmented && body_len >= 1) {
        out->frag_index = body[0] >> 4;
        out->frag_count = body[0] & 0x0F;
        body += 1;
        body_len -= 1;
    }
    out->payload = body;
    out->payload_len = body_len;
    return true;
}

int anker_frame_build(uint8_t function_type, uint16_t opcode, bool encrypted,
                      uint8_t slave_id, const uint8_t *payload, size_t n,
                      uint8_t *out, size_t cap) {
    size_t total = n + ANKER_FRAME_OVERHEAD;
    if (!out || total > cap || total > 0xFFFF) return -1;

    uint8_t opcode_hi = (uint8_t)((opcode >> 8) & ANKER_OPCODE_HI_MASK);
    uint8_t feature = encrypted ? ANKER_FEAT_ENCRYPTED : 0x00;

    out[0] = 0xFF;
    out[1] = 0x09;
    out[2] = (uint8_t)(total & 0xFF);
    out[3] = (uint8_t)((total >> 8) & 0xFF);
    out[4] = 0x03;
    out[5] = slave_id;
    out[6] = function_type;
    out[7] = (uint8_t)(feature ^ opcode_hi);
    out[8] = (uint8_t)(opcode & 0xFF);
    if (n && payload) memcpy(out + 9, payload, n);
    out[total - 1] = xor_bytes(out, total - 1);
    return (int)total;
}

// ---- reassembly ------------------------------------------------------------

void anker_reasm_reset(anker_reasm_t *r) {
    if (r) memset(r, 0, sizeof(*r));
}

int anker_reasm_push(anker_reasm_t *r, const anker_frame_t *f, const uint8_t **out) {
    if (!f) return 0;
    if (!f->fragmented) {
        if (out) *out = f->payload;
        return f->payload_len;
    }
    if (!r) return 0;

    // new group?
    if (r->opcode != f->opcode || r->expect_count != f->frag_count || f->frag_index == 1) {
        anker_reasm_reset(r);
        r->opcode = f->opcode;
        r->expect_count = f->frag_count;
    }

    uint8_t idx = f->frag_index; // 1-based
    if (idx < 1 || idx > 8) return 0;
    if (r->len + f->payload_len > ANKER_REASM_MAX) {
        anker_reasm_reset(r);
        return 0;
    }
    // fragments arrive in order on the C1000; append
    memcpy(r->buf + r->len, f->payload, f->payload_len);
    r->len += f->payload_len;
    r->have_mask |= (uint8_t)(1u << (idx - 1));

    uint8_t full = (uint8_t)((1u << r->expect_count) - 1);
    if ((r->have_mask & full) == full && r->expect_count) {
        int n = r->len;
        if (out) *out = r->buf;
        // leave data in place; caller copies before next push
        return n;
    }
    return 0;
}
