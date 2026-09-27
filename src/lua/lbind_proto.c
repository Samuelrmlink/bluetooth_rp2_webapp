/*
 * Lua bindings for the byte-level protocol primitives.
 *
 * These are the C half of the C/Lua split: the frame header and its checksum,
 * both TLV dialects, and the session crypto. Everything above them -- opcodes,
 * field dictionaries, the handshake, the keystream constants -- is Lua, which
 * is the whole point. A protocol under reverse engineering changes daily, and
 * putting that behind a flash erase cycle is what made the previous attempt
 * slow.
 *
 * Nothing here knows an opcode, a UUID or a stage. It moves bytes.
 *
 * Buffers are ordinary Lua strings, which are 8-bit clean and counted, so
 * there is no hex round-trip on the hot path and no NUL truncation. `bytes.hex`
 * exists only because the Bluetooth write binding takes hex.
 */
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "codec/anker_frame.h"
#include "codec/anker_tlv.h"
#include "autoconf.h"
#if CONFIG_PROTO_ANKER_CRYPTO
#include "crypto/anker_crypto.h"
#include "crypto/ghash.h"
#include "lua/lrt.h"
#endif

/* The parse scratch. 96 items at ~40 bytes each is nearly 4 KB, which is more
 * than our stacks want to carry -- solix-esp32 had it as a local and could
 * afford to. File scope, and safe because Lua is single-threaded here and the
 * items never outlive the call that fills them. */
#define TLV_MAX_ITEMS 96
static anker_tlv_item_t s_items[TLV_MAX_ITEMS];

/* Frame assembly scratch. A frame is 10 bytes of overhead around a payload
 * that reassembly caps at ANKER_REASM_MAX. */
static uint8_t s_scratch[ANKER_REASM_MAX + 64];

/* --- bytes ---------------------------------------------------------------- */

static int l_hex(lua_State *L) {
    size_t n = 0;
    const char *b = luaL_checklstring(L, 1, &n);
    luaL_Buffer buf;
    char *p = luaL_buffinitsize(L, &buf, n * 2);
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        p[2 * i] = H[(unsigned char)b[i] >> 4];
        p[2 * i + 1] = H[(unsigned char)b[i] & 0xf];
    }
    luaL_pushresultsize(&buf, n * 2);
    return 1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

static int l_unhex(lua_State *L) {
    size_t n = 0;
    const char *s = luaL_checklstring(L, 1, &n);
    if (n % 2) { lua_pushnil(L); lua_pushstring(L, "odd hex length"); return 2; }
    luaL_Buffer buf;
    char *p = luaL_buffinitsize(L, &buf, n / 2);
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            lua_pushnil(L); lua_pushstring(L, "not hex"); return 2;
        }
        p[i] = (char)((hi << 4) | lo);
    }
    luaL_pushresultsize(&buf, n / 2);
    return 1;
}

/* XOR over the shorter of the two, which is what a keystream wants. */
static int l_xor(lua_State *L) {
    size_t an = 0, bn = 0;
    const char *a = luaL_checklstring(L, 1, &an);
    const char *b = luaL_checklstring(L, 2, &bn);
    size_t n = an < bn ? an : bn;
    luaL_Buffer buf;
    char *p = luaL_buffinitsize(L, &buf, n);
    for (size_t i = 0; i < n; i++) { p[i] = (char)(a[i] ^ b[i]); }
    luaL_pushresultsize(&buf, n);
    return 1;
}

/* --- frame ---------------------------------------------------------------- */

static int l_frame_build(lua_State *L) {
    lua_Integer ft = luaL_checkinteger(L, 1);
    lua_Integer opcode = luaL_checkinteger(L, 2);
    int enc = lua_toboolean(L, 3);
    lua_Integer slave = luaL_optinteger(L, 4, 0);
    size_t n = 0;
    const char *pl = lua_isnoneornil(L, 5) ? NULL : luaL_checklstring(L, 5, &n);

    int total = anker_frame_build((uint8_t)ft, (uint16_t)opcode, enc != 0,
                                  (uint8_t)slave, (const uint8_t *)pl, n,
                                  s_scratch, sizeof s_scratch);
    if (total < 0) { lua_pushnil(L); lua_pushstring(L, "payload too large"); return 2; }
    lua_pushlstring(L, (const char *)s_scratch, (size_t)total);
    return 1;
}

