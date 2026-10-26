/* Product tables, setter checks and report decode.
 * Field names were copied from the former Lua model tables. Those files are
 * gone. Unknown tags keep the unknown_* names. No pex.h. */
#ifndef ANKER_MODELS_H
#define ANKER_MODELS_H
#include <stddef.h>
#include <stdint.h>
#include "anker_wire.h"

#define ANKER_MODEL_A1761 1
#define ANKER_MODEL_A2687 2

typedef struct {
    const char *name;
    uint16_t opcode;
    uint8_t kind;
    uint8_t fe;
    uint8_t enc;
    uint8_t busy_ok;
    uint8_t no_readback;
    uint8_t verified;
    uint8_t nvalues;
    uint8_t nnames;
    uint8_t nitems;
    int32_t minv, maxv;
    const int32_t *values;
    const char *const *name_keys;
    const int32_t *name_vals;
    struct {
        uint8_t tag, enc, has_const;
        int32_t konst;
    } items[3];
} anker_setter;

typedef struct {
    int is_int, is_text, is_obj;
    int32_t i;
    int32_t mode, auto_exit, c1, c2, c3;
    int have_mode, have_auto, have_c1, have_c2, have_c3;
    char text[16];
} anker_val;

typedef struct {
    uint8_t tag;
    const uint8_t *raw;
    uint16_t raw_len;
    const uint8_t *val;
    uint16_t val_len;
    int has_num;
    int64_t ival;
} anker_in;

typedef int (*anker_next_fn)(void *st, anker_in *item);

typedef struct {
    void *ctx;
    void (*str)(void *ctx, const char *key, const char *v);
    void (*i64)(void *ctx, const char *key, int64_t v);
    void (*num)(void *ctx, const char *key, double v);
    void (*boolean)(void *ctx, const char *key, int v);
    void (*obj)(void *ctx, const char *key);
    void (*end)(void *ctx);
    void (*hex)(void *ctx, const char *key, const void *p, size_t n);
} anker_emit;

int anker_lookup(const char *product_type, char code[8], char name[64], int *inferred);
const char *anker_country(const char *code, const char *sku);

int anker_model_id(const char *code);
int anker_model_verified(int id);
int anker_model_post_auth(int id);
int anker_model_screensaver(int id);
int anker_model_history(int id);
uint16_t anker_model_setter_ft(int id);
int anker_model_poll(int id, uint16_t *ft, uint16_t *op, uint16_t *want, uint32_t *period_ms);
int anker_is_report(int id, uint16_t opcode);

const anker_setter *anker_setters(int id, int *n);
const anker_setter *anker_setter_by_name(int id, const char *field);
/* 0 and fills *job, or -1 and a static message in *err. */
int anker_setter_check(const anker_setter *s, const anker_val *v, anker_job *job, const char **err);

/* want_unknown: include up to 8 unknown tags, once per session. */
int anker_decode(int id, anker_next_fn next, void *st, int want_unknown, const anker_emit *em, int *tlv_count);
#endif
