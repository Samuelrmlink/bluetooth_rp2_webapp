/*
 * The three channels a script has into the API, and the Bluetooth surface.
 *
 * C owns the websocket, the framing and the dispatch table; Lua extends them.
 * Handing the whole socket to a script would be worse in both directions: the
 * generic explorer has to work with no scripts loaded, and a script error
 * would take down the very channel needed to fix the script.
 *
 *   bus.emit(topic, tbl)   -> {"evt":"profile.<topic>","data":{...}}
 *   ops.register(name, fn) -> a new entry in the API's dispatch table
 *   log.info/warn/error    -> {"evt":"log","data":{"src":"lua",...}}
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"

#include "lua/lbind.h"
#include "lua/lrt.h"
#include "lua/lheap.h"
#include "api/api.h"
#include "api/ops.h"
#include "bt/registry.h"
#include "fs/fs.h"
#include "net/mdns.h"
#include "net/ping.h"
#include "net/gw_lan.h"
#include "net/uplink.h"
#include "net/fwd.h"
#include "net/ws.h"
#include "lua/ljson.h"
#include "lua/lfile.h"
#include "lua/lhttp.h"
#include "net/sntp_time.h"
#include "pico/time.h"
#include "autoconf.h"

#if CONFIG_BT_LE
#include "bt/le/central.h"
#include "bt/le/gattdb.h"
#include "bt/le/bond.h"
#include "btstack.h"
#include "bt/le/gattserver.h"
#endif

#define MAX_LUA_OPS 24
/* `owners`: a bit per script slot that registered this op, plus OP_PINNED
 * for one registered outside any script (script.eval). An op's handler is a
 * closure, and a closure keeps alive everything it can reach -- the script's
 * environment, its upvalues, the code it was compiled from. Ops used to be
 * kept forever, so every script that had ever registered one stayed resident
 * after it stopped: found 2026-10-01 with only the C1000 driver running, 58 KB
 * of a 160 KB arena held by a stopped script's ten ops and an old copy of the
 * Anker driver, and no free block above 2.8 KB. lbind_release_ops drops a
 * script's claim when it goes; the op goes with its last owner. */
#define OP_PINNED 0x80000000u
typedef struct { char name[40]; int fn_ref; uint32_t owners; } lua_op_t;
static lua_op_t lua_ops[MAX_LUA_OPS];
static int n_lua_ops;
static lua_State *gL;

/* Sized like api.c's scratch, and for the same reason: this holds one event's
 * `data` value, and the envelope around it has to fit the same frame. 1536 was
 * enough until a telemetry report grew past it. The encoder itself is in
 * ljson.c, where a host test can drive it. */
static char enc_buf[WS_TX_MAX - 256];

/* --- bus ------------------------------------------------------------------ */
static int l_bus_emit(lua_State *L) {
    const char *topic = luaL_checkstring(L, 1);
    /* profile.* is reserved for scripts precisely so one can never forge a
     * core event such as dev.connected. */
    char full[72];
    snprintf(full, sizeof(full), "profile.%s", topic);
    bool enc_trunc = false;
    enc_buf[0] = 0;
    if (lua_gettop(L) >= 2) {
        ljson_encode(L, 2, enc_buf, sizeof(enc_buf), &enc_trunc);
    } else {
        snprintf(enc_buf, sizeof(enc_buf), "null");
    }
    if (enc_trunc) {
        /* Emitting the clamped text would put an unterminated string on the
         * wire; the browser throws on it and points at nothing. Name the topic
         * instead, so the profile that outgrew the buffer is the one the log
         * identifies. */
        snprintf(enc_buf, sizeof(enc_buf),
                 "{\"truncated\":true,\"message\":\"event too large to encode\"}");
        char esc[72];
        api_json_escape(esc, sizeof(esc), full);
        char line[220];
        snprintf(line, sizeof(line),
                 "{\"level\":\"error\",\"src\":\"lua\",\"message\":"
                 "\"%s dropped: does not fit %u bytes\"}", esc, (unsigned)sizeof(enc_buf));
        api_emit("log", line);
    }
    api_emit(full, enc_buf);
    return 0;
}

/* --- log ------------------------------------------------------------------ */
static int log_at(lua_State *L, const char *level) {
    const char *msg = luaL_tolstring(L, 1, NULL);
    char esc[320];
    api_json_escape(esc, sizeof(esc), msg);
    char json[420];
    snprintf(json, sizeof(json), "{\"src\":\"lua\",\"level\":\"%s\",\"message\":\"%s\"}", level, esc);
    api_emit("log", json);
    printf("lua[%s]: %s\n", level, msg);
    lua_pop(L, 1);
    return 0;
}
static int l_log_info(lua_State *L) { return log_at(L, "info"); }
static int l_log_warn(lua_State *L) { return log_at(L, "warn"); }
static int l_log_error(lua_State *L) { return log_at(L, "error"); }

/* --- ops.register --------------------------------------------------------- */
static int l_ops_register(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    /* A script may not shadow a built-in: a client must be able to trust that
     * sys.info means what the firmware says it means. */
    if (ops_lookup(name)) {
        return luaL_error(L, "'%s' is a built-in op and cannot be replaced", name);
    }
    int slot = lrt_current_slot();
    uint32_t bit = (slot >= 0 && slot < 31) ? (1u << slot) : OP_PINNED;
    for (int i = 0; i < n_lua_ops; i++) {
        if (strcmp(lua_ops[i].name, name) == 0) {
            /* Registering again replaces the handler (two Anker launchers
             * share one dispatcher) and adds this script as an owner. */
            luaL_unref(L, LUA_REGISTRYINDEX, lua_ops[i].fn_ref);
            lua_pushvalue(L, 2);
            lua_ops[i].fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
            lua_ops[i].owners |= bit;
            return 0;
        }
    }
    if (n_lua_ops >= MAX_LUA_OPS) { return luaL_error(L, "too many registered ops"); }
    snprintf(lua_ops[n_lua_ops].name, sizeof(lua_ops[n_lua_ops].name), "%s", name);
    lua_pushvalue(L, 2);
    lua_ops[n_lua_ops].fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_ops[n_lua_ops].owners = bit;
    n_lua_ops++;
    return 0;
}

#if CONFIG_BT_LE
static void lbind_release_pairing(int slot);
#endif
static void lbind_release_net(int slot);

/* A script is going: drop its claim on every op, and the ops it was the last
 * owner of. Returns how many ops were removed. */
int lbind_release_ops(int slot) {
    if (!gL || slot < 0 || slot >= 31) { return 0; }
    uint32_t bit = 1u << slot;
    int removed = 0;
#if CONFIG_BT_LE
    lbind_release_pairing(slot);
#endif
    /* What it declared on the network goes with it: mDNS services and
     * record groups, and (CONFIG_NET_FORWARD) its forwarding rules. */
    lbind_release_net(slot);
    /* Its open files, and a storage job it was waiting on. */
    lfile_release(slot);
#if CONFIG_NET_HTTP_CLIENT
    lhttp_release(slot);
#endif
    for (int i = 0; i < n_lua_ops; ) {
        if (!(lua_ops[i].owners & bit)) { i++; continue; }
        lua_ops[i].owners &= ~bit;
        if (lua_ops[i].owners) { i++; continue; }
        luaL_unref(gL, LUA_REGISTRYINDEX, lua_ops[i].fn_ref);
        lua_ops[i] = lua_ops[--n_lua_ops];
        removed++;
    }
    return removed;
}

/* --- params: JSON object -> Lua table --------------------------------------
 * An op handler used to be handed the raw params JSON as a STRING, which made
 * every registered op that takes an argument silently useless: p.celsius on a
 * string is nil, so a setpoint write became a write of zero. It went unnoticed
 * because the only registered op until now took no parameters.
 *
 * This decodes the flat object the API actually sends -- string, number,
 * boolean, null -- and hands a nested object or array through as its raw JSON
 * text rather than dropping it, so nothing is lost and a script can parse it
 * itself if it ever needs to. It is not a general JSON parser and does not
 * pretend to be one. */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { p++; }
    return p;
}

/* Copy a JSON string body into `out`, resolving the escapes that matter.
 * Returns the character after the closing quote, or NULL. */
static const char *json_string(const char *p, char *out, size_t cap) {
    if (*p != '"') { return NULL; }
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        char ch = *p++;
        if (ch == '\\' && *p) {
            char e = *p++;
            switch (e) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'u': {
                    /* Only the Latin-1 range, which is all an op parameter has
                     * ever carried; anything else becomes '?' rather than
                     * silently truncating the string. */
                    unsigned v = 0;
                    for (int k = 0; k < 4 && p[k]; k++) {
                        char d = p[k];
                        v = (v << 4) | (unsigned)(d <= '9' ? d - '0' : (d | 32) - 'a' + 10);
                    }
                    p += 4;
                    ch = v < 256 ? (char)v : '?';
                    break;
                }
                default: ch = e; break;     /* \" \\ \/ and anything else */
            }
        }
        if (n + 1 < cap) { out[n++] = ch; }
    }
    if (*p != '"') { return NULL; }
    out[n < cap ? n : cap - 1] = 0;
    return p + 1;
}

/* Span of a nested object/array, so it can be passed through verbatim. */
static const char *json_span(const char *p) {
    char open = *p, close = (open == '{') ? '}' : ']';
    int depth = 0;
    bool in_str = false;
    while (*p) {
        if (in_str) {
            if (*p == '\\' && p[1]) { p++; }
            else if (*p == '"') { in_str = false; }
        } else if (*p == '"') {
            in_str = true;
        } else if (*p == open) {
            depth++;
        } else if (*p == close) {
            if (--depth == 0) { return p + 1; }
        }
        p++;
    }
    return NULL;
}

