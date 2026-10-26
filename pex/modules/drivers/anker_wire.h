/* Payloads and the negotiation keystream. No pex.h.
 *
 * anker_tlv_emit is supplied by the caller: the module sends the items
 * through the host codec, the host test through anker_tlv_build_payload.
 * That is what keeps the wire bytes in one place.
 *
 * Epoch here is 1700000000 + ms/1000, the base the former link helper used.
 * A base of 0 is a different helper and is not this clock. */
#ifndef ANKER_WIRE_H
#define ANKER_WIRE_H
#include <stddef.h>
#include <stdint.h>

#define ANKER_ENC_STRING 0
#define ANKER_ENC_U8 1
#define ANKER_ENC_U16 2
#define ANKER_ENC_U32 3
#define ANKER_ENC_F32 4
#define ANKER_ENC_BYTES 5

#define ANKER_KEYSTREAM_LEN 67

typedef struct {
    uint8_t tag;
    uint8_t enc;
    int16_t type_byte; /* -2 omit, -1 default, >= 0 explicit */
    int64_t ival;
    const uint8_t *data;
    uint16_t data_len;
} anker_item;

int anker_tlv_emit(const anker_item *it, size_t n, uint8_t *out, size_t cap);

typedef void (*anker_ghash_fn)(const uint8_t H[16], const uint8_t *aad, size_t aad_len,
                               const uint8_t *ct, size_t ct_len, uint8_t out[16]);
void anker_set_ghash(anker_ghash_fn fn);

uint32_t anker_epoch(uint32_t ms);
void anker_le32(uint32_t v, uint8_t d[4]);
const uint8_t *anker_keystream(void);
const uint8_t *anker_ghash_h(void);
const uint8_t *anker_ej0(void);
const uint8_t *anker_aad(void);

/* n+16, or -1 when n exceeds the keystream (trap 5) or ghash is unset. */
int anker_neg_encrypt(const uint8_t *pt, size_t n, uint8_t *out, size_t cap);
int anker_neg_decrypt(const uint8_t *body, size_t n, uint8_t *out, size_t cap);
int anker_nak_poison(const uint8_t *body, size_t n);

/* Inner value-region encodings, the encode() in setter.lua. Not TLV encs. */
#define ANKER_V_U8 1
#define ANKER_V_U16 2
#define ANKER_V_U32 3
#define ANKER_V_U32B 6

#define ANKER_FE_TYPED 0
#define ANKER_FE_BARE 1
#define ANKER_FE_NONE 2

#define ANKER_JOB_INT 0
#define ANKER_JOB_CUSTOM 1
#define ANKER_JOB_PROTO 2
#define ANKER_JOB_ITEMS 3

typedef struct {
    uint8_t kind;
    uint8_t fe;
    uint8_t enc;
    uint8_t nitems;
    struct {
        uint8_t tag;
        uint8_t enc;
        uint8_t has_const;
        int32_t konst;
    } items[3];
    int32_t value;
    int32_t extra[5]; /* custom: mode, auto_exit, c1, c2, c3. protocols: c1, c2, c3 */
} anker_job;

int anker_setter_payload(const anker_job *j, uint32_t epoch, uint8_t *out, size_t cap);
int anker_history_request(int more, uint32_t epoch, uint8_t *out, size_t cap);
int anker_poll_payload(uint32_t epoch, uint8_t *out, size_t cap);

int anker_ss_select(uint32_t id, uint32_t crc, uint8_t mode, uint32_t epoch, uint8_t *out, size_t cap);
int anker_ss_begin(uint32_t id, uint32_t crc, uint32_t size, uint16_t count, uint16_t plen,
                   uint8_t window, uint32_t epoch, uint8_t *out, size_t cap);
int anker_ss_data(uint16_t n, const uint8_t *data, size_t dlen, uint8_t *out, size_t cap);
/* The u16 in an ack's a2, or -1. The scan is screensaver.lua's, not a guess. */
int anker_ss_received(const uint8_t *body, size_t len);
#endif
