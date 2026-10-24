/* Anker frame, TLV and session crypto for PEX modules.
 * The functions are the symbols pex_host_codec.c (generated) points at.
 * uECC stays in the firmware: a module that linked it would carry a second
 * copy and a larger stack. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "autoconf.h"
#include "pex/pex_gw.h"
#include "codec/anker_frame.h"
#include "codec/anker_tlv.h"
#include "crypto/anker_crypto.h"
#include "crypto/ghash.h"

_Static_assert(sizeof(anker_reasm_t) <= PEX_CODEC_REASM_SIZE, "reasm state grew");
_Static_assert(ANKER_TLV_STRING == PEX_CODEC_TLV_STRING, "tlv kind");
_Static_assert(ANKER_TLV_U8 == PEX_CODEC_TLV_U8, "tlv kind");
_Static_assert(ANKER_TLV_U16 == PEX_CODEC_TLV_U16, "tlv kind");
_Static_assert(ANKER_TLV_U32 == PEX_CODEC_TLV_U32, "tlv kind");
_Static_assert(ANKER_TLV_F32 == PEX_CODEC_TLV_F32, "tlv kind");
_Static_assert(ANKER_TLV_BYTES == PEX_CODEC_TLV_BYTES, "tlv kind");
_Static_assert(ANKER_TLV_UNKNOWN == PEX_CODEC_TLV_UNKNOWN, "tlv kind");
_Static_assert(ANKER_ENC_STRING == PEX_CODEC_ENC_STRING, "enc");
_Static_assert(ANKER_ENC_U8 == PEX_CODEC_ENC_U8, "enc");
_Static_assert(ANKER_ENC_U16 == PEX_CODEC_ENC_U16, "enc");
_Static_assert(ANKER_ENC_U32 == PEX_CODEC_ENC_U32, "enc");
_Static_assert(ANKER_ENC_F32 == PEX_CODEC_ENC_F32, "enc");
_Static_assert(ANKER_ENC_BYTES == PEX_CODEC_ENC_BYTES, "enc");
/* The type byte is not the enum. f32's byte is 0x05 and bytes' is 0x04. */
_Static_assert(PEX_CODEC_ENC_F32 == 4 && PEX_CODEC_ENC_BYTES == 5, "enc order");

static void fill_tlv(pex_codec_tlv_t *o, const anker_tlv_item_t *it, const uint8_t *base, int neg) {
    memset(o, 0, sizeof *o);
    o->tag = it->tag;
    o->type = (uint8_t)it->type;
    o->type_byte = it->type_byte;
    o->ival = it->ival;
    if (neg) {
        o->raw = base + it->val_off;
        o->raw_len = it->val_len;
        o->val = o->raw;
        o->val_len = it->val_len;
    } else {
        o->raw = anker_tlv_raw(it, base);
        o->raw_len = anker_tlv_raw_len(it);
        o->val = base + it->val_off;
        o->val_len = it->val_len;
    }
}

int pex_codec_frame_parse(const uint8_t *buf, size_t len, pex_codec_frame_t *out) {
    if (!buf || !out) { return PEX_EINVAL; }
    anker_frame_t f;
    if (!anker_frame_parse(buf, len, &f)) { return 0; }
    memset(out, 0, sizeof *out);
    out->version = f.version;
    out->slave_id = f.slave_id;
    out->function_type = f.function_type;
    out->encrypted = f.encrypted ? 1 : 0;
    out->fragmented = f.fragmented ? 1 : 0;
    out->is_response = f.is_response ? 1 : 0;
    out->frag_index = f.frag_index;
    out->frag_count = f.frag_count;
    out->checksum_ok = f.checksum_ok ? 1 : 0;
    out->length_ok = f.length_ok ? 1 : 0;
    out->opcode = f.opcode;
    out->payload_len = f.payload_len;
    out->payload = f.payload;
    return 1;
}

int pex_codec_frame_build(uint8_t function_type, uint16_t opcode, int encrypted, uint8_t slave_id,
                          const uint8_t *payload, size_t n, uint8_t *out, size_t cap) {
    int w = anker_frame_build(function_type, opcode, encrypted != 0, slave_id, payload, n, out, cap);
    return w < 0 ? PEX_ENOSPC : w;
}

void pex_codec_reasm_reset(void *st) {
    if (st) { anker_reasm_reset(st); }
}

int pex_codec_reasm_push(void *st, const pex_codec_frame_t *f, const uint8_t **out, size_t *outlen) {
    if (!st || !f) { return PEX_EINVAL; }
    anker_frame_t a;
    memset(&a, 0, sizeof a);
    a.opcode = f->opcode;
    a.fragmented = f->fragmented != 0;
    a.frag_index = f->frag_index;
    a.frag_count = f->frag_count;
    a.payload = f->payload;
    a.payload_len = f->payload_len;
    const uint8_t *p = NULL;
    int n = anker_reasm_push(st, &a, &p);
    if (out) { *out = n > 0 ? p : NULL; }
    if (outlen) { *outlen = n > 0 ? (size_t)n : 0; }
    return n;
}