static int l_frame_parse(lua_State *L) {
    size_t n = 0;
    const char *b = luaL_checklstring(L, 1, &n);
    anker_frame_t f;
    if (!anker_frame_parse((const uint8_t *)b, n, &f)) {
        lua_pushnil(L);
        lua_pushstring(L, "not an ff09 frame");
        return 2;
    }
    lua_createtable(L, 0, 14);
#define SETI(k, v) do { lua_pushinteger(L, (lua_Integer)(v)); lua_setfield(L, -2, k); } while (0)
#define SETB(k, v) do { lua_pushboolean(L, (v)); lua_setfield(L, -2, k); } while (0)
    SETI("version", f.version);
    SETI("slave_id", f.slave_id);
    SETI("function_type", f.function_type);
    SETI("opcode", f.opcode);
    SETI("opcode_hi", f.opcode_hi);
    SETI("opcode_lo", f.opcode_lo);
    SETI("frag_index", f.frag_index);
    SETI("frag_count", f.frag_count);
    SETB("encrypted", f.encrypted);
    SETB("fragmented", f.fragmented);
    SETB("is_response", f.is_response);
    SETB("length_ok", f.length_ok);
    SETB("checksum_ok", f.checksum_ok);
#undef SETI
#undef SETB
    lua_pushlstring(L, (const char *)f.payload, f.payload_len);
    lua_setfield(L, -2, "payload");
    return 1;
}

/* Reassembly keeps state, so the caller names a channel rather than us
 * guessing one from the frame -- two links to two devices must not share a
 * reassembler, and the codec has no notion of a link. */
#ifndef PROTO_REASM_CHANNELS
#define PROTO_REASM_CHANNELS 2
#endif
static anker_reasm_t s_reasm[PROTO_REASM_CHANNELS];

static int l_frame_reasm(lua_State *L) {
    lua_Integer chan = luaL_checkinteger(L, 1);
    if (chan < 0 || chan >= PROTO_REASM_CHANNELS) {
        return luaL_error(L, "reasm channel out of range");
    }
    luaL_checktype(L, 2, LUA_TTABLE);

    anker_frame_t f;
    memset(&f, 0, sizeof f);
    lua_getfield(L, 2, "opcode");       f.opcode = (uint16_t)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, 2, "fragmented");   f.fragmented = lua_toboolean(L, -1); lua_pop(L, 1);
    lua_getfield(L, 2, "frag_index");   f.frag_index = (uint8_t)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, 2, "frag_count");   f.frag_count = (uint8_t)lua_tointeger(L, -1); lua_pop(L, 1);
    size_t pn = 0;
    lua_getfield(L, 2, "payload");
    const char *pl = lua_tolstring(L, -1, &pn);
    f.payload = (const uint8_t *)pl;
    f.payload_len = (uint16_t)pn;

    const uint8_t *out = NULL;
    int n = anker_reasm_push(&s_reasm[chan], &f, &out);
    lua_pop(L, 1);                      /* the payload string */
    if (n <= 0 || !out) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, (const char *)out, (size_t)n);
    return 1;
}

static int l_frame_reasm_reset(lua_State *L) {
    lua_Integer chan = luaL_checkinteger(L, 1);
    if (chan >= 0 && chan < PROTO_REASM_CHANNELS) { anker_reasm_reset(&s_reasm[chan]); }
    return 0;
}

/* --- tlv ------------------------------------------------------------------ */