static void push_params(lua_State *L, const char *json) {
    lua_newtable(L);
    if (!json) { return; }
    const char *p = skip_ws(json);
    if (*p != '{') { return; }
    p = skip_ws(p + 1);
    while (*p && *p != '}') {
        static char key[64];
        p = json_string(p, key, sizeof key);
        if (!p) { return; }
        p = skip_ws(p);
        if (*p != ':') { return; }
        p = skip_ws(p + 1);

        if (*p == '"') {
            static char val[256];
            const char *q = json_string(p, val, sizeof val);
            if (!q) { return; }
            lua_pushstring(L, val);
            p = q;
        } else if (*p == '{' || *p == '[') {
            const char *q = json_span(p);
            if (!q) { return; }
            lua_pushlstring(L, p, (size_t)(q - p));
            p = q;
        } else if (!strncmp(p, "true", 4)) {
            lua_pushboolean(L, 1); p += 4;
        } else if (!strncmp(p, "false", 5)) {
            lua_pushboolean(L, 0); p += 5;
        } else if (!strncmp(p, "null", 4)) {
            lua_pushnil(L); p += 4;
        } else {
            char *end = NULL;
            double d = strtod(p, &end);
            if (end == p) { return; }
            /* Integers stay integers: a byte count or a temperature that
             * arrives as 320.0 reads badly and indexes worse. */
            if (d == (double)(long long)d) { lua_pushinteger(L, (lua_Integer)d); }
            else { lua_pushnumber(L, (lua_Number)d); }
            p = end;
        }
        lua_setfield(L, -2, key);

        p = skip_ws(p);
        if (*p == ',') { p = skip_ws(p + 1); }
    }
}

/* An op handler runs here, on the MAIN Lua state, not inside the registering
 * script's coroutine. It therefore MAY NOT YIELD: no sys.sleep, no awaiting a
 * reply, nothing that suspends. Yielding across this pcall fails, and the
 * handler returns script_error with no obvious cause.
 *
 * A handler that needs to wait should set a flag that its script's own loop
 * reads and acts on. The loop IS a coroutine and may yield; the handler is
 * not and may not.
 */
bool lbind_dispatch(const char *name, const char *params_json,
                    void (*reply)(const char *, void *), void *ctx, long id) {
    if (!gL) { return false; }
    for (int i = 0; i < n_lua_ops; i++) {
        if (strcmp(lua_ops[i].name, name) != 0) { continue; }
        lua_rawgeti(gL, LUA_REGISTRYINDEX, lua_ops[i].fn_ref);
        push_params(gL, params_json);
        /* The raw text stays reachable as params._json for anything this flat
         * decoder cannot represent. Trimmed to the object: params_of() hands
         * back a pointer INTO the request and never terminates the span, so
         * the unclipped string carries the envelope's closing brace too. */
        if (params_json) {
            const char *q = skip_ws(params_json);
            const char *end = (*q == '{') ? json_span(q) : NULL;
            if (end) { lua_pushlstring(gL, q, (size_t)(end - q)); }
            else { lua_pushstring(gL, params_json); }
        } else {
            lua_pushstring(gL, "{}");
        }
        lua_setfield(gL, -2, "_json");
        if (lua_pcall(gL, 1, 1, 0) != LUA_OK) {
            char esc[240];
            api_json_escape(esc, sizeof(esc), lua_tostring(gL, -1) ? lua_tostring(gL, -1) : "error");
            char out[360];
            snprintf(out, sizeof(out),
                     "{\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"script_error\",\"message\":\"%s\"}}",
                     id, esc);
            reply(out, ctx);
            lua_pop(gL, 1);
            return true;
        }
        bool enc_trunc = false;
        enc_buf[0] = 0;
        ljson_encode(gL, lua_gettop(gL), enc_buf, sizeof(enc_buf), &enc_trunc);
        lua_pop(gL, 1);
        /* Static for the same reason api.c's buffers are: every caller runs
         * from the single cooperative loop and consumes this within the call,
         * and core0's stack is too small to hold a whole frame. It has to be
         * able to hold enc_buf plus the envelope, or a large result is
         * truncated here after surviving the encoder. */
        static char out[WS_TX_MAX];
        if (enc_trunc) {
            snprintf(out, sizeof(out),
                     "{\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"result_too_large\","
                     "\"message\":\"op result does not fit %u bytes\"}}",
                     id, (unsigned)sizeof(enc_buf));
        } else {
            snprintf(out, sizeof(out), "{\"id\":%ld,\"ok\":true,\"result\":%s}", id, enc_buf);
        }
        reply(out, ctx);
        return true;
    }
    return false;
}

size_t lbind_ops_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "[");
    for (int i = 0; i < n_lua_ops && n + 48 < cap; i++) {
        n += (size_t)snprintf(dst + n, cap - n, "%s\"%s\"", i ? "," : "", lua_ops[i].name);
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]");
}

/* --- sys ------------------------------------------------------------------ */
/* --- sys.secret ------------------------------------------------------------
 * Read one string field out of a JSON file in cfg/.
 *
 * A device protocol needs per-deployment values that must never be compiled
 * in or committed: an account identifier, a private key, a token. They live
 * in cfg/<name>.json, which is git-ignored and which the HTTP filesystem API
 * refuses to serve, so this binding is the only way to reach them -- and it
 * is why no profile has to carry a secret in its source.
 *
 * It is deliberately narrow: `name` may not contain a path separator or a
 * dot, so the path is always exactly cfg/<name>.json and cannot be steered
 * anywhere else. It reads only top-level string fields, which is all a
 * credential ever is, and it is a READ -- there is no way to set a secret
 * from a script.
 *
 * Any script can call it. That is the intent: scripts are code the operator
 * uploaded, not untrusted input, and a profile that cannot reach its own
 * credentials is useless. It is worth knowing rather than discovering. */
static int l_sys_secret(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const char *key = luaL_checkstring(L, 2);
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '.') {
            return luaL_error(L, "secret file name may not contain a path");
        }
    }
    char path[48];
    snprintf(path, sizeof path, "cfg/%s.json", name);

    static char buf[1024];
    size_t sz = 0;
    if (!fs_stat(path, &sz) || sz >= sizeof buf) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is missing or too large", path);
        return 2;
    }
    int h = fs_begin_read(path, NULL);
    if (h < 0) { lua_pushnil(L); lua_pushstring(L, "cannot open"); return 2; }
    int n = fs_read(h, (uint8_t *)buf, sz);
    fs_end_read(h);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, "read failed"); return 2; }
    buf[n] = 0;

    /* "key" : "value" -- enough JSON for a flat credential file, and it does
     * not pretend to be more. Escapes are not interpreted: a credential that
     * needs one is a credential that should be hex. */
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *k = strstr(buf, needle);
    if (!k) { lua_pushnil(L); return 1; }
    const char *v = strchr(k + strlen(needle), ':');
    if (!v) { lua_pushnil(L); return 1; }
    v++;
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') { v++; }
    if (*v != '"') { lua_pushnil(L); return 1; }
    v++;
    const char *end = strchr(v, '"');
    if (!end) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, v, (size_t)(end - v));
    return 1;
}

/* sys.unload(name) -- forget a required module so the arena can reclaim it.
 * Returns the live heap size afterwards, so a caller can see what it bought.
 * Clearing the cache is only half the job: drop your own references first, or
 * the module stays reachable and nothing is freed. */
static int l_sys_unload(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    bool had = lrt_unload_module(name);
    size_t after = lrt_collect();
    lua_pushboolean(L, had);
    lua_pushinteger(L, (lua_Integer)after);
    return 2;
}

/* sys.collect() -- full collection, returns the live heap size. */
static int l_sys_collect(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)lrt_collect());
    return 1;
}

/* sys.heap() -> the Lua arena, as the allocator sees it (no collection):
 *   {capacity=, used=, peak=, free=, largest_free=, blocks=, free_blocks=,
 *    header=, alloc_fails=, alloc_fail_last=}
 * `used` counts live payload bytes, `free` free payload bytes; the rest of
 * the capacity is block headers. largest_free is the biggest single
 * allocation that can succeed now: when it falls far below `free`, the arena
 * is fragmented. alloc_fails counts requests that found no block (Lua then
 * collects and retries, so most are not errors). One walk of the arena. */
static int l_sys_heap(lua_State *L) {
    lheap_stats_t st;
    lheap_stats(&st);
    size_t fail_last = 0, fail_count = 0;
    lheap_fail_stats(&fail_last, &fail_count);
    lua_createtable(L, 0, 15);
    lua_pushinteger(L, (lua_Integer)lheap_capacity());   lua_setfield(L, -2, "capacity");
    lua_pushinteger(L, (lua_Integer)lheap_used());       lua_setfield(L, -2, "used");
    lua_pushinteger(L, (lua_Integer)lheap_peak());       lua_setfield(L, -2, "peak");
    lua_pushinteger(L, (lua_Integer)st.free_bytes);      lua_setfield(L, -2, "free");
    lua_pushinteger(L, (lua_Integer)st.largest_free);    lua_setfield(L, -2, "largest_free");
    lua_pushinteger(L, (lua_Integer)st.blocks);          lua_setfield(L, -2, "blocks");
    lua_pushinteger(L, (lua_Integer)st.free_blocks);     lua_setfield(L, -2, "free_blocks");
    lua_pushinteger(L, (lua_Integer)lheap_header_size()); lua_setfield(L, -2, "header");
    lua_pushinteger(L, (lua_Integer)fail_count);         lua_setfield(L, -2, "alloc_fails");
    lua_pushinteger(L, (lua_Integer)fail_last);          lua_setfield(L, -2, "alloc_fail_last");
    /* By lifetime: the largest free block is the gap between the scripts'
     * data (bottom) and the long-lived side (top); the rest are holes. */
    lua_pushinteger(L, (lua_Integer)st.largest_free_low);  lua_setfield(L, -2, "largest_free_low");
    lua_pushinteger(L, (lua_Integer)st.largest_free_high); lua_setfield(L, -2, "largest_free_high");
    lua_pushinteger(L, (lua_Integer)st.free_low);          lua_setfield(L, -2, "free_low");
    lua_pushinteger(L, (lua_Integer)st.free_high);         lua_setfield(L, -2, "free_high");
    lheap_scratch_stats_t sc;
    lheap_scratch_stats(&sc);
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, (lua_Integer)sc.active);     lua_setfield(L, -2, "active");
    lua_pushinteger(L, (lua_Integer)sc.bytes);      lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, (lua_Integer)sc.bytes_peak); lua_setfield(L, -2, "bytes_peak");
    lua_pushinteger(L, (lua_Integer)sc.live_peak);  lua_setfield(L, -2, "live_peak");
    lua_pushinteger(L, (lua_Integer)sc.page_fails); lua_setfield(L, -2, "page_fails");
    lua_pushinteger(L, (lua_Integer)sc.runs);       lua_setfield(L, -2, "runs");
    lua_setfield(L, -2, "scratch");
    return 1;
}