int pex_codec_tlv_next(const uint8_t **body, size_t *len, pex_codec_tlv_t *out) {
    if (!body || !*body || !len || !out) { return PEX_EINVAL; }
    anker_tlv_item_t it;
    int next = anker_tlv_next(*body, *len, 0, &it);
    if (next < 0) { return 0; }
    fill_tlv(out, &it, *body, 0);
    *body += next;
    *len -= (size_t)next;
    return 1;
}

int pex_codec_tlv_neg(const uint8_t **body, size_t *len, pex_codec_tlv_t *out) {
    if (!body || !*body || !len || !out) { return PEX_EINVAL; }
    /* anker_tlv_parse_negotiation with cap 1 still walks the whole body, so
     * one item is decoded here by the same rules as that function. */
    const uint8_t *b = *body;
    size_t n = *len;
    if (n < 2) { return 0; }
    uint8_t tag = b[0], nn = b[1];
    if ((size_t)nn + 2 > n) { return 0; }
    anker_tlv_item_t items[1];
    size_t count = 0;
    uint8_t one[2 + 255];
    if ((size_t)nn + 2 > sizeof one) { return PEX_EINVAL; }
    memcpy(one, b, (size_t)nn + 2);
    if (anker_tlv_parse_negotiation(one, (size_t)nn + 2, items, 1, &count) < 0 || count != 1) {
        return 0;
    }
    fill_tlv(out, &items[0], one, 1);
    /* val/raw pointed into `one`. Repoint them at the caller's buffer. */
    out->raw = b + 2;
    out->val = b + 2;
    *body += (size_t)nn + 2;
    *len -= (size_t)nn + 2;
    return 1;
}

int pex_codec_tlv_build(const pex_codec_spec_t *specs, size_t n, uint8_t *out, size_t cap) {
    if (!specs || !out) { return PEX_EINVAL; }
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        anker_tlv_spec_t s;
        memset(&s, 0, sizeof s);
        s.tag = specs[i].tag;
        s.enc = (anker_enc_t)specs[i].enc;
        s.type_byte = specs[i].type_byte;
        s.ival = specs[i].ival;
        s.data = specs[i].data;
        s.data_len = specs[i].data_len;
        int w = anker_tlv_build_item(&s, out + used, cap - used);
        if (w < 0) { return PEX_ENOSPC; }
        used += (size_t)w;
    }
    return (int)used;
}

int pex_codec_pubkey(const uint8_t *d, uint8_t *xy) {
    if (!d || !xy) { return PEX_EINVAL; }
    return anker_pubkey_xy(d, xy) == 0 ? 64 : PEX_EIO;
}

int pex_codec_ecdh(const uint8_t *d, const uint8_t *peer, size_t peer_len, uint8_t *Z) {
    if (!d || !peer || !Z) { return PEX_EINVAL; }
    return anker_ecdh_z(d, peer, peer_len, Z) == 0 ? 32 : PEX_EIO;
}

int pex_codec_gcm_encrypt(const uint8_t *key, size_t keylen, const uint8_t *iv12, const uint8_t *aad,
                          size_t aad_len, const uint8_t *pt, size_t n, uint8_t *out, size_t cap) {
    int w = anker_aes_gcm_encrypt(key, keylen, iv12, aad, aad_len, pt, n, out, cap);
    return w < 0 ? PEX_EINVAL : w;
}

int pex_codec_gcm_decrypt(const uint8_t *key, size_t keylen, const uint8_t *iv12, const uint8_t *body,
                          size_t n, uint8_t *out, size_t cap) {
    int w = anker_aes_gcm_decrypt(key, keylen, iv12, body, n, out, cap);
    return w < 0 ? PEX_EINVAL : w;
}

void pex_codec_gcm_tag(const uint8_t *key, size_t keylen, const uint8_t *iv12, const uint8_t *aad,
                       size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *tag) {
    if (!key || !iv12 || !tag) { return; }
    anker_aes_gcm_tag(key, keylen, iv12, aad, aad_len, ct, ct_len, tag);
}

int pex_codec_cbc(int encrypt, const uint8_t *key, size_t keylen, const uint8_t *iv, const uint8_t *in,
                  size_t n, uint8_t *out, size_t cap, size_t *outlen) {
    size_t ol = 0;
    int rc = anker_aes_cbc(encrypt != 0, key, keylen, iv, in, n, out, cap, &ol);
    if (outlen) { *outlen = rc == 0 ? ol : 0; }
    return rc == 0 ? (int)ol : PEX_EINVAL;
}

int pex_codec_kdf(const uint8_t *Z, uint8_t *out, size_t outlen) {
    if (!Z || !out) { return PEX_EINVAL; }
    return anker_kdf(Z, out, outlen) == 0 ? (int)outlen : PEX_EINVAL;
}

int pex_codec_account_key(const char *id, uint8_t *key, uint8_t *iv) {
    if (!id || !key || !iv) { return PEX_EINVAL; }
    return anker_derive_account_key(id, key, iv) == 0 ? 32 : PEX_EINVAL;
}

void pex_codec_ghash(const uint8_t *H, const uint8_t *aad, size_t aad_len, const uint8_t *ct,
                     size_t ct_len, uint8_t *out) {
    if (!H || !out) { return; }
    ghash_aad_ct(H, aad, aad_len, ct, ct_len, out);
}