static void push_item(lua_State *L, const anker_tlv_item_t *it, const uint8_t *body) {
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, it->tag);                   lua_setfield(L, -2, "tag");
    char hx[3] = { "0123456789abcdef"[it->tag >> 4], "0123456789abcdef"[it->tag & 0xf], 0 };
    lua_pushstring(L, hx);                         lua_setfield(L, -2, "hex");
    lua_pushstring(L, anker_tlv_type_name(it->type)); lua_setfield(L, -2, "type");
    lua_pushboolean(L, it->promoted);              lua_setfield(L, -2, "promoted");

    switch (it->type) {
        case ANKER_TLV_U8:
        case ANKER_TLV_U16:
        case ANKER_TLV_U32:
            lua_pushinteger(L, (lua_Integer)it->ival);
            break;
        case ANKER_TLV_F32:
            lua_pushnumber(L, (lua_Number)it->fval);
            break;
        default:
            lua_pushlstring(L, (const char *)body + it->val_off, it->val_len);
            break;
    }
    lua_setfield(L, -2, "value");

    /* The value region including the type byte -- the notes' "rawHex". */
    lua_pushlstring(L, (const char *)anker_tlv_raw(it, body), anker_tlv_raw_len(it));
    lua_setfield(L, -2, "raw");
}

/* tlv.parse(body [, negotiation]) -> table
 *
 * The array part is every item in wire order, so a repeated tag is not lost.
 * Tags are ALSO reachable by their two-character hex name (t.a1, t.fe), which
 * cannot collide with the array because those keys are strings.
 */
static int l_tlv_parse(lua_State *L) {
    size_t n = 0;
    const char *body = luaL_checklstring(L, 1, &n);
    int negotiation = lua_toboolean(L, 2);

    size_t count = 0;
    int consumed = negotiation
        ? anker_tlv_parse_negotiation((const uint8_t *)body, n, s_items, TLV_MAX_ITEMS, &count)
        : anker_tlv_parse((const uint8_t *)body, n, s_items, TLV_MAX_ITEMS, &count);
    if (consumed < 0) { lua_pushnil(L); lua_pushstring(L, "bad arguments"); return 2; }

    lua_createtable(L, (int)count, 6);
    for (size_t i = 0; i < count; i++) {
        push_item(L, &s_items[i], (const uint8_t *)body);   /* result, item */
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, (lua_Integer)i + 1);             /* result[i+1] = item */

        /* Also reachable as t.a1 / t.fe. First occurrence wins the named slot;
         * a repeated tag is still in the array part, so nothing is lost. */
        lua_getfield(L, -1, "hex");                         /* result, item, hex */
        const char *hx = lua_tostring(L, -1);
        lua_getfield(L, -3, hx);                            /* ..., existing */
        int taken = !lua_isnil(L, -1);
        lua_pop(L, 1);                                      /* result, item, hex */
        if (!taken) {
            lua_pushvalue(L, -2);                           /* result, item, hex, item */
            lua_setfield(L, -4, hx);                        /* result, item, hex */
        }
        lua_pop(L, 2);                                      /* result */
    }
    lua_pushinteger(L, (lua_Integer)count);              lua_setfield(L, -2, "n");
    lua_pushinteger(L, consumed);                        lua_setfield(L, -2, "consumed");
    lua_pushinteger(L, (lua_Integer)n - consumed);       lua_setfield(L, -2, "trailing");
    lua_pushboolean(L, anker_tlv_looks_anomalous(s_items, count)); lua_setfield(L, -2, "anomalous");
    /* The C parser stops at cap and says nothing about it. Hiding that turns a
     * truncated report into a silently short one, so it is reported. */
    lua_pushboolean(L, count >= TLV_MAX_ITEMS);          lua_setfield(L, -2, "truncated");
    return 1;
}

static int l_tlv_find(lua_State *L) {
    size_t n = 0;
    const char *body = luaL_checklstring(L, 1, &n);
    lua_Integer tag = luaL_checkinteger(L, 2);
    anker_tlv_item_t it;
    if (!anker_tlv_find_tag((const uint8_t *)body, n, (uint8_t)tag, &it)) {
        lua_pushnil(L);
        return 1;
    }
    push_item(L, &it, (const uint8_t *)body);
    return 1;
}