/* sys.restart_vm() -> true. Asks for the Lua VM to be rebuilt on the next
 * main-loop pass (not now: this script is running): every script stopped, the
 * arena reset, and the scripts that were running started again -- what a
 * reboot does for Lua, without dropping Wi-Fi or Bluetooth. Lua never moves
 * an object, so this is the one way to put a long-running set of scripts'
 * arena back in one piece; a script asks for it when it is idle and finds the
 * free space too fragmented, after saving whatever must survive (nothing in
 * the arena does). Yield after asking (sys.sleep): the restart happens then. */
static int l_sys_restart_vm(lua_State *L) {
    lrt_request_vm_restart();
    lua_pushboolean(L, 1);
    return 1;
}

/* sys.time() -> Unix seconds once SNTP has set the clock, else nil. */
static int l_sys_time(lua_State *L) {
    if (!sntp_time_valid()) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)sntp_time_unix());
    return 1;
}

/* sys.hostname() -> the mDNS / DHCP host name (mdns.hostname() without the
 * setter): a gateway's identity among several. */
static int l_sys_hostname(lua_State *L) {
    lua_pushstring(L, mdns_hostname());
    return 1;
}

static int l_sys_ms(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)to_ms_since_boot(get_absolute_time()));
    return 1;
}

/* Yields the coroutine so the main loop keeps driving the radio and network.
 * A busy-wait here would stall both; the scheduler resumes us next pass. */
static int l_sys_sleep(lua_State *L) {
    lua_Integer ms = luaL_optinteger(L, 1, 0);
    uint32_t until = to_ms_since_boot(get_absolute_time()) + (uint32_t)ms;
    lua_pushinteger(L, (lua_Integer)until);
    return lua_yield(L, 1);
}

/* --- bt ------------------------------------------------------------------- */
#if CONFIG_BT_LE
/* One registry entry as a table, pushed on the stack. */
static void push_device(lua_State *L, const bt_device_t *d) {
    lua_createtable(L, 0, 9);
    char addr[20];
    snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
             d->addr[0], d->addr[1], d->addr[2], d->addr[3], d->addr[4], d->addr[5]);
    lua_pushstring(L, addr);              lua_setfield(L, -2, "addr");
    lua_pushstring(L, bt_transport_name(d->transport)); lua_setfield(L, -2, "transport");
    lua_pushinteger(L, d->rssi);          lua_setfield(L, -2, "rssi");
    lua_pushboolean(L, d->connected);     lua_setfield(L, -2, "connected");
    if (d->have_name) { lua_pushstring(L, d->name); lua_setfield(L, -2, "name"); }
    /* The advertisement, which is what a profile matches on. Without this
     * a script can only match on the name -- and a name is exactly what
     * an Anker charger does not put its model in, so every match failed
     * silently. */
    if (d->have_mfg) {
        char hex[2 * BT_MFG_MAX + 1];
        for (uint8_t k = 0; k < d->mfg_len; k++) {
            snprintf(hex + 2 * k, 3, "%02x", d->mfg[k]);
        }
        hex[2 * d->mfg_len] = 0;
        lua_pushstring(L, hex);
        lua_setfield(L, -2, "mfg_data");
    }
    {
        /* 128-bit service UUIDs, from the raw advertisement and scan
         * response, so a profile can match a vendor service. */
        char u128[4][BT_UUID128_STR];
        int k = bt_device_uuids128(d, u128, 4);
        lua_createtable(L, k, 0);
        for (int j = 0; j < k; j++) { lua_pushstring(L, u128[j]); lua_rawseti(L, -2, j + 1); }
        lua_setfield(L, -2, "uuids128");
    }
    /* How long since the last advertising report: the table keeps
     * entries for devices long gone, and this is how to tell. */
    lua_pushinteger(L, (lua_Integer)(to_ms_since_boot(get_absolute_time()) - d->last_seen_ms));
    lua_setfield(L, -2, "age_ms");
    if (d->have_uuids) {
        lua_createtable(L, d->n_uuid16, 0);
        for (uint8_t k = 0; k < d->n_uuid16; k++) {
            char u[8];
            snprintf(u, sizeof u, "%04x", d->uuid16[k]);
            lua_pushstring(L, u);
            lua_rawseti(L, -2, k + 1);
        }
        lua_setfield(L, -2, "service_uuids");
    }
}

/* Does the device pass bt.devices' filter (the table at `opts`)? `uuids128`:
 * a list of 128-bit UUIDs, any of which it advertises; `name_prefixes`: a
 * list, one of which its name starts with. Either matching is enough. */
