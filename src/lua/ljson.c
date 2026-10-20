/*
 * Lua value -> JSON, with a hard bound.
 *
 * Arrays (1..n with no holes) become [], everything else {}. Strings are
 * escaped through the same function the C side uses, because a value from a
 * script is no more trustworthy than one from the air.
 *
 * Every append goes through add()/addf(), never `n += snprintf(...)`.
 * snprintf returns the length it WOULD have written, so accumulating that lets
 * n run past cap; the next append then computes dst + n, which is past the
 * end, and cap - n, which has wrapped to a huge size_t, and writes there. A
 * table's closing brace is emitted unconditionally, so it lands out of bounds
 * by however far the table overran. Nothing exotic is needed to reach that --
 * an ordinary telemetry report with enough fields does it, and a profile
 * chooses its own field names and count.
 *
 * So the writes clamp, and the fact that they clamped is reported, because
 * half an object on the wire is a parse error in the browser with nothing
 * pointing at the cause.
 */
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"

#include "api/api.h"
#include "lua/ljson.h"

#define MAX_DEPTH 6

typedef struct {
    char  *dst;
    size_t cap;   /* total buffer size; n never reaches it */
    size_t n;
    bool   trunc;
} enc_ctx;

static void add(enc_ctx *e, const char *s) {
    if (e->n + 1 >= e->cap) { e->trunc = true; return; }
    size_t room = e->cap - e->n - 1, len = strlen(s);
    if (len > room) { e->trunc = true; len = room; }
    memcpy(e->dst + e->n, s, len);
    e->n += len;
    e->dst[e->n] = 0;
}

static void addf(enc_ctx *e, const char *fmt, ...) {
    if (e->n + 1 >= e->cap) { e->trunc = true; return; }
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(e->dst + e->n, e->cap - e->n, fmt, ap);
    va_end(ap);
    if (r < 0) { e->trunc = true; return; }
    if ((size_t)r >= e->cap - e->n) { e->trunc = true; e->n = e->cap - 1; return; }
    e->n += (size_t)r;
}

/* api_json_escape clamps and returns what it actually wrote, so it is safe on
 * its own -- but only when handed a length that has not already wrapped. */
static void add_escaped(enc_ctx *e, const char *s) {
    if (e->n + 1 >= e->cap) { e->trunc = true; return; }
    size_t w = api_json_escape(e->dst + e->n, e->cap - e->n, s);
    if (w < strlen(s)) { e->trunc = true; }
    e->n += w;
    e->dst[e->n] = 0;
}

static void enc(lua_State *L, int idx, enc_ctx *e, int depth) {
    if (depth > MAX_DEPTH || e->n + 8 >= e->cap) {
        e->trunc = true;
        add(e, "null");
        return;
    }
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            add(e, "null");
            return;
        case LUA_TBOOLEAN:
            add(e, lua_toboolean(L, idx) ? "true" : "false");
            return;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                addf(e, "%lld", (long long)lua_tointeger(L, idx));
            } else {
                addf(e, "%g", (double)lua_tonumber(L, idx));
            }
            return;
        case LUA_TSTRING:
            add(e, "\"");
            add_escaped(e, lua_tostring(L, idx));
            add(e, "\"");
            return;
        case LUA_TTABLE: {
            lua_Integer len = luaL_len(L, idx);
            if (len > 0) {
                add(e, "[");
                lua_Integer i = 1;
                for (; i <= len && e->n + 4 < e->cap; i++) {
                    if (i > 1) { add(e, ","); }
                    lua_geti(L, idx, i);
                    enc(L, lua_gettop(L), e, depth + 1);
                    lua_pop(L, 1);
                }
                if (i <= len) { e->trunc = true; }
                add(e, "]");
            } else {
                add(e, "{");
                bool first = true;
                int t = lua_absindex(L, idx);
                lua_pushnil(L);
                while (lua_next(L, t) != 0) {
                    if (lua_type(L, -2) == LUA_TSTRING) {
                        if (e->n + 8 < e->cap) {
                            if (!first) { add(e, ","); }
                            add(e, "\"");
                            add_escaped(e, lua_tostring(L, -2));
                            add(e, "\":");
                            enc(L, lua_gettop(L), e, depth + 1);
                            first = false;
                        } else {
                            /* A dropped key is data loss, not a formatting
                             * quirk; the caller has to be able to say so. */
                            e->trunc = true;
                        }
                    }
                    lua_pop(L, 1);
                }
                add(e, "}");
            }
            return;
        }
        default:
            add(e, "null");
    }
}

