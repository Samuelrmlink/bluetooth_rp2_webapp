// Anker "ZX / Charging" BLE frame model — C mirror of web/js/protocol/frame.js.
//
// Wire layout:
//   [0..1]  FF 09
//   [2..3]  total frame length, uint16 LE  (whole frame incl. checksum)
//   [4]     version = 0x03
//   [5]     slaveId
//   [6]     functionType   0x01 = zx/pairing, 0x0F = charging
//   [7]     feature | opcodeHi   (0x80 fragmented, 0x40 encrypted, 0x0F opcode hi nibble)
//   [8]     opcodeLo
//   [9..]   payload  (fragmented: payload[0] is (idx<<4|count); responses carry a
//                     leading status byte before the TLV items)
//   [last]  XOR checksum of every preceding byte
//
// Pure C, no ESP-IDF deps — unit-testable on host.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ANKER_FUNC_ZX 0x01
#define ANKER_FUNC_CHARGING 0x0F
#define ANKER_FUNC_CHARGING_RESP 0x11 // A2687 / mini-charge responses & reports

#define ANKER_FEAT_FRAGMENT 0x80
#define ANKER_FEAT_ENCRYPTED 0x40
#define ANKER_OPCODE_HI_MASK 0x0F
#define ANKER_RESPONSE_HI_BIT 0x08
#define ANKER_RESPONSE_BIT 0x0800
#define ANKER_FRAME_OVERHEAD 10

typedef struct {
    uint8_t version;
    uint8_t slave_id;
    uint8_t function_type;
    uint8_t feature_byte; // byte7 & 0xF0
    bool encrypted;
    bool fragmented;
    bool is_response; // opcodeHi & 0x08 -> carries a status byte
    uint16_t opcode;   // (opcodeHi << 8) | opcodeLo
    uint8_t opcode_hi;
    uint8_t opcode_lo;
    uint8_t frag_index; // 1-based, 0 if not fragmented
    uint8_t frag_count; // 0 if not fragmented
    const uint8_t *payload; // points into the caller's buffer (after any frag header)
    uint16_t payload_len;
    bool length_ok;   // length field == buffer length
    bool checksum_ok; // XOR of all preceding bytes == last byte
} anker_frame_t;

// Returns false if buf is not a well-formed FF09 frame (too short / bad magic).
// length_ok / checksum_ok are still reported for a structurally-parseable frame.
bool anker_frame_parse(const uint8_t *buf, size_t len, anker_frame_t *out);

// Build a single (non-fragmented) frame into out[cap]. Returns total length or -1.
int anker_frame_build(uint8_t function_type, uint16_t opcode, bool encrypted,
                      uint8_t slave_id, const uint8_t *payload, size_t n,
                      uint8_t *out, size_t cap);

static inline uint16_t anker_response_opcode(uint16_t request) {
    return (uint16_t)((request | ANKER_RESPONSE_BIT) & 0xFFFF);
}

// ---- fragment reassembly ------------------------------------------------

#define ANKER_REASM_MAX 640 // C1000 full status is ~330B decrypted; ciphertext a bit more

typedef struct {
    uint16_t opcode;
    uint8_t expect_count;
    uint8_t have_mask; // bit i set = fragment i+1 seen
    uint16_t len;
    uint8_t buf[ANKER_REASM_MAX];
} anker_reasm_t;

void anker_reasm_reset(anker_reasm_t *r);

// Feed one parsed frame. Returns:
//   0  = not fragmented, or still waiting for more fragments
//   >0 = reassembly complete; *out points at r->buf, return value is its length
// A non-fragmented frame passes through: returns f->payload_len with *out = f->payload.
int anker_reasm_push(anker_reasm_t *r, const anker_frame_t *f, const uint8_t **out);