static bool device_matches(lua_State *L, int opts, const bt_device_t *d) {
    bool any_rule = false;
    if (lua_getfield(L, opts, "uuids128") == LUA_TTABLE) {
        any_rule = true;
        char u128[4][BT_UUID128_STR];
        int k = bt_device_uuids128(d, u128, 4);
        lua_Integer n = luaL_len(L, -1);
        for (lua_Integer i = 1; i <= n && k > 0; i++) {
            if (lua_geti(L, -1, i) == LUA_TSTRING) {
                const char *want = lua_tostring(L, -1);
                for (int j = 0; j < k; j++) {
                    if (strcasecmp(want, u128[j]) == 0) { lua_pop(L, 2); return true; }
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (lua_getfield(L, opts, "name_prefixes") == LUA_TTABLE) {
        any_rule = true;
        if (d->have_name) {
            lua_Integer n = luaL_len(L, -1);
            for (lua_Integer i = 1; i <= n; i++) {
                if (lua_geti(L, -1, i) == LUA_TSTRING) {
                    size_t pl = 0;
                    const char *pre = lua_tolstring(L, -1, &pl);
                    if (pl > 0 && strncmp(d->name, pre, pl) == 0) { lua_pop(L, 2); return true; }
                }
                lua_pop(L, 1);
            }
        }
    }
    lua_pop(L, 1);
    return !any_rule;
}

/* bt.devices([opts]) -> every device the registry holds, or only those that
 * pass opts {uuids128 = {...}, name_prefixes = {...}} (see device_matches):
 * a script that wants a few devices among a crowd of advertisers then does
 * not get -- and the collector does not have to sweep -- a table for each of
 * up to 48 of them every time it looks. */
static int l_bt_devices(lua_State *L) {
    int opts = lua_istable(L, 1) ? 1 : 0;
    lua_newtable(L);
    int idx = 1;
    for (size_t i = 0; i < bt_registry_count(); i++) {
        bt_device_t *d = bt_registry_at(i);
        if (!d) { continue; }
        if (opts && !device_matches(L, opts, d)) { continue; }
        push_device(L, d);
        lua_seti(L, -2, idx++);
    }
    return 1;
}

static bool addr_from_lua(lua_State *L, int idx, uint8_t *out) {
    const char *s = luaL_checkstring(L, idx);
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) { out[i] = (uint8_t)v[i]; }
    return true;
}

/* bt.device(addr) -> that device's entry (as in bt.devices), or nil: one
 * lookup instead of a table for every device in range. */
static int l_bt_device(lua_State *L) {
    uint8_t a[6];
    if (!addr_from_lua(L, 1, a)) { return luaL_error(L, "expected an address"); }
    bt_device_t *d = bt_registry_find(a, BT_TRANSPORT_LE);
    if (!d) { lua_pushnil(L); return 1; }
    push_device(L, d);
    return 1;
}

static int l_bt_connect(lua_State *L) {
    uint8_t a[6];
    if (!addr_from_lua(L, 1, a)) { return luaL_error(L, "expected an address"); }
    bt_device_t *d = bt_registry_find(a, BT_TRANSPORT_LE);
    if (!d) { lua_pushboolean(L, 0); lua_pushstring(L, "not in the scan table"); return 2; }
    bool ok = le_connect(d->addr, d->addr_type);
    /* Remember who opened it. A script that dies without releasing its links
     * leaves them up forever, and an orphaned link keeps le_radio_busy()
     * true, which makes the one-radio guard refuse every WiFi join. */
    if (ok) { lrt_note_link(d->addr); }
    lua_pushboolean(L, ok);
    return 1;
}

/* bt.scan(on [, duration_ms]) -> bool
 *
 * A profile could read bt.devices() but could not make the radio LOOK, which
 * is a trap rather than a limitation: the scan table is whatever was last
 * seen, so a profile whose discovery loop polls bt.devices() spins forever,
 * silently, whenever nothing else happens to be scanning. That is exactly
 * what happened after a scan.stop -- the Anker profile sat at "looking for an
 * Anker device" with both chargers advertising a metre away.
 *
 * Defaults to an active scan with no duration: passive scanning misses the
 * SCAN_RSP that usually carries the name, and a profile wants the scan to
 * last as long as its search does. */
static int l_bt_scan(lua_State *L) {
    bool on = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
    if (!on) { le_scan_stop(); lua_pushboolean(L, 1); return 1; }
    /* Idempotent on purpose. A discovery loop calls this every pass, and
     * le_scan_start() issues gap_start_scan() unconditionally -- restarting
     * the scan twice a second would keep tearing down the very window in
     * which a peer's advertisement arrives. */
    if (le_scanning()) { lua_pushboolean(L, 1); return 1; }
    uint32_t ms = (uint32_t)luaL_optinteger(L, 2, 0);
    le_scan_filter_t f = { .active = true };
    lua_pushboolean(L, le_scan_start(&f, ms));
    return 1;
}

/* The counterpart, which did not exist: a script could open a link but had no
 * way to close one. Teardown drops whatever is left, but a profile that is
 * finished with a device should be able to say so while still running. */
static int l_bt_disconnect(lua_State *L) {
    uint8_t a[6];
    if (!addr_from_lua(L, 1, a)) { return luaL_error(L, "expected an address"); }
    bool ok = le_disconnect(a);
    lrt_forget_link(a);
    lua_pushboolean(L, ok);
    return 1;
}

/* bt.write(addr, path, hex, with_response=true, want_done=false)
 *   -> true | false, why | nil, "too_long"
 *
 * A value longer than MTU-3 is refused at once ("too_long"): queued, the
 * stack would refuse it on every pass until its 5 s deadline and then drop it
 * silently, holding up the writes behind it. With want_done the outcome comes
 * through bt.await (and bt.notify_take) as
 *   { addr, id = path, source = "write", ok = bool, att_error = n [, error = "timeout"] }
 * -- with response when the peer answers, without when the controller takes it. */
static int l_bt_write(lua_State *L) {
    uint8_t a[6];
    if (!addr_from_lua(L, 1, a)) { return luaL_error(L, "expected an address"); }
    size_t hexlen = 0;
    const char *id = luaL_checkstring(L, 2);
    const char *hex = luaL_checklstring(L, 3, &hexlen);
    bool with_resp = lua_isnoneornil(L, 4) ? true : lua_toboolean(L, 4);
    bool want_done = lua_toboolean(L, 5);
    /* 6th argument true: `hex` is the bytes themselves, not hex text. An image
     * transfer writes ~1650 chunks; as hex each was a 290-character string
     * built from a dozen pieces -- ~1.3 KB of garbage per chunk for the
     * collector, and a sscanf per byte here. */
    bool binary = lua_toboolean(L, 6);

    bt_device_t *d = bt_registry_find(a, BT_TRANSPORT_LE);
    if (!d || !d->connected) { lua_pushboolean(L, 0); lua_pushstring(L, "not connected"); return 2; }
    gattdb_t *db = le_db_for(d->con_handle);
    uint8_t si, ci;
    if (!db || !db->ready || !gattdb_resolve_chr(db, id, &si, &ci)) {
        lua_pushboolean(L, 0); lua_pushstring(L, "no such characteristic"); return 2;
    }
    static uint8_t buf[512];
    size_t n = binary ? hexlen : hexlen / 2;
    uint16_t mtu = le_mtu(d->addr);
    if (n > (size_t)(mtu ? mtu : 23) - 3 || n > sizeof buf) { lua_pushnil(L); lua_pushstring(L, "too_long"); return 2; }
    if (binary) {
        memcpy(buf, hex, n);
    } else {
        for (size_t i = 0; i < n; i++) {
            unsigned b = 0; sscanf(hex + i * 2, "%2x", &b); buf[i] = (uint8_t)b;
        }
    }
    int rc = le_write(d->addr, si, ci, buf, (uint16_t)n, with_resp, want_done);
    if (rc == 0) { lua_pushboolean(L, 1); return 1; }
    lua_pushboolean(L, 0);
    lua_pushstring(L, rc == -5 ? "queue_full" : rc == -3 ? "too_long" : "not ready");
    return 2;
}

/* bt.mtu(addr) -> the negotiated ATT MTU, or nil before the exchange (or
 * when not connected). A write without response carries at most MTU-3. */
static int l_bt_mtu(lua_State *L) {
    uint8_t a[6];
    if (!addr_from_lua(L, 1, a)) { return luaL_error(L, "expected an address"); }
    uint16_t m = le_mtu(a);
    if (!m) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, m);
    return 1;
}

static int l_bt_subscribe(lua_State *L) {
    uint8_t a[6];
    if (!addr_from_lua(L, 1, a)) { return luaL_error(L, "expected an address"); }
    const char *id = luaL_checkstring(L, 2);
    const char *mode = luaL_optstring(L, 3, "notify");
    bt_device_t *d = bt_registry_find(a, BT_TRANSPORT_LE);
    if (!d || !d->connected) { lua_pushboolean(L, 0); return 1; }
    gattdb_t *db = le_db_for(d->con_handle);
    uint8_t si, ci;
    if (!db || !db->ready || !gattdb_resolve_chr(db, id, &si, &ci)) {
        lua_pushboolean(L, 0); return 1;
    }
    int m = strcmp(mode, "indicate") == 0 ? 2 : strcmp(mode, "off") == 0 ? 0 : 1;
    lua_pushboolean(L, le_subscribe(d->addr, si, ci, m) == 0);
    return 1;
}
#endif

/* --- gatt_server: declare what a PEER sees when it queries us --------------
 *
 * att_db_util builds the ATT database at runtime, which is what makes this
 * scriptable at all -- the usual route is compile_gatt.py at build time, and
 * that would mean a reflash for every change.
 *
 *   gatt_server.begin()
 *   gatt_server.service('180f')
 *   gatt_server.characteristic('2a19', {read=true, notify=true}, '\x5a')
 *   gatt_server.commit()
 */
#if CONFIG_BT_LE
static int l_gs_begin(lua_State *L) { (void)L; gatt_server_begin(); return 0; }

static int l_gs_service(lua_State *L) {
    const char *uuid = luaL_checkstring(L, 1);
    const char *err = "unknown";
    if (!gatt_server_add_service(uuid, &err)) { return luaL_error(L, "%s", err); }
    return 0;
}

static uint16_t props_from_table(lua_State *L, int idx) {
    if (!lua_istable(L, idx)) { return ATT_PROPERTY_READ; }
    static const struct { const char *k; uint16_t bit; } map[] = {
        { "read", ATT_PROPERTY_READ }, { "write", ATT_PROPERTY_WRITE },
        { "write_no_response", ATT_PROPERTY_WRITE_WITHOUT_RESPONSE },
        { "notify", ATT_PROPERTY_NOTIFY }, { "indicate", ATT_PROPERTY_INDICATE },
    };
    uint16_t p = 0;
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        lua_getfield(L, idx, map[i].k);
        if (lua_toboolean(L, -1)) { p |= map[i].bit; }
        lua_pop(L, 1);
    }
    return p ? p : ATT_PROPERTY_READ;
}

static int l_gs_characteristic(lua_State *L) {
    const char *uuid = luaL_checkstring(L, 1);
    uint16_t props = props_from_table(L, 2);
    size_t len = 0;
    const char *val = lua_isnoneornil(L, 3) ? "" : luaL_checklstring(L, 3, &len);
    uint16_t handle = 0;
    const char *err = "unknown";
    if (!gatt_server_add_characteristic(uuid, props, (const uint8_t *)val,
                                        (uint16_t)len, &handle, &err)) {
        return luaL_error(L, "%s", err);
    }
    lua_pushinteger(L, handle);
    return 1;
}

static int l_gs_commit(lua_State *L) {
    const char *err = "unknown";
    if (!gatt_server_commit(&err)) { return luaL_error(L, "%s", err); }
    return 0;
}

/* Push a new value to subscribers of a characteristic we serve. */
static int l_gs_notify(lua_State *L) {
    lua_Integer handle = luaL_checkinteger(L, 1);
    size_t len = 0;
    const char *val = luaL_checklstring(L, 2, &len);
    int sent = 0;
    for (int i = 0; i < CONFIG_BT_MAX_LINKS; i++) {
        bt_device_t *d = bt_registry_at((size_t)i);
        if (d && d->connected) {
            att_server_notify(d->con_handle, (uint16_t)handle, (const uint8_t *)val, (uint16_t)len);
            sent++;
        }
    }
    lua_pushinteger(L, sent);
    return 1;
}
#endif

/* --- install -------------------------------------------------------------- */
#if CONFIG_BT_LE
/* Resolve a UUID pair to the "svc.chr" path the other bindings take. A script
 * addresses a characteristic by UUID -- a path index is an artefact of
 * discovery order and means nothing to a protocol -- but resolving once and
 * reusing the path keeps the write path cheap. */
static int l_bt_find(lua_State *L) {
    uint8_t a[6];
    if (!addr_from_lua(L, 1, a)) { return luaL_error(L, "expected an address"); }
    const char *svc = luaL_checkstring(L, 2);
    const char *chr = luaL_checkstring(L, 3);
    lua_Integer inst = luaL_optinteger(L, 4, 0);

    bt_device_t *d = bt_registry_find(a, BT_TRANSPORT_LE);
    if (!d || !d->connected) { lua_pushnil(L); lua_pushstring(L, "not connected"); return 2; }
    gattdb_t *db = le_db_for(d->con_handle);
    uint8_t si, ci;
    if (!db || !db->ready) { lua_pushnil(L); lua_pushstring(L, "discovery not finished"); return 2; }
    if (!gattdb_find_chr(db, svc, chr, (int)inst, &si, &ci)) {
        lua_pushnil(L); lua_pushstring(L, "no such characteristic"); return 2;
    }
    /* chr_i is a database index; the path wants it relative to its service. */
    char path[16];
    snprintf(path, sizeof path, "%u.%u", si, (unsigned)(ci - db->svc[si].first_chr));
    lua_pushstring(L, path);
    return 1;
}

/* One inbox entry as the table bt.await / bt.notify_take return. A "write"
 * entry (bt.write's want_done) carries ok / att_error (/ error) instead of a
 * value. */
static void push_frame(lua_State *L, const char *addr, const char *id, const char *src,
                       const uint8_t *val, int n, int st) {
    lua_createtable(L, 0, 5);
    lua_pushstring(L, addr);                  lua_setfield(L, -2, "addr");
    lua_pushstring(L, id);                    lua_setfield(L, -2, "id");
    lua_pushstring(L, src);                   lua_setfield(L, -2, "source");
    if (st >= 0) {
        int att = st & 0xFF, reason = st >> 8;
        lua_pushboolean(L, att == 0 && reason == LE_WRITE_DONE); lua_setfield(L, -2, "ok");
        lua_pushinteger(L, att);                                lua_setfield(L, -2, "att_error");
        if (reason == LE_WRITE_TIMEOUT) { lua_pushstring(L, "timeout"); lua_setfield(L, -2, "error"); }
    } else {
        lua_pushlstring(L, (const char *)val, (size_t)n); lua_setfield(L, -2, "value");
    }
}

/* Drain one queued notification, non-blocking. nil when the inbox is empty.
 *
 * This is the primitive under the profile's link:await(): a Lua helper polls
 * it and sys.sleep()s between tries, which is why the scheduler needed no new
 * yield discipline for the handshake to read as straight-line code. The poll
 * interval costs one coroutine resume every few milliseconds against a device
 * that takes ~900 ms to answer its key exchange. */
/* bt.notify_take([addr]) -- with an address, takes only that device's oldest
 * entry and leaves everyone else's queued. Two profiles sharing one FIFO
 * silently ate each other's replies; passing the address is how a profile
 * stays out of another's way. */
static int l_bt_notify_take(lua_State *L) {
    char addr[18], id[8], src[10];
    const char *filter = NULL;
    uint8_t fa[6];
    char fbuf[18];
    if (!lua_isnoneornil(L, 1) && addr_from_lua(L, 1, fa)) {
        snprintf(fbuf, sizeof fbuf, "%02X:%02X:%02X:%02X:%02X:%02X",
                 fa[0], fa[1], fa[2], fa[3], fa[4], fa[5]);
        filter = fbuf;
    }
    static uint8_t val[256];
    int st = -1;
    int n = le_notify_take(filter, addr, sizeof addr, id, sizeof id, src, sizeof src,
                           val, sizeof val, &st);
    if (n < 0) { lua_pushnil(L); return 1; }
    push_frame(L, addr, id, src, val, n, st);
    return 1;
}


/* --- bt.await -------------------------------------------------------------
 * Block until this device says something, and be told why the wait ended.
 *
 *   local frame, why = bt.await(addr, 4000)
 *   if not frame then ... end          -- why is "timeout" | "link_lost" | ...
 *
 * This is bt.notify_take with the polling removed. The old shape --
 *
 *   repeat f = bt.notify_take(addr); if not f then sys.sleep(5) end until f
 *
 * -- costs a resume every 5 ms whether or not anything has arrived (measured:
 * about 190 resumes per second to deliver one report per second), and it has
 * no way to notice that the link died: it simply keeps asking an absent
 * device until some outer deadline gives up.
 *
 * The four outcomes and the rules behind them are in lua/lwait.h. The two
 * that matter to a profile author: the wait is ALWAYS bounded, and a
 * disconnect ends it immediately rather than after the full timeout.
 */
static int await_result(lua_State *L, lwait_reason_t why);

static int await_k(lua_State *L, int status, lua_KContext ctx) {
    (void)status; (void)ctx;
    return await_result(L, lrt_wake_reason());
}

/* Drain the frame the wake was about. Delivery and draining are separate
 * steps on purpose: the frame stays in the inbox until the script actually
 * runs, so a blocking reader and a polling reader see identical data. */
static int await_drain(lua_State *L, const char *filter) {
    char addr[18], id[8], src[10];
    static uint8_t val[256];
    int st = -1;
    int n = le_notify_take(filter, addr, sizeof addr, id, sizeof id,
                           src, sizeof src, val, sizeof val, &st);
    if (n < 0) { return -1; }
    push_frame(L, addr, id, src, val, n, st);
    return 1;
}

/* PER SCRIPT, not one buffer. With a single shared filter the second script
 * to call bt.await overwrites the first's address, and on resume the first
 * drains the wrong device's inbox -- which showed up as the charger's
 * handshake failing with "no reply to 0x0003" the moment the iron's profile
 * was also running. One coroutine per script means one pending await per
 * script, so slot-indexed is exactly the right width. */
static char s_await_filter[LWAIT_MAX_OWNERS][18];

static char *await_filter_for_current(void) {
    int slot = lrt_current_slot();
    if (slot < 0 || slot >= LWAIT_MAX_OWNERS) { return NULL; }
    return s_await_filter[slot];
}

static int await_result(lua_State *L, lwait_reason_t why) {
    const char *filter = await_filter_for_current();
    if (why == LWAIT_DELIVERED && filter) {
        if (await_drain(L, filter) == 1) { return 1; }
        /* Woken for a frame that is no longer there: another reader on the
         * same device drained it first. Report it as a timeout rather than
         * inventing a frame -- the caller's retry is the right response. */
        lua_pushnil(L);
        lua_pushstring(L, "timeout");
        return 2;
    }
    lua_pushnil(L);
    lua_pushstring(L, lwait_reason_name(why));
    return 2;
}

static int l_bt_await(lua_State *L) {
    uint8_t a[6];
    if (!addr_from_lua(L, 1, a)) { return luaL_error(L, "expected an address"); }
    lua_Integer timeout = luaL_optinteger(L, 2, 0);   /* 0 = the default cap */
    if (timeout < 0) { timeout = 0; }

    char *filter = await_filter_for_current();
    if (!filter) { return luaL_error(L, "bt.await is only callable from a script"); }
    snprintf(filter, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             a[0], a[1], a[2], a[3], a[4], a[5]);

    /* Already waiting for us? Then there is nothing to block on. */
    if (await_drain(L, filter) == 1) { return 1; }

    int slot = le_link_slot(filter);
    if (slot < 0) {
        /* Not connected at all. Blocking would be a wait on a link that
         * cannot answer, which is the case a deadline alone handles badly. */
        lua_pushnil(L);
        lua_pushstring(L, lwait_reason_name(LWAIT_LINK_LOST));
        return 2;
    }

    lwait_reason_t r = lrt_block_current(le_addr_token(filter),
                                         (uint8_t)slot, (uint32_t)timeout);
    if (r != LWAIT_PENDING) {
        return await_result(L, r);      /* resolved without ever blocking */
    }
    return lua_yieldk(L, 0, 0, await_k);
}

/* Ask for a characteristic's value. The answer does NOT come back here -- it
 * arrives in the inbox tagged source="read", because a GATT read is a round
 * trip and blocking the cooperative loop on one would stall the radio. The
 * caller polls bt.notify_take() for it, which is the same shape as waiting
 * for a notification and keeps one pattern instead of two. */
static int l_bt_read(lua_State *L) {
    uint8_t a[6];
    if (!addr_from_lua(L, 1, a)) { return luaL_error(L, "expected an address"); }
    const char *id = luaL_checkstring(L, 2);
    bt_device_t *d = bt_registry_find(a, BT_TRANSPORT_LE);
    if (!d || !d->connected) { lua_pushboolean(L, 0); lua_pushstring(L, "not connected"); return 2; }
    gattdb_t *db = le_db_for(d->con_handle);
    uint8_t si, ci;
    if (!db || !db->ready || !gattdb_resolve_chr(db, id, &si, &ci)) {
        lua_pushboolean(L, 0); lua_pushstring(L, "no such characteristic"); return 2;
    }
    int rc = le_read(d->addr, si, ci);
    lua_pushboolean(L, rc == 0);
    if (rc != 0) { lua_pushstring(L, "read refused (busy?)"); return 2; }
    return 1;
}

static int l_bt_notify_dropped(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)le_notify_dropped());
    return 1;
}