static int l_tlv_strip_status(lua_State *L) {
    size_t n = 0;
    const char *body = luaL_checklstring(L, 1, &n);
    lua_Integer mode = luaL_optinteger(L, 2, -1);
    const uint8_t *p = (const uint8_t *)body;
    size_t len = n;
    int stripped = anker_tlv_status_strip(&p, &len, (int)mode);
    lua_pushlstring(L, (const char *)p, len);
    lua_pushinteger(L, stripped);
    return 2;
}

static int l_tlv_stamp_fe(lua_State *L) {
    size_t n = 0;
    const char *body = luaL_checklstring(L, 1, &n);
    lua_Integer epoch = luaL_checkinteger(L, 2);
    if (n > sizeof s_scratch) { lua_pushnil(L); lua_pushstring(L, "body too large"); return 2; }
    memcpy(s_scratch, body, n);
    int stamped = anker_tlv_stamp_fe(s_scratch, n, (uint32_t)epoch);
    lua_pushlstring(L, (const char *)s_scratch, n);
    lua_pushinteger(L, stamped);
    return 2;
}

static anker_enc_t enc_of(lua_State *L, const char *s) {
    if (!strcmp(s, "u8")) { return ANKER_ENC_U8; }
    if (!strcmp(s, "u16")) { return ANKER_ENC_U16; }
    if (!strcmp(s, "u32")) { return ANKER_ENC_U32; }
    if (!strcmp(s, "f32")) { return ANKER_ENC_F32; }
    if (!strcmp(s, "string")) { return ANKER_ENC_STRING; }
    if (!strcmp(s, "bytes")) { return ANKER_ENC_BYTES; }
    luaL_error(L, "unknown tlv encoding '%s'", s);
    return ANKER_ENC_BYTES;
}

/* tlv.build{ {tag=0xa1, enc="u8", value=0x21, type_byte=false}, ... } */
static int l_tlv_build(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer count = luaL_len(L, 1);

    size_t off = 0;
    for (lua_Integer i = 1; i <= count; i++) {
        lua_geti(L, 1, i);
        luaL_checktype(L, -1, LUA_TTABLE);

        anker_tlv_spec_t spec;
        memset(&spec, 0, sizeof spec);

        lua_getfield(L, -1, "tag");
        spec.tag = (uint8_t)luaL_checkinteger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "enc");
        spec.enc = enc_of(L, luaL_checkstring(L, -1));
        lua_pop(L, 1);

        /* type_byte: absent = the default for the encoding, false = omit it
         * entirely (a bare selector like a1 01 21), a number = that byte. */
        lua_getfield(L, -1, "type_byte");
        if (lua_isnil(L, -1)) {
            spec.type_byte = -1;
        } else if (lua_isboolean(L, -1)) {
            spec.type_byte = lua_toboolean(L, -1) ? -1 : -2;
        } else {
            spec.type_byte = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, -1, "value");
        size_t dl = 0;
        const char *d = NULL;
        switch (spec.enc) {
            case ANKER_ENC_F32:
                spec.fval = (double)luaL_checknumber(L, -1);
                break;
            case ANKER_ENC_STRING:
            case ANKER_ENC_BYTES:
                d = luaL_checklstring(L, -1, &dl);
                spec.data = (const uint8_t *)d;
                spec.data_len = dl;
                break;
            default:
                spec.ival = (int64_t)luaL_checkinteger(L, -1);
                break;
        }

        int w = anker_tlv_build_item(&spec, s_scratch + off, sizeof s_scratch - off);
        lua_pop(L, 2);                  /* value, item table */
        if (w < 0) { lua_pushnil(L); lua_pushstring(L, "tlv payload too large"); return 2; }
        off += (size_t)w;
    }
    lua_pushlstring(L, (const char *)s_scratch, off);
    return 1;
}

/* --- crypto --------------------------------------------------------------- */
#if CONFIG_PROTO_ANKER_CRYPTO

static const uint8_t *checkbuf(lua_State *L, int idx, size_t want, size_t *got) {
    size_t n = 0;
    const char *s = luaL_checklstring(L, idx, &n);
    if (want && n != want) {
        luaL_error(L, "argument %d: expected %d bytes, got %d", idx, (int)want, (int)n);
    }
    if (got) { *got = n; }
    return (const uint8_t *)s;
}

