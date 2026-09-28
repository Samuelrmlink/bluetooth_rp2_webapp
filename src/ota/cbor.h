/*
 * A CBOR codec the size of what SMP actually uses.
 *
 * MCUmgr frames are one CBOR map of a dozen keys with unsigned integers,
 * text and byte strings, booleans and the odd nested map. A full library
 * (QCBOR, TinyCBOR) would be ten times this file for the same six shapes,
 * and none of them is in the tree today. So: an encoder that appends into a
 * fixed buffer and remembers whether it overflowed, and a decoder that finds
 * a key in a map and can step over anything it does not understand.
 *
 * Encoding uses INDEFINITE-length maps and arrays (0xbf ... 0xff). Every
 * SMP client decodes those (cbor2, zcbor, ugorji, ciborium) and it lets a
 * handler emit fields as it discovers them instead of counting first.
 */
#ifndef OTA_CBOR_H
#define OTA_CBOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    bool err;          /* set on the first overflow; nothing after it is written */
} cbor_enc_t;

void cbor_enc_init(cbor_enc_t *e, uint8_t *buf, size_t cap);
void cbor_map_open(cbor_enc_t *e);      /* indefinite; pair with cbor_close */
void cbor_array_open(cbor_enc_t *e);
void cbor_close(cbor_enc_t *e);
void cbor_uint(cbor_enc_t *e, uint64_t v);
void cbor_int(cbor_enc_t *e, int64_t v);
void cbor_bool(cbor_enc_t *e, bool v);
void cbor_nil(cbor_enc_t *e);
void cbor_tstr(cbor_enc_t *e, const char *s);
void cbor_bstr(cbor_enc_t *e, const void *p, size_t n);
/* key + value in one call, which is how every SMP field reads */
void cbor_kv_uint(cbor_enc_t *e, const char *k, uint64_t v);
void cbor_kv_int(cbor_enc_t *e, const char *k, int64_t v);
void cbor_kv_bool(cbor_enc_t *e, const char *k, bool v);
void cbor_kv_tstr(cbor_enc_t *e, const char *k, const char *v);
void cbor_kv_bstr(cbor_enc_t *e, const char *k, const void *p, size_t n);

typedef enum {
    CBOR_UINT, CBOR_NINT, CBOR_BSTR, CBOR_TSTR, CBOR_ARRAY, CBOR_MAP,
    CBOR_BOOL, CBOR_NULL, CBOR_OTHER
} cbor_type_t;

typedef struct {
    cbor_type_t type;
    uint64_t u;              /* UINT, or the magnitude-1 for NINT */
    bool b;                  /* BOOL */
    const uint8_t *bytes;    /* BSTR/TSTR payload (not NUL-terminated) */
    size_t len;              /* payload length */
    const uint8_t *raw;      /* the whole encoded item, for nested maps */
    size_t raw_len;
} cbor_val_t;

/* Find `key` (a text string) in the map at [p, p+n). Returns false if the
 * bytes are not a well-formed map or the key is absent. */
bool cbor_map_find(const uint8_t *p, size_t n, const char *key, cbor_val_t *out);
/* Typed conveniences over cbor_map_find. Missing or wrong type => false. */
bool cbor_get_uint(const uint8_t *p, size_t n, const char *key, uint64_t *out);
bool cbor_get_bool(const uint8_t *p, size_t n, const char *key, bool *out);
bool cbor_get_tstr(const uint8_t *p, size_t n, const char *key, char *dst, size_t cap);
bool cbor_get_bstr(const uint8_t *p, size_t n, const char *key, const uint8_t **out, size_t *len);

#endif