/* bt.notify_flush([addr]) -- scoped the same way. A global flush from one
 * profile used to wipe another's pending key exchange. */
static int l_bt_notify_flush(lua_State *L) {
    uint8_t fa[6];
    char fbuf[18];
    if (!lua_isnoneornil(L, 1) && addr_from_lua(L, 1, fa)) {
        snprintf(fbuf, sizeof fbuf, "%02X:%02X:%02X:%02X:%02X:%02X",
                 fa[0], fa[1], fa[2], fa[3], fa[4], fa[5]);
        le_notify_flush(fbuf);
    } else {
        le_notify_flush(NULL);
    }
    return 0;
}

/* --- pairing ---------------------------------------------------------------
 * A script can pair and answer the pairing questions itself -- a device with
 * a fixed PIN printed on it needs no human at all:
 *
 *   bt.pair(addr)                    ask for pairing on a connected link
 *   bt.passkey(addr, 123456)         answer a passkey request
 *   bt.confirm(addr, true|false)     answer a numeric comparison
 *   bt.decline(addr)                 refuse to pair
 *   bt.pairing_state(addr)          { state = "idle"|"asking"|"bonded"|"failed",
 *                                      action, number (while asking), reason (failed) }
 *   bt.on_pairing(function(ev) ... end)   (nil to remove)
 *       ev = { addr = "AA:..", action = "input"|"compare"|"display",
 *              number = <the number to compare or to show> }
 *       return a number for "input", true/false for "compare"; nil leaves
 *       the question to whoever else answers (the web UI shows it too).
 *
 * The handler is called from the runtime's main loop, NOT inside the
 * script's coroutine: it must answer at once and cannot sleep or wait. It
 * belongs to the script that set it and goes when that script does
 * (lbind_release_ops). The questions themselves always reach WebSocket
 * clients as security.* events; an answer from anywhere closes them with
 * security.answered.
 *
 * For plain blocking code there is profiles/pairing.lua on top of these:
 * pairing.pair(addr, { pin = 123456 }) requests pairing, waits, answers and
 * returns the outcome. */
static bt_device_t *le_dev(lua_State *L, int idx) {
    uint8_t a[6];
    if (!addr_from_lua(L, idx, a)) { luaL_error(L, "expected an address"); return NULL; }
    bt_device_t *d = bt_registry_find(a, BT_TRANSPORT_LE);
    return (d && d->connected) ? d : NULL;
}

/* Where each link's pairing stands, for bt.pairing_state. */
#define PAIR_DEV 4
static struct { uint16_t con_handle; uint8_t state; uint8_t ask; uint32_t number; bool used; } s_pair_dev[PAIR_DEV];
enum { PS_IDLE, PS_ASKING, PS_BONDED, PS_FAILED };

static int l_bt_pair(lua_State *L) {
    bt_device_t *d = le_dev(L, 1);
    /* A fresh start: connection handles are reused, and an earlier link's
     * "bonded" must not answer for this one. */
    for (int i = 0; d && i < PAIR_DEV; i++) {
        if (s_pair_dev[i].used && s_pair_dev[i].con_handle == d->con_handle) { s_pair_dev[i].state = PS_IDLE; }
    }
    lua_pushboolean(L, d && bond_request(d->con_handle));
    return 1;
}