static int l_crypto_pubkey(lua_State *L) {
    const uint8_t *d = checkbuf(L, 1, 32, NULL);
    uint8_t xy[64];
    int rc = anker_pubkey_xy(d, xy);
    lrt_yield_soon();          /* ~41 ms of uninterruptible C; see lrt.h */
    if (rc != 0) { lua_pushnil(L); lua_pushstring(L, "bad private key"); return 2; }
    lua_pushlstring(L, (const char *)xy, 64);
    return 1;
}

static int l_crypto_ecdh(lua_State *L) {
    const uint8_t *d = checkbuf(L, 1, 32, NULL);
    size_t pn = 0;
    const uint8_t *peer = checkbuf(L, 2, 0, &pn);
    uint8_t Z[32];
    int rc = anker_ecdh_z(d, peer, pn, Z);
    lrt_yield_soon();          /* ~41 ms of uninterruptible C; see lrt.h */
    if (rc != 0) { lua_pushnil(L); lua_pushstring(L, "ecdh failed"); return 2; }
    lua_pushlstring(L, (const char *)Z, 32);
    return 1;
}

static int l_crypto_gcm_encrypt(lua_State *L) {
    size_t kn = 0, an = 0, pn = 0;
    const uint8_t *key = checkbuf(L, 1, 0, &kn);
    const uint8_t *iv = checkbuf(L, 2, 12, NULL);
    const uint8_t *aad = checkbuf(L, 3, 0, &an);
    const uint8_t *pt = checkbuf(L, 4, 0, &pn);
    if (pn + 16 > sizeof s_scratch) { lua_pushnil(L); lua_pushstring(L, "too large"); return 2; }
    int n = anker_aes_gcm_encrypt(key, kn, iv, aad, an, pt, pn, s_scratch, sizeof s_scratch);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, "gcm encrypt failed"); return 2; }
    lua_pushlstring(L, (const char *)s_scratch, (size_t)n);
    return 1;
}

static int l_crypto_gcm_decrypt(lua_State *L) {
    size_t kn = 0, bn = 0;
    const uint8_t *key = checkbuf(L, 1, 0, &kn);
    const uint8_t *iv = checkbuf(L, 2, 12, NULL);
    const uint8_t *body = checkbuf(L, 3, 0, &bn);
    if (bn > sizeof s_scratch) { lua_pushnil(L); lua_pushstring(L, "too large"); return 2; }
    int n = anker_aes_gcm_decrypt(key, kn, iv, body, bn, s_scratch, sizeof s_scratch);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, "gcm decrypt failed"); return 2; }
    lua_pushlstring(L, (const char *)s_scratch, (size_t)n);
    return 1;
}

/* The tag a conforming implementation would have produced. Decrypt never
 * checks one -- the device reuses its nonce, so a tag proves nothing about
 * freshness -- but the profile uses this to tell "the charger adopted our
 * key" from "it silently derived a different one". */
static int l_crypto_gcm_tag(lua_State *L) {
    size_t kn = 0, an = 0, cn = 0;
    const uint8_t *key = checkbuf(L, 1, 0, &kn);
    const uint8_t *iv = checkbuf(L, 2, 12, NULL);
    const uint8_t *aad = checkbuf(L, 3, 0, &an);
    const uint8_t *ct = checkbuf(L, 4, 0, &cn);
    uint8_t tag[16];
    anker_aes_gcm_tag(key, kn, iv, aad, an, ct, cn, tag);
    lua_pushlstring(L, (const char *)tag, 16);
    return 1;
}

static int l_crypto_cbc(lua_State *L) {
    int encrypt = lua_toboolean(L, 1);
    size_t kn = 0, dn = 0;
    const uint8_t *key = checkbuf(L, 2, 0, &kn);
    const uint8_t *iv = checkbuf(L, 3, 16, NULL);
    const uint8_t *in = checkbuf(L, 4, 0, &dn);
    size_t out = 0;
    if (anker_aes_cbc(encrypt != 0, key, kn, iv, in, dn, s_scratch, sizeof s_scratch, &out) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "cbc failed");
        return 2;
    }
    lua_pushlstring(L, (const char *)s_scratch, out);
    return 1;
}