size_t ljson_encode(lua_State *L, int idx, char *dst, size_t cap, bool *truncated) {
    if (truncated) { *truncated = false; }
    if (!dst || cap == 0) { if (truncated) { *truncated = true; } return 0; }
    enc_ctx e = { .dst = dst, .cap = cap, .n = 0, .trunc = false };
    dst[0] = 0;
    enc(L, idx, &e, 0);
    if (truncated) { *truncated = e.trunc; }
    return e.n;
}

/* --- decode ----------------------------------------------------------------
 * Recursive descent over a length-bounded buffer: the input may be a Lua
 * string with embedded NULs or a body that is not NUL-terminated, so nothing
 * here may rely on a terminator. Every access goes through peek()/eof().
 *
 * The recursion is bounded (LJSON_DECODE_DEPTH): this runs on core0's 4 KB
 * stack, under a Lua coroutine, and a body of 10 000 '[' is something a
 * server can send. Each level holds a few words; string bodies are built in
 * a luaL_Buffer, which spills into the Lua heap rather than onto the stack. */
typedef struct {
    lua_State  *L;
    const char *s;
    size_t      len, i;
    const char *err;
    char        msg[64];
} dec_ctx;

static bool dec_fail(dec_ctx *d, const char *what) {
    if (!d->err) {
        snprintf(d->msg, sizeof d->msg, "%s at byte %u", what, (unsigned)d->i);
        d->err = d->msg;
    }
    return false;
}