static int l_bt_passkey(lua_State *L) {
    bt_device_t *d = le_dev(L, 1);
    lua_Integer pk = luaL_checkinteger(L, 2);
    lua_pushboolean(L, d && pk >= 0 && pk <= 999999 &&
                       bond_supply_passkey(d->con_handle, (uint32_t)pk, "script"));
    return 1;
}

static int l_bt_confirm(lua_State *L) {
    bt_device_t *d = le_dev(L, 1);
    lua_pushboolean(L, d && bond_confirm(d->con_handle, lua_toboolean(L, 2), "script"));
    return 1;
}

static int l_bt_pairing_state(lua_State *L) {
    bt_device_t *d = le_dev(L, 1);
    lua_createtable(L, 0, 4);
    const char *st = "idle";
    if (d) {
        for (int i = 0; i < PAIR_DEV; i++) {
            if (!s_pair_dev[i].used || s_pair_dev[i].con_handle != d->con_handle) { continue; }
            uint8_t v = s_pair_dev[i].state;
            st = v == PS_ASKING ? "asking" : v == PS_BONDED ? "bonded" : v == PS_FAILED ? "failed" : "idle";
            if (v == PS_ASKING) {
                uint8_t a = s_pair_dev[i].ask;
                lua_pushstring(L, a == BOND_ASK_INPUT ? "input" : a == BOND_ASK_COMPARE ? "compare" : "display");
                lua_setfield(L, -2, "action");
                if (a != BOND_ASK_INPUT) { lua_pushinteger(L, s_pair_dev[i].number); lua_setfield(L, -2, "number"); }
            } else if (v == PS_FAILED) {
                lua_pushinteger(L, s_pair_dev[i].number); lua_setfield(L, -2, "reason");
            }
        }
    } else {
        st = "not_connected";
    }
    lua_pushstring(L, st);
    lua_setfield(L, -2, "state");
    return 1;
}

static int l_bt_decline(lua_State *L) {
    bt_device_t *d = le_dev(L, 1);
    lua_pushboolean(L, d && bond_decline(d->con_handle, "script"));
    return 1;
}

#define PAIR_SLOTS 31
static int s_pair_ref[PAIR_SLOTS];          /* 0 = no handler */

static void lbind_release_pairing(int slot) {
    if (slot >= 0 && slot < PAIR_SLOTS && s_pair_ref[slot]) {
        luaL_unref(gL, LUA_REGISTRYINDEX, s_pair_ref[slot]);
        s_pair_ref[slot] = 0;
    }
}

static int l_bt_on_pairing(lua_State *L) {
    int slot = lrt_current_slot();
    if (slot < 0 || slot >= PAIR_SLOTS) { return luaL_error(L, "bt.on_pairing is only callable from a script"); }
    if (!lua_isnoneornil(L, 1)) { luaL_checktype(L, 1, LUA_TFUNCTION); }
    if (s_pair_ref[slot]) { luaL_unref(L, LUA_REGISTRYINDEX, s_pair_ref[slot]); s_pair_ref[slot] = 0; }
    if (!lua_isnoneornil(L, 1)) {
        lua_pushvalue(L, 1);
        s_pair_ref[slot] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

/* Questions from the SM, waiting for the main loop. The hook runs inside the
 * BTstack event handler, so it only copies. */
#define PAIR_Q 4
static struct { uint16_t con_handle; uint8_t ask; uint32_t number; bool used; } s_pair_q[PAIR_Q];

static void pairing_hook(uint16_t con_handle, bond_ask_t ask, uint32_t number) {
    int at = -1, free_i = -1;
    for (int i = 0; i < PAIR_DEV; i++) {
        if (s_pair_dev[i].used && s_pair_dev[i].con_handle == con_handle) { at = i; break; }
        if (!s_pair_dev[i].used && free_i < 0) { free_i = i; }
    }
    if (at < 0) { at = free_i >= 0 ? free_i : 0; }
    s_pair_dev[at].used = true;
    s_pair_dev[at].con_handle = con_handle;
    switch (ask) {
        case BOND_ANSWERED:    if (s_pair_dev[at].state == PS_ASKING) { s_pair_dev[at].state = PS_IDLE; } return;
        case BOND_DONE_OK:     s_pair_dev[at].state = PS_BONDED; return;
        case BOND_DONE_FAILED: s_pair_dev[at].state = PS_FAILED; s_pair_dev[at].number = number; return;
        default:
            s_pair_dev[at].state = PS_ASKING;
            s_pair_dev[at].ask = (uint8_t)ask;
            s_pair_dev[at].number = number;
            break;
    }
    for (int i = 0; i < PAIR_Q; i++) {
        if (!s_pair_q[i].used) {
            s_pair_q[i].con_handle = con_handle;
            s_pair_q[i].ask = (uint8_t)ask;
            s_pair_q[i].number = number;
            s_pair_q[i].used = true;
            return;
        }
    }
}

void lbind_pairing_pump(void) {
    if (!gL) { return; }
    for (int q = 0; q < PAIR_Q; q++) {
        if (!s_pair_q[q].used) { continue; }
        uint16_t ch = s_pair_q[q].con_handle;
        uint8_t ask = s_pair_q[q].ask;
        uint32_t number = s_pair_q[q].number;
        s_pair_q[q].used = false;
        bt_device_t *d = bt_registry_by_handle(ch);
        if (!d) { continue; }
        char addr[18];
        snprintf(addr, sizeof addr, "%02X:%02X:%02X:%02X:%02X:%02X",
                 d->addr[0], d->addr[1], d->addr[2], d->addr[3], d->addr[4], d->addr[5]);
        const char *action = ask == BOND_ASK_INPUT ? "input" : ask == BOND_ASK_COMPARE ? "compare" : "display";
        for (int slot = 0; slot < PAIR_SLOTS; slot++) {
            if (!s_pair_ref[slot]) { continue; }
            int top = lua_gettop(gL);
            lua_rawgeti(gL, LUA_REGISTRYINDEX, s_pair_ref[slot]);
            lua_createtable(gL, 0, 3);
            lua_pushstring(gL, addr);   lua_setfield(gL, -2, "addr");
            lua_pushstring(gL, action); lua_setfield(gL, -2, "action");
            if (ask != BOND_ASK_INPUT) { lua_pushinteger(gL, (lua_Integer)number); lua_setfield(gL, -2, "number"); }
            bool done = false;
            if (lua_pcall(gL, 1, 1, 0) != LUA_OK) {
                printf("lua: bt.on_pairing handler failed: %s\n", lua_tostring(gL, -1));
            } else if (ask == BOND_ASK_INPUT && lua_isinteger(gL, -1)) {
                lua_Integer pk = lua_tointeger(gL, -1);
                done = pk >= 0 && pk <= 999999 && bond_supply_passkey(ch, (uint32_t)pk, "script");
            } else if (ask == BOND_ASK_COMPARE && lua_isboolean(gL, -1)) {
                done = bond_confirm(ch, lua_toboolean(gL, -1), "script");
            }
            lua_settop(gL, top);
            if (done) { break; }
        }
    }
}

#endif


/* --- mdns ----------------------------------------------------------------
 * The device's name and what it advertises are deployment facts, not
 * firmware facts, so they are set from Lua rather than compiled in. lwIP
 * emits the records; these choose what goes in them.
 *
 *   mdns.hostname()            -> "bluetooth-rp2-webapp"
 *   mdns.hostname('lab-gw')    -> true            (A, and every SRV target;
 *                                                  stored in cfg/hostname.json)
 *   mdns.peers()               -> { {name=, ip=, port=, board=, assets=,
 *                                   age_ms=, self=}, ... }
 *   mdns.service{ type = '_http', port = 80,      (PTR + SRV + TXT)
 *                 txt = { 'path=/', 'role=gw' } }
 *   mdns.remove('_http')
 *   mdns.list()                -> { hostname=..., services={...} }
 */
static int l_mdns_hostname(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushstring(L, mdns_hostname());
        return 1;
    }
    const char *name = luaL_checkstring(L, 1);
    if (!mdns_set_hostname(name)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "a hostname may contain only letters, digits and hyphens");
        return 2;
    }
    /* The boot formula does not come through here. A script write does. */
    gw_hostname_written(mdns_hostname());
    mdns_hostname_remember();
    lua_pushboolean(L, 1);
    return 1;
}

