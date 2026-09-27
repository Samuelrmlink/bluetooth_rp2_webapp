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
#include <stdarg.h>
#include <stdio.h>
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