static void dec_ws(dec_ctx *d) {
    while (d->i < d->len) {
        char c = d->s[d->i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { break; }
        d->i++;
    }
}

static int dec_hex4(dec_ctx *d) {
    if (d->len - d->i < 4) { return -1; }
    int v = 0;
    for (int k = 0; k < 4; k++) {
        char c = d->s[d->i + k];
        int h = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (h < 0) { return -1; }
        v = (v << 4) | h;
    }
    d->i += 4;
    return v;
}

static void add_utf8(luaL_Buffer *b, unsigned long cp) {
    char u[4];
    int n;
    if (cp < 0x80)         { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800)   { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                             u[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else                   { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                             u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    luaL_addlstring(b, u, (size_t)n);
}

/* At the opening quote. Pushes the string. */
static bool dec_string(dec_ctx *d) {
    d->i++;                                   /* the quote */
    /* Fast path: no escapes, so the string is a slice of the input. */
    size_t start = d->i;
    while (d->i < d->len) {
        unsigned char c = (unsigned char)d->s[d->i];
        if (c == '"') {
            lua_pushlstring(d->L, d->s + start, d->i - start);
            d->i++;
            return true;
        }
        if (c == '\\') { break; }
        if (c < 0x20) { return dec_fail(d, "control character in string"); }
        d->i++;
    }
    if (d->i >= d->len) { return dec_fail(d, "unterminated string"); }

    luaL_Buffer b;
    luaL_buffinit(d->L, &b);
    luaL_addlstring(&b, d->s + start, d->i - start);
    while (d->i < d->len) {
        unsigned char c = (unsigned char)d->s[d->i];
        if (c == '"') {
            d->i++;
            luaL_pushresult(&b);
            return true;
        }
        if (c < 0x20) { return dec_fail(d, "control character in string"); }
        if (c != '\\') {
            luaL_addchar(&b, (char)c);
            d->i++;
            continue;
        }
        d->i++;
        if (d->i >= d->len) { break; }
        char e = d->s[d->i++];
        switch (e) {
            case '"':  luaL_addchar(&b, '"');  break;
            case '\\': luaL_addchar(&b, '\\'); break;
            case '/':  luaL_addchar(&b, '/');  break;
            case 'b':  luaL_addchar(&b, '\b'); break;
            case 'f':  luaL_addchar(&b, '\f'); break;
            case 'n':  luaL_addchar(&b, '\n'); break;
            case 'r':  luaL_addchar(&b, '\r'); break;
            case 't':  luaL_addchar(&b, '\t'); break;
            case 'u': {
                int hi = dec_hex4(d);
                if (hi < 0) { return dec_fail(d, "bad \\u escape"); }
                unsigned long cp = (unsigned long)hi;
                if (hi >= 0xD800 && hi <= 0xDBFF) {
                    /* A high surrogate must be followed by a low one. */
                    if (d->len - d->i < 6 || d->s[d->i] != '\\' || d->s[d->i + 1] != 'u') {
                        return dec_fail(d, "lone surrogate");
                    }
                    d->i += 2;
                    int lo = dec_hex4(d);
                    if (lo < 0xDC00 || lo > 0xDFFF) { return dec_fail(d, "bad surrogate pair"); }
                    cp = 0x10000ul + (((unsigned long)hi - 0xD800ul) << 10) + ((unsigned long)lo - 0xDC00ul);
                } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
                    return dec_fail(d, "lone surrogate");
                }
                add_utf8(&b, cp);
                break;
            }
            default:
                return dec_fail(d, "bad escape");
        }
    }
    return dec_fail(d, "unterminated string");
}

static bool dec_number(dec_ctx *d) {
    size_t st = d->i;
    bool is_float = false;
    if (d->i < d->len && d->s[d->i] == '-') { d->i++; }
    if (d->i >= d->len || d->s[d->i] < '0' || d->s[d->i] > '9') { return dec_fail(d, "bad number"); }
    if (d->s[d->i] == '0') {
        d->i++;
    } else {
        while (d->i < d->len && d->s[d->i] >= '0' && d->s[d->i] <= '9') { d->i++; }
    }
    if (d->i < d->len && d->s[d->i] == '.') {
        is_float = true;
        d->i++;
        if (d->i >= d->len || d->s[d->i] < '0' || d->s[d->i] > '9') { return dec_fail(d, "bad number"); }
        while (d->i < d->len && d->s[d->i] >= '0' && d->s[d->i] <= '9') { d->i++; }
    }
    if (d->i < d->len && (d->s[d->i] == 'e' || d->s[d->i] == 'E')) {
        is_float = true;
        d->i++;
        if (d->i < d->len && (d->s[d->i] == '+' || d->s[d->i] == '-')) { d->i++; }
        if (d->i >= d->len || d->s[d->i] < '0' || d->s[d->i] > '9') { return dec_fail(d, "bad number"); }
        while (d->i < d->len && d->s[d->i] >= '0' && d->s[d->i] <= '9') { d->i++; }
    }
    char tmp[40];
    size_t n = d->i - st;
    if (n >= sizeof tmp) {
        /* A 40-digit integer is not a lua_Integer on any build; keep the
         * magnitude as a float rather than refusing a valid document. */
        is_float = true;
        n = sizeof tmp - 1;
    }
    memcpy(tmp, d->s + st, n);
    tmp[n] = 0;
    if (!is_float) {
        char *end = NULL;
        long long v = strtoll(tmp, &end, 10);
        if ((lua_Integer)v == v && v != LLONG_MAX && v != LLONG_MIN) {
            lua_pushinteger(d->L, (lua_Integer)v);
            return true;
        }
    }
    lua_pushnumber(d->L, (lua_Number)strtod(tmp, NULL));
    return true;
}

static bool dec_lit(dec_ctx *d, const char *word) {
    size_t n = strlen(word);
    if (d->len - d->i < n || memcmp(d->s + d->i, word, n) != 0) { return dec_fail(d, "unexpected character"); }
    d->i += n;
    return true;
}

static bool dec_value(dec_ctx *d, int depth);

static bool dec_array(dec_ctx *d, int depth) {
    d->i++;                                   /* [ */
    lua_newtable(d->L);
    dec_ws(d);
    if (d->i < d->len && d->s[d->i] == ']') { d->i++; return true; }
    lua_Integer n = 0;
    for (;;) {
        if (!dec_value(d, depth + 1)) { return false; }
        lua_rawseti(d->L, -2, ++n);
        dec_ws(d);
        if (d->i >= d->len) { return dec_fail(d, "unterminated array"); }
        char c = d->s[d->i++];
        if (c == ']') { return true; }
        if (c != ',') { d->i--; return dec_fail(d, "expected ',' or ']'"); }
        dec_ws(d);
    }
}

static bool dec_object(dec_ctx *d, int depth) {
    d->i++;                                   /* { */
    lua_newtable(d->L);
    dec_ws(d);
    if (d->i < d->len && d->s[d->i] == '}') { d->i++; return true; }
    for (;;) {
        if (d->i >= d->len || d->s[d->i] != '"') { return dec_fail(d, "expected a string key"); }
        if (!dec_string(d)) { return false; }
        dec_ws(d);
        if (d->i >= d->len || d->s[d->i] != ':') { return dec_fail(d, "expected ':'"); }
        d->i++;
        dec_ws(d);
        if (!dec_value(d, depth + 1)) { return false; }
        lua_rawset(d->L, -3);
        dec_ws(d);
        if (d->i >= d->len) { return dec_fail(d, "unterminated object"); }
        char c = d->s[d->i++];
        if (c == '}') { return true; }
        if (c != ',') { d->i--; return dec_fail(d, "expected ',' or '}'"); }
        dec_ws(d);
    }
}

static bool dec_value(dec_ctx *d, int depth) {
    if (depth >= LJSON_DECODE_DEPTH) { return dec_fail(d, "nested too deeply"); }
    /* Each level keeps a table and a key on the Lua stack. */
    if (!lua_checkstack(d->L, 4)) { return dec_fail(d, "stack overflow"); }
    dec_ws(d);
    if (d->i >= d->len) { return dec_fail(d, "unexpected end"); }
    char c = d->s[d->i];
    switch (c) {
        case '{': return dec_object(d, depth);
        case '[': return dec_array(d, depth);
        case '"': return dec_string(d);
        case 't': if (!dec_lit(d, "true"))  { return false; } lua_pushboolean(d->L, 1); return true;
        case 'f': if (!dec_lit(d, "false")) { return false; } lua_pushboolean(d->L, 0); return true;
        case 'n': if (!dec_lit(d, "null"))  { return false; } ljson_push_null(d->L);    return true;
        default:
            if (c == '-' || (c >= '0' && c <= '9')) { return dec_number(d); }
            return dec_fail(d, "unexpected character");
    }
}

void ljson_push_null(lua_State *L) {
    lua_pushlightuserdata(L, NULL);
}

bool ljson_decode(lua_State *L, const char *s, size_t len, const char **err) {
    static dec_ctx d;      /* static: the message outlives the call, and core0's stack is small */
    memset(&d, 0, sizeof d);
    d.L = L; d.s = s; d.len = len;
    int top = lua_gettop(L);
    bool ok = dec_value(&d, 0);
    if (ok) {
        dec_ws(&d);
        if (d.i != d.len) { ok = dec_fail(&d, "trailing characters"); }
    }
    if (!ok) {
        lua_settop(L, top);
        if (err) { *err = d.err ? d.err : "bad JSON"; }
        return false;
    }
    if (err) { *err = NULL; }
    return true;
}
