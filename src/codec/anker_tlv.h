// Charging-dialect TLV codec — C mirror of web/js/protocol/charging-tlv.js
// (parseChargingPayload / parseNegotiationPayload / buildChargingItem /
// encodeChargingValue) and web/js/protocol/telemetry.js (parseTelemetryDetailed's
// status strip + printable-ASCII promotion, tlvLooksAnomalous).
//
// Charging item on the wire:
//   tag(1) · length(1|2, LE) · [typeByte(1)] · value(bytes, LE)
//   length counts typeByte + value.  A one-byte length is the default.
//   length == 1  -> a bare selector byte, no separate value (e.g. a1 01 21).
//   length == 0  -> empty item.
//
// The first value byte doubles as a type tag:
//   0x00 str · 0x01/02/03 unsigned LE · 0x04/06 bytes · 0x05 f32 LE ·
//   anything else -> raw bytes, the "type" byte kept in the value.
//
// Pure C, no ESP-IDF deps — unit-tested on host against the JS reference.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Decoded type of a TLV item — the value web/js telemetry.js exposes as
// `backendType`, i.e. KIND_TO_BACKEND applied to charging-tlv.js `kind` *after*
// the printable-ASCII raw->string promotion.
typedef enum {
    ANKER_TLV_STRING = 0,
    ANKER_TLV_U8,
    ANKER_TLV_U16,
    ANKER_TLV_U32,
    ANKER_TLV_F32,
    ANKER_TLV_BYTES,
    ANKER_TLV_UNKNOWN, // charging-tlv.js `kind == "raw"` / backendType "unknown"
} anker_tlv_type_t;

// "u8" / "u16" / "u32" / "f32" / "string" / "bytes" / "unknown"
const char *anker_tlv_type_name(anker_tlv_type_t t);

typedef struct {
    uint16_t off;       // item start within the parsed body
    uint8_t tag;        // e.g. 0xA2
    uint8_t len;        // TLV length byte n (counts type byte + value)
    int16_t type_byte;  // body[off+2], or -1 when len == 0

    anker_tlv_type_t type; // promoted / backend type (what the API emits)
    bool promoted;         // charging-tlv.js kind was "raw", promoted to STRING

    int64_t ival; // U8/U16/U32 decoded value (LE); 0 otherwise
    double fval;  // F32 decoded value, Math.round(x*1000)/1000 like JS; 0 otherwise

    // Value slice within the body, interpreted per `type`:
    //   STRING (type byte 0x00) : bytes after the type byte, trailing NUL trimmed
    //   STRING (promoted)       : the whole value region, incl. the "type" byte
    //   BYTES                   : bytes after the type byte
    //   UNKNOWN                 : the whole value region, incl. the type byte
    //   U8/U16/U32/F32          : the whole value region, incl. the type byte
    uint16_t val_off;
    uint16_t val_len;
} anker_tlv_item_t;

// rawHex source (charging-tlv.js `rawHex`) = the value region *including* the type
// byte: body[off+2 .. off+2+len].
static inline const uint8_t *anker_tlv_raw(const anker_tlv_item_t *it,
                                           const uint8_t *body) {
    return body + it->off + 2;
}
static inline uint16_t anker_tlv_raw_len(const anker_tlv_item_t *it) {
    return it->len;
}

// Parse a status-stripped, decrypted charging TLV body. One pass, folding
// charging-tlv.js parseChargingPayload and telemetry.js parseTelemetryDetailed's
// promotion together.
//   items/cap : caller scratch; parsing stops after `cap` items.
//   *n_out    : number of items produced (may be NULL).
// Returns bytes consumed (JS `consumed`); `trailing` = len - consumed. -1 on bad
// args.
int anker_tlv_parse(const uint8_t *body, size_t len, anker_tlv_item_t *items,
                    size_t cap, size_t *n_out);

// Find one tagged item without a scratch array (O(1) stack) — for comparing a
// single tag against a previously-cached body, e.g. the telemetry change
// detector, without re-parsing the whole thing into a second items[] array.
// Returns true and fills *out if found.
bool anker_tlv_find_tag(const uint8_t *body, size_t len, uint8_t tag,
                        anker_tlv_item_t *out);

// Negotiation-dialect parse (charging-tlv.js parseNegotiationPayload):
// [tag][len][value], no type byte. Type is inferred — fully printable ASCII ->
// STRING; else a 1/2/4-byte value -> U8/U16/U32 LE; else BYTES. `val_off`/`val_len`
// span the raw value (there is no type byte to skip here).
int anker_tlv_parse_negotiation(const uint8_t *body, size_t len,
                                anker_tlv_item_t *items, size_t cap,
                                size_t *n_out);

// Status-strip head of telemetry.js parseTelemetryDetailed:
//   mode > 0  : always drop one leading byte  (responses, opcode hi & 0x08 -> true)
//   mode == 0 : never                          (statusByte:false)
//   mode < 0  : "auto" — drop one iff body[0] == 0x00
// Advances *body / *len in place. Returns bytes stripped (0 or 1).
int anker_tlv_status_strip(const uint8_t **body, size_t *len, int mode);

// "This doesn't look like a real TLV parse" soft signal (telemetry.js
// tlvLooksAnomalous): n < 5 || unknown > max(2, n/3), unknown = items whose
// `type` is ANKER_TLV_UNKNOWN.
bool anker_tlv_looks_anomalous(const anker_tlv_item_t *items, size_t n);

// ---- encode --------------------------------------------------------------------

// Logical encoder type for a built item — the simple subset of charging-tlv.js
// encodeChargingValue. const / epoch_s / u8s / u16s / skipWhenEmpty stay in
// field-dict.js and reach the firmware as a prebuilt tlvHex.
typedef enum {
    ANKER_ENC_STRING = 0,
    ANKER_ENC_U8,
    ANKER_ENC_U16,
    ANKER_ENC_U32,
    ANKER_ENC_F32,
    ANKER_ENC_BYTES,
} anker_enc_t;

// ENCODER_TYPE_BYTE: string 0, u8 1, u16 2, u32 3, f32 5, bytes 4.
int anker_enc_default_type_byte(anker_enc_t enc);

typedef struct {
    uint8_t tag;
    anker_enc_t enc;
    int type_byte; // >= 0 explicit; -1 = default for enc; -2 = omit (bare item)
    int64_t ival;  // ANKER_ENC_U8/U16/U32 source
    double fval;   // ANKER_ENC_F32 source
    const uint8_t *data; // ANKER_ENC_STRING / ANKER_ENC_BYTES source
    size_t data_len;
} anker_tlv_spec_t;

// Build one item: tag(1) · len(1) · [typeByte] · value(LE). Returns bytes written
// into out[cap], or -1.
int anker_tlv_build_item(const anker_tlv_spec_t *spec, uint8_t *out, size_t cap);

// Concatenate built items (charging-tlv.js buildChargingPayload, simple path).
// Returns total bytes written into out[cap], or -1.
int anker_tlv_build_payload(const anker_tlv_spec_t *specs, size_t n, uint8_t *out,
                            size_t cap);

// Walk a charging-TLV body in place and overwrite the 4 LE value bytes of every
// 0xFE item with `epoch` — handles the typed `fe 05 03 <e32>` and the bare
// `fe 04 <e32>` forms. Returns the number of FE items stamped. (The firmware
// stamps outbound commands from its own clock just before encrypting; see
// anker_time_stamp_fe.)
int anker_tlv_stamp_fe(uint8_t *body, size_t len, uint32_t epoch);