static int l_mdns_peers(lua_State *L) {
    lua_newtable(L);
    mdns_peer_view_t v;
    for (int i = 0; mdns_peer_at(i, &v); i++) {
        lua_createtable(L, 0, 7);
        lua_pushstring(L, v.name); lua_setfield(L, -2, "name");
        lua_pushstring(L, v.ip); lua_setfield(L, -2, "ip");
        lua_pushinteger(L, v.port); lua_setfield(L, -2, "port");
        lua_pushstring(L, v.board); lua_setfield(L, -2, "board");
        if (v.assets < 0) { lua_pushnil(L); } else { lua_pushinteger(L, v.assets); }
        lua_setfield(L, -2, "assets");
        lua_pushinteger(L, (lua_Integer)v.age_ms); lua_setfield(L, -2, "age_ms");
        lua_pushboolean(L, v.self); lua_setfield(L, -2, "self");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_mdns_service(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "type");
    const char *type = luaL_optstring(L, -1, NULL);
    if (!type) { return luaL_error(L, "mdns.service needs a type, e.g. '_http'"); }
    char type_buf[24];
    snprintf(type_buf, sizeof type_buf, "%s", type);
    lua_pop(L, 1);

    lua_getfield(L, 1, "instance");
    const char *inst = luaL_optstring(L, -1, NULL);
    char inst_buf[48];
    inst_buf[0] = 0;
    if (inst) { snprintf(inst_buf, sizeof inst_buf, "%s", inst); }
    lua_pop(L, 1);

    lua_getfield(L, 1, "port");
    lua_Integer port = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    if (port <= 0 || port > 65535) { return luaL_error(L, "mdns.service needs a port"); }

    lua_getfield(L, 1, "proto");
    const char *proto = luaL_optstring(L, -1, "tcp");
    bool udp = strcmp(proto, "udp") == 0;
    lua_pop(L, 1);

    /* Copied out of Lua before the call: lwIP asks for the TXT items later,
     * at announce time, long after this stack is gone. */
    char txt[MDNS_TXT_MAX][MDNS_TXT_LEN + 1];
    const char *txtp[MDNS_TXT_MAX];
    int n_txt = 0;
    lua_getfield(L, 1, "txt");
    if (lua_istable(L, -1)) {
        lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
        for (lua_Integer i = 1; i <= n && n_txt < MDNS_TXT_MAX; i++) {
            lua_rawgeti(L, -1, i);
            const char *item = lua_tostring(L, -1);
            if (item && item[0]) {
                snprintf(txt[n_txt], sizeof txt[n_txt], "%s", item);
                txtp[n_txt] = txt[n_txt];
                n_txt++;
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    /* Owned by the calling script: withdrawn when it stops. */
    if (!mdns_service_add_owned(lrt_current_slot(), inst_buf[0] ? inst_buf : NULL, type_buf, udp,
                                (uint16_t)port, txtp, n_txt)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no free service slot, or a bad type");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_mdns_remove(lua_State *L) {
    const char *type = luaL_checkstring(L, 1);
    const char *proto = luaL_optstring(L, 2, "tcp");
    lua_pushboolean(L, mdns_service_remove(type, strcmp(proto, "udp") == 0));
    return 1;
}

static int l_mdns_announce(lua_State *L) {
    (void)L;
    mdns_announce_all();
    return 0;
}

/*   id = mdns.group{ records = {
 *          { type='A',   name='printer.local', addr='192.168.8.50' },
 *          { type='PTR', name='_ipp._tcp.local', target='Printer._ipp._tcp.local' },
 *          { type='SRV', name='Printer._ipp._tcp.local', target='printer.local', port=631 },
 *          { type='TXT', name='Printer._ipp._tcp.local', txt={ 'rp=ipp/print' } } } }
 *   mdns.ungroup(id)   mdns.groups() -> JSON, like mdns.list()
 * Records for names that are not ours; announced on add, withdrawn (TTL 0)
 * by ungroup or when the script stops. */
static bool lua_field_str(lua_State *L, int t, const char *k, char *out, size_t cap) {
    lua_getfield(L, t, k);
    const char *v = lua_tostring(L, -1);
    bool ok = v != NULL;
    if (ok) { snprintf(out, cap, "%s", v); }
    lua_pop(L, 1);
    return ok;
}

static lua_Integer lua_field_int(lua_State *L, int t, const char *k, lua_Integer dflt) {
    lua_getfield(L, t, k);
    lua_Integer v = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : dflt;
    lua_pop(L, 1);
    return v;
}

static int l_mdns_group(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "records");
    if (!lua_istable(L, -1)) { return luaL_error(L, "mdns.group needs records = { ... }"); }
    int rt = lua_gettop(L);
    /* On the Lua heap, not static: ~1 KB that is needed only here. */
    mrec_t *recs = (mrec_t *)lua_newuserdatauv(L, sizeof(mrec_t) * MREC_PER_GROUP, 0);
    int n = 0;
    lua_Integer len = (lua_Integer)lua_rawlen(L, rt);
    if (len > MREC_PER_GROUP) {
        lua_pushnil(L); lua_pushstring(L, "too many records in one group (max 8)"); return 2;
    }
    for (lua_Integer i = 1; i <= len; i++) {
        lua_rawgeti(L, rt, i);
        int e = lua_gettop(L);
        if (!lua_istable(L, e)) { lua_pop(L, 1); continue; }
        mrec_t *r = &recs[n];
        memset(r, 0, sizeof *r);
        char type[8] = "", tmp[24];
        lua_field_str(L, e, "type", type, sizeof type);
        lua_field_str(L, e, "name", r->name, sizeof r->name);
        if (strcmp(type, "A") == 0) {
            r->type = MREC_A;
            unsigned a, b, c, d;
            if (!lua_field_str(L, e, "addr", tmp, sizeof tmp) ||
                sscanf(tmp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
                lua_pushnil(L); lua_pushstring(L, "an A record needs addr = 'a.b.c.d'"); return 2;
            }
            r->a[0] = (uint8_t)a; r->a[1] = (uint8_t)b; r->a[2] = (uint8_t)c; r->a[3] = (uint8_t)d;
        } else if (strcmp(type, "PTR") == 0) {
            r->type = MREC_PTR;
            lua_field_str(L, e, "target", r->data, sizeof r->data);
        } else if (strcmp(type, "SRV") == 0) {
            r->type = MREC_SRV;
            lua_field_str(L, e, "target", r->data, sizeof r->data);
            lua_Integer port = lua_field_int(L, e, "port", 0);
            r->port = (port > 0 && port <= 65535) ? (uint16_t)port : 0;
            r->prio = (uint16_t)lua_field_int(L, e, "priority", 0);
            r->weight = (uint16_t)lua_field_int(L, e, "weight", 0);
        } else if (strcmp(type, "TXT") == 0) {
            r->type = MREC_TXT;
            char items[8][64];
            const char *ip[8];
            int ni = 0;
            lua_getfield(L, e, "txt");
            if (lua_istable(L, -1)) {
                lua_Integer tn = (lua_Integer)lua_rawlen(L, -1);
                for (lua_Integer k = 1; k <= tn && ni < 8; k++) {
                    lua_rawgeti(L, -1, k);
                    const char *it = lua_tostring(L, -1);
                    if (it) { snprintf(items[ni], sizeof items[ni], "%s", it); ip[ni] = items[ni]; ni++; }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            if (!mrec_txt_pack(ip, ni, r)) {
                lua_pushnil(L); lua_pushstring(L, "TXT items do not fit in 63 bytes"); return 2;
            }
        }
        n++;
        lua_pop(L, 1);
    }
    const char *why = "bad group";
    int id = mdns_group_add(lrt_current_slot(), recs, n, &why);
    if (id < 0) { lua_pushnil(L); lua_pushstring(L, why); return 2; }
    lua_pushinteger(L, id);
    return 1;
}

static int l_mdns_ungroup(lua_State *L) {
    lua_pushboolean(L, mdns_group_remove((int)luaL_checkinteger(L, 1)));
    return 1;
}

static int l_mdns_groups(lua_State *L) {
    char *js = api_scratch();
    size_t n = mdns_groups_json(js, api_scratch_size());
    lua_pushlstring(L, js, n);
    return 1;
}

static int l_mdns_list(lua_State *L) {
    static char js[512];
    size_t n = mdns_status_json(js, sizeof js);
    lua_pushlstring(L, js, n);
    return 1;
}

static void lbind_release_net(int slot) {
    int n = mdns_release_owner(slot);
#if CONFIG_NET_FORWARD
    n += fwd_release_owner(slot);
#endif
    if (n) { printf("lua: released %d network declaration(s) of slot %d\n", n, slot); }
}

/* --- net -----------------------------------------------------------------
 *   net.ping('169.254.1.1') -> {state='sent', ip=, iface=, id=}
 *   net.ping()              -> that result, then state='reply' or 'timeout'
 * One echo at a time. The reply is also the event net.ping. Link-local
 * is on-link on the interface that holds it; there is no gateway requirement.
 *
 *   net.uplinks() -> { {name='w0', kind='wifi', state='ok', active=true,
 *                       ip=..., mask=..., gw=..., rtt_ms=21, jitter_ms=3, loss_pct=0,
 *                       metric=27}, ... }, policy
 * rtt/jitter/metric are nil until measured. See src/net/uplink.h. */
#if CONFIG_NET_UPLINK
static void set_num_or_nil(lua_State *L, const char *k, long v) {
    if (v < 0) { lua_pushnil(L); } else { lua_pushinteger(L, v); }
    lua_setfield(L, -2, k);
}

static int l_net_uplinks(lua_State *L) {
    int n = uplink_count();
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        uplink_info_t u;
        if (!uplink_info(i, &u)) { continue; }
        lua_createtable(L, 0, 11);
        lua_pushstring(L, u.name);  lua_setfield(L, -2, "name");
        lua_pushstring(L, u.kind);  lua_setfield(L, -2, "kind");
        lua_pushstring(L, u.state); lua_setfield(L, -2, "state");
        lua_pushboolean(L, u.active); lua_setfield(L, -2, "active");
        lua_pushstring(L, u.ip);    lua_setfield(L, -2, "ip");
        lua_pushstring(L, u.mask);  lua_setfield(L, -2, "mask");
        lua_pushstring(L, u.gw);    lua_setfield(L, -2, "gw");
        set_num_or_nil(L, "rtt_ms", u.rtt_ms);
        set_num_or_nil(L, "jitter_ms", u.jitter_ms);
        set_num_or_nil(L, "metric", u.metric);
        lua_pushinteger(L, u.loss_pct); lua_setfield(L, -2, "loss_pct");
        lua_rawseti(L, -2, i + 1);
    }
    lua_pushstring(L, uplink_policy_name());
    return 2;
}
#endif

/*   id = net.forward{ proto='tcp'|'udp', listen=6767, iface=nil|'e0'|'w0'|...,
 *                     to='127.0.0.1', port=80, fail_fast=true }
 *   net.unforward(id)    net.forwards() -> { {id=, proto=, listen=, ...}, ... }
 * Rules belong to the calling script. See src/net/fwd.h. */
#if CONFIG_NET_FORWARD
static int l_net_forward(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    char proto[8] = "tcp", iface[8] = "", to[24] = "";
    lua_field_str(L, 1, "proto", proto, sizeof proto);
    lua_field_str(L, 1, "iface", iface, sizeof iface);
    lua_field_str(L, 1, "to", to, sizeof to);
    lua_Integer listen = lua_field_int(L, 1, "listen", 0), port = lua_field_int(L, 1, "port", 0);
    lua_getfield(L, 1, "fail_fast");
    bool ff = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
    lua_pop(L, 1);
    bool udp = strcmp(proto, "udp") == 0;
    if (!udp && strcmp(proto, "tcp") != 0) { lua_pushnil(L); lua_pushstring(L, "proto is 'tcp' or 'udp'"); return 2; }
    if (listen <= 0 || listen > 65535 || port <= 0 || port > 65535) {
        lua_pushnil(L); lua_pushstring(L, "listen and port must be 1..65535"); return 2;
    }
    if (!to[0]) { snprintf(to, sizeof to, "127.0.0.1"); }
    const char *why = "bad rule";
    int id = fwd_add(lrt_current_slot(), udp, (uint16_t)listen, iface, to, (uint16_t)port, ff, &why);
    if (id < 0) { lua_pushnil(L); lua_pushstring(L, why); return 2; }
    lua_pushinteger(L, id);
    return 1;
}

static int l_net_unforward(lua_State *L) {
    lua_pushboolean(L, fwd_remove((int)luaL_checkinteger(L, 1)));
    return 1;
}

static int l_net_forwards(lua_State *L) {
    int n = fwd_count();
    lua_createtable(L, n, 0);
    fwd_info_t f;
    for (int i = 0; i < n && fwd_info(i, &f); i++) {
        lua_createtable(L, 0, 14);
        lua_pushinteger(L, f.id); lua_setfield(L, -2, "id");
        lua_pushstring(L, f.udp ? "udp" : "tcp"); lua_setfield(L, -2, "proto");
        lua_pushinteger(L, f.listen); lua_setfield(L, -2, "listen");
        if (f.iface[0]) { lua_pushstring(L, f.iface); lua_setfield(L, -2, "iface"); }
        lua_pushstring(L, f.to); lua_setfield(L, -2, "to");
        lua_pushinteger(L, f.port); lua_setfield(L, -2, "port");
        lua_pushboolean(L, f.fail_fast); lua_setfield(L, -2, "fail_fast");
        lua_pushstring(L, f.state); lua_setfield(L, -2, "target");
        if (f.why[0]) { lua_pushstring(L, f.why); lua_setfield(L, -2, "why"); }
        lua_pushinteger(L, (lua_Integer)f.active); lua_setfield(L, -2, "active");
        lua_pushinteger(L, (lua_Integer)f.total); lua_setfield(L, -2, "total");
        lua_pushinteger(L, (lua_Integer)f.refused); lua_setfield(L, -2, "refused");
        lua_pushinteger(L, (lua_Integer)f.bytes_in); lua_setfield(L, -2, "bytes_in");
        lua_pushinteger(L, (lua_Integer)f.bytes_out); lua_setfield(L, -2, "bytes_out");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}
#endif

/* --- json ------------------------------------------------------------------
 *   json.decode(s)  -> value | nil, "<what> at byte N"   (null -> json.null)
 *   json.encode(v)  -> string | nil, err
 *   json.null       -- the value JSON null decodes to, and encodes back as null
 * Both are C (ljson.c). encode shares the event encoder: arrays are tables
 * with 1..n, other tables objects with their string keys, nesting at most 6. */
static int l_json_decode(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    const char *err = NULL;
    lua_settop(L, 1);
    bool ok = ljson_decode(L, s, len, &err);
    if (len > 4096) { lrt_yield_soon(); }
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "bad JSON");
        return 2;
    }
    return 1;
}

static int l_json_encode(lua_State *L) {
    luaL_checkany(L, 1);
    lua_settop(L, 1);
    bool trunc = false;
    size_t n = ljson_encode(L, 1, enc_buf, sizeof enc_buf, &trunc);
    if (!trunc) {
        lua_pushlstring(L, enc_buf, n);
        return 1;
    }
    /* Larger than an event: retry in a buffer on the Lua heap. */
    for (size_t cap = 16384; cap <= 65536; cap *= 4) {
        char *big = (char *)lua_newuserdatauv(L, cap, 0);
        n = ljson_encode(L, 1, big, cap, &trunc);
        if (!trunc) {
            lua_pushlstring(L, big, n);
            lrt_yield_soon();
            return 1;
        }
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    lua_pushstring(L, "toolarge");      /* over 64 KiB, or nested deeper than 6 */
    return 2;
}

static int l_net_ping(lua_State *L) {
    if (lua_isstring(L, 1) || lua_istable(L, 1)) {
        char ip[20];
        ip[0] = 0;
        if (lua_isstring(L, 1)) {
            snprintf(ip, sizeof ip, "%s", lua_tostring(L, 1));
        } else {
            lua_field_str(L, 1, "ip", ip, sizeof ip);
        }
        const char *why = "bad request";
        if (!ping_start(ip, &why)) {
            lua_pushnil(L);
            lua_pushstring(L, why);
            return 2;
        }
    }
    static char js[160];
    size_t n = ping_status_json(js, sizeof js);
    lua_pushlstring(L, js, n);
    return 1;
}

static void set_table(lua_State *L, const char *name, const luaL_Reg *fns) {
    /* Not luaL_newlib: its size hint is sizeof(l)/sizeof(l[0]), which on a
     * POINTER argument computes 8/32 - 1 = -1 rather than the entry count.
     * Build the table explicitly instead. */
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, name);
}

void lbind_open_all(lua_State *L) {
    gL = L;

    static const luaL_Reg mdns_fns[] = {
        { "hostname", l_mdns_hostname }, { "service", l_mdns_service },
        { "remove", l_mdns_remove }, { "announce", l_mdns_announce },
        { "list", l_mdns_list }, { "peers", l_mdns_peers },
        { "group", l_mdns_group }, { "ungroup", l_mdns_ungroup }, { "groups", l_mdns_groups },
        { NULL, NULL } };
    set_table(L, "mdns", mdns_fns);

    static const luaL_Reg net_fns[] = {
#if CONFIG_NET_UPLINK
        { "uplinks", l_net_uplinks },
#endif
        { "ping", l_net_ping },
#if CONFIG_NET_FORWARD
        { "forward", l_net_forward }, { "unforward", l_net_unforward },
        { "forwards", l_net_forwards },
#endif
        { NULL, NULL } };
    set_table(L, "net", net_fns);

    static const luaL_Reg json_fns[] = {
        { "decode", l_json_decode }, { "encode", l_json_encode }, { NULL, NULL } };
    set_table(L, "json", json_fns);
    lua_getglobal(L, "json");
    ljson_push_null(L);
    lua_setfield(L, -2, "null");
    lua_pop(L, 1);

    lfile_open(L);
#if CONFIG_NET_HTTP_CLIENT
    lhttp_open(L);
#endif

    static const luaL_Reg bus[] = { { "emit", l_bus_emit }, { NULL, NULL } };
    set_table(L, "bus", bus);

    static const luaL_Reg lg[] = {
        { "info", l_log_info }, { "warn", l_log_warn }, { "error", l_log_error }, { NULL, NULL } };
    set_table(L, "log", lg);

    static const luaL_Reg ops[] = { { "register", l_ops_register }, { NULL, NULL } };
    set_table(L, "ops", ops);

    static const luaL_Reg sys[] = {
        { "ms", l_sys_ms }, { "sleep", l_sys_sleep },
        { "time", l_sys_time }, { "hostname", l_sys_hostname },
        { "secret", l_sys_secret },
        { "unload", l_sys_unload }, { "collect", l_sys_collect },
        { "heap", l_sys_heap }, { "restart_vm", l_sys_restart_vm }, { NULL, NULL } };
    set_table(L, "sys", sys);

#if CONFIG_BT_LE
    static const luaL_Reg gs[] = {
        { "begin", l_gs_begin }, { "service", l_gs_service },
        { "characteristic", l_gs_characteristic }, { "commit", l_gs_commit },
        { "notify", l_gs_notify }, { NULL, NULL } };
    set_table(L, "gatt_server", gs);

    static const luaL_Reg bt[] = {
        { "devices", l_bt_devices }, { "device", l_bt_device }, { "connect", l_bt_connect },
        { "disconnect", l_bt_disconnect }, { "scan", l_bt_scan },
        { "write", l_bt_write }, { "subscribe", l_bt_subscribe },
        { "mtu", l_bt_mtu },
        { "find", l_bt_find },
        { "read", l_bt_read },
        { "notify_take", l_bt_notify_take },
        { "await", l_bt_await },
        { "notify_dropped", l_bt_notify_dropped },
        { "notify_flush", l_bt_notify_flush },
        { "pair", l_bt_pair }, { "passkey", l_bt_passkey },
        { "confirm", l_bt_confirm }, { "decline", l_bt_decline },
        { "on_pairing", l_bt_on_pairing }, { "pairing_state", l_bt_pairing_state },
        { NULL, NULL } };
    set_table(L, "bt", bt);
    /* What a script can test for: bt.write takes raw bytes (6th argument). */
    lua_getglobal(L, "bt");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "write_binary");
    lua_pop(L, 1);
    bond_set_pairing_hook(pairing_hook);
#endif

#if CONFIG_PROTO_ANKER
    lbind_proto_open(L);
#endif
}

#if !CONFIG_BT_LE
void lbind_pairing_pump(void) {}
#endif

/* The Lua state is being thrown away (lrt_vm_restart): forget every
 * reference into it without touching it. */
void lbind_reset(void) {
    /* Not slot-by-slot: the state is gone, so is everything it declared. */
    mdns_release_all_owned();
#if CONFIG_NET_FORWARD
    fwd_release_all_owned();
#endif
    lfile_reset();
#if CONFIG_NET_HTTP_CLIENT
    lhttp_reset();       /* before the arena is reset: TLS sessions live in it */
#endif
    memset(lua_ops, 0, sizeof lua_ops);
    n_lua_ops = 0;
#if CONFIG_BT_LE
    memset(s_pair_ref, 0, sizeof s_pair_ref);
    memset(s_pair_q, 0, sizeof s_pair_q);
    memset(s_pair_dev, 0, sizeof s_pair_dev);
#endif
    gL = NULL;
}