/* kdf(Z, outlen) -> bytes. The C1000's AES-256-CBC key/iv come from this;
 * the A2687 never calls it. */
static int l_crypto_kdf(lua_State *L) {
    const uint8_t *Z = checkbuf(L, 1, 32, NULL);
    lua_Integer n = luaL_optinteger(L, 2, 64);
    if (n < 1 || (size_t)n > sizeof s_scratch) {
        lua_pushnil(L); lua_pushstring(L, "bad kdf length"); return 2;
    }
    if (anker_kdf(Z, s_scratch, (size_t)n) != 0) {
        lua_pushnil(L); lua_pushstring(L, "kdf failed"); return 2;
    }
    lua_pushlstring(L, (const char *)s_scratch, (size_t)n);
    return 1;
}

/* account_key(id) -> key(32), iv(16). AesConferV1. */
static int l_crypto_account_key(lua_State *L) {
    const char *id = luaL_checkstring(L, 1);
    uint8_t key[32], iv[16];
    if (anker_derive_account_key(id, key, iv) != 0) {
        lua_pushnil(L); lua_pushstring(L, "account key failed"); return 2;
    }
    lua_pushlstring(L, (const char *)key, sizeof key);
    lua_pushlstring(L, (const char *)iv, sizeof iv);
    return 2;
}

static int l_crypto_ghash(lua_State *L) {
    size_t an = 0, cn = 0;
    const uint8_t *H = checkbuf(L, 1, 16, NULL);
    const uint8_t *aad = checkbuf(L, 2, 0, &an);
    const uint8_t *ct = checkbuf(L, 3, 0, &cn);
    uint8_t s[16];
    ghash_aad_ct(H, aad, an, ct, cn, s);
    lua_pushlstring(L, (const char *)s, 16);
    return 1;
}
#endif /* CONFIG_PROTO_ANKER_CRYPTO */

/* --- registration --------------------------------------------------------- */

static void set_table(lua_State *L, const char *name, const luaL_Reg *fns) {
    /* Not luaL_newlib: its size hint is sizeof(l)/sizeof(l[0]), which on a
     * POINTER argument computes 8/32 - 1 = -1 rather than the entry count.
     * Build the table explicitly instead. */
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, name);
}

void lbind_proto_open(lua_State *L) {
    static const luaL_Reg bytes_fns[] = {
        { "hex", l_hex }, { "unhex", l_unhex }, { "xor", l_xor }, { NULL, NULL }
    };
    set_table(L, "bytes", bytes_fns);

    static const luaL_Reg frame_fns[] = {
        { "build", l_frame_build },
        { "parse", l_frame_parse },
        { "reasm", l_frame_reasm },
        { "reasm_reset", l_frame_reasm_reset },
        { NULL, NULL }
    };
    set_table(L, "frame", frame_fns);

    static const luaL_Reg tlv_fns[] = {
        { "parse", l_tlv_parse },
        { "find", l_tlv_find },
        { "build", l_tlv_build },
        { "strip_status", l_tlv_strip_status },
        { "stamp_fe", l_tlv_stamp_fe },
        { NULL, NULL }
    };
    set_table(L, "tlv", tlv_fns);

#if CONFIG_PROTO_ANKER_CRYPTO
    static const luaL_Reg crypto_fns[] = {
        { "pubkey", l_crypto_pubkey },
        { "ecdh", l_crypto_ecdh },
        { "gcm_encrypt", l_crypto_gcm_encrypt },
        { "gcm_decrypt", l_crypto_gcm_decrypt },
        { "gcm_tag", l_crypto_gcm_tag },
        { "cbc", l_crypto_cbc },
        { "kdf", l_crypto_kdf },
        { "account_key", l_crypto_account_key },
        { "ghash", l_crypto_ghash },
        { NULL, NULL }
    };
    set_table(L, "crypto", crypto_fns);
#endif
}
