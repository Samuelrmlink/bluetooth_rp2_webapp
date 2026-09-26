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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"

#include "lua/lbind.h"
#include "api/api.h"
#include "api/ops.h"
#include "bt/registry.h"
#include "pico/time.h"
#include "autoconf.h"

#if CONFIG_BT_LE
#include "bt/le/central.h"
#include "bt/le/gattdb.h"
#endif

#define MAX_LUA_OPS 24
typedef struct { char name[40]; int fn_ref; } lua_op_t;
static lua_op_t lua_ops[MAX_LUA_OPS];
static int n_lua_ops;
static lua_State *gL;

/* --- table -> JSON --------------------------------------------------------
 * Arrays (1..n with no holes) become [], everything else {}. Strings are
 * escaped through the same function the C side uses, because a value from a
 * script is no more trustworthy than one from the air. */
static void enc(lua_State *L, int idx, char *dst, size_t cap, size_t *n, int depth) {
    if (depth > 6 || *n + 8 >= cap) {
        *n += (size_t)snprintf(dst + *n, cap - *n, "null");
        return;
    }
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            *n += (size_t)snprintf(dst + *n, cap - *n, "null"); return;
        case LUA_TBOOLEAN:
            *n += (size_t)snprintf(dst + *n, cap - *n, lua_toboolean(L, idx) ? "true" : "false");
            return;
        case LUA_TNUMBER: {
            if (lua_isinteger(L, idx)) {
                *n += (size_t)snprintf(dst + *n, cap - *n, "%lld",
                                       (long long)lua_tointeger(L, idx));
            } else {
                *n += (size_t)snprintf(dst + *n, cap - *n, "%g", (double)lua_tonumber(L, idx));
            }
            return;
        }
        case LUA_TSTRING: {
            const char *s = lua_tostring(L, idx);
            *n += (size_t)snprintf(dst + *n, cap - *n, "\"");
            *n += api_json_escape(dst + *n, cap - *n, s);
            *n += (size_t)snprintf(dst + *n, cap - *n, "\"");
            return;
        }
        case LUA_TTABLE: {
            lua_Integer len = luaL_len(L, idx);
            bool is_array = len > 0;
            if (is_array) {
                *n += (size_t)snprintf(dst + *n, cap - *n, "[");
                for (lua_Integer i = 1; i <= len && *n + 4 < cap; i++) {
                    if (i > 1) { *n += (size_t)snprintf(dst + *n, cap - *n, ","); }
                    lua_geti(L, idx, i);
                    enc(L, lua_gettop(L), dst, cap, n, depth + 1);
                    lua_pop(L, 1);
                }
                *n += (size_t)snprintf(dst + *n, cap - *n, "]");
            } else {
                *n += (size_t)snprintf(dst + *n, cap - *n, "{");
                bool first = true;
                lua_pushnil(L);
                int t = lua_absindex(L, idx);
                while (lua_next(L, t) != 0) {
                    if (lua_type(L, -2) == LUA_TSTRING && *n + 8 < cap) {
                        if (!first) { *n += (size_t)snprintf(dst + *n, cap - *n, ","); }
                        *n += (size_t)snprintf(dst + *n, cap - *n, "\"");
                        *n += api_json_escape(dst + *n, cap - *n, lua_tostring(L, -2));
                        *n += (size_t)snprintf(dst + *n, cap - *n, "\":");
                        enc(L, lua_gettop(L), dst, cap, n, depth + 1);
                        first = false;
                    }
                    lua_pop(L, 1);
                }
                *n += (size_t)snprintf(dst + *n, cap - *n, "}");
            }
            return;
        }
        default:
            *n += (size_t)snprintf(dst + *n, cap - *n, "null");
    }
}

static char enc_buf[1536];

/* --- bus ------------------------------------------------------------------ */
static int l_bus_emit(lua_State *L) {
    const char *topic = luaL_checkstring(L, 1);
    /* profile.* is reserved for scripts precisely so one can never forge a
     * core event such as dev.connected. */
    char full[72];
    snprintf(full, sizeof(full), "profile.%s", topic);
    size_t n = 0;
    enc_buf[0] = 0;
    if (lua_gettop(L) >= 2) { enc(L, 2, enc_buf, sizeof(enc_buf), &n, 0); }
    else { snprintf(enc_buf, sizeof(enc_buf), "null"); }
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
    for (int i = 0; i < n_lua_ops; i++) {
        if (strcmp(lua_ops[i].name, name) == 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, lua_ops[i].fn_ref);
            lua_pushvalue(L, 2);
            lua_ops[i].fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
            return 0;
        }
    }
    if (n_lua_ops >= MAX_LUA_OPS) { return luaL_error(L, "too many registered ops"); }
    snprintf(lua_ops[n_lua_ops].name, sizeof(lua_ops[n_lua_ops].name), "%s", name);
    lua_pushvalue(L, 2);
    lua_ops[n_lua_ops].fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    n_lua_ops++;
    return 0;
}

bool lbind_dispatch(const char *name, const char *params_json,
                    void (*reply)(const char *, void *), void *ctx, long id) {
    if (!gL) { return false; }
    for (int i = 0; i < n_lua_ops; i++) {
        if (strcmp(lua_ops[i].name, name) != 0) { continue; }
        lua_rawgeti(gL, LUA_REGISTRYINDEX, lua_ops[i].fn_ref);
        lua_pushstring(gL, params_json ? params_json : "{}");
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
        size_t n = 0;
        enc_buf[0] = 0;
        enc(gL, lua_gettop(gL), enc_buf, sizeof(enc_buf), &n, 0);
        lua_pop(gL, 1);
        char out[1700];
        snprintf(out, sizeof(out), "{\"id\":%ld,\"ok\":true,\"result\":%s}", id, enc_buf);
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
static int l_bt_devices(lua_State *L) {
    lua_newtable(L);
    int idx = 1;
    for (size_t i = 0; i < bt_registry_count(); i++) {
        bt_device_t *d = bt_registry_at(i);
        if (!d) { continue; }
        lua_newtable(L);
        char addr[20];
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 d->addr[0], d->addr[1], d->addr[2], d->addr[3], d->addr[4], d->addr[5]);
        lua_pushstring(L, addr);              lua_setfield(L, -2, "addr");
        lua_pushstring(L, bt_transport_name(d->transport)); lua_setfield(L, -2, "transport");
        lua_pushinteger(L, d->rssi);          lua_setfield(L, -2, "rssi");
        lua_pushboolean(L, d->connected);     lua_setfield(L, -2, "connected");
        if (d->have_name) { lua_pushstring(L, d->name); lua_setfield(L, -2, "name"); }
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

static int l_bt_connect(lua_State *L) {
    uint8_t a[6];
    if (!addr_from_lua(L, 1, a)) { return luaL_error(L, "expected an address"); }
    bt_device_t *d = bt_registry_find(a, BT_TRANSPORT_LE);
    if (!d) { lua_pushboolean(L, 0); lua_pushstring(L, "not in the scan table"); return 2; }
    lua_pushboolean(L, le_connect(d->addr, d->addr_type));
    return 1;
}

static int l_bt_write(lua_State *L) {
    uint8_t a[6];
    if (!addr_from_lua(L, 1, a)) { return luaL_error(L, "expected an address"); }
    size_t hexlen = 0;
    const char *id = luaL_checkstring(L, 2);
    const char *hex = luaL_checklstring(L, 3, &hexlen);
    bool with_resp = lua_isnoneornil(L, 4) ? true : lua_toboolean(L, 4);

    bt_device_t *d = bt_registry_find(a, BT_TRANSPORT_LE);
    if (!d || !d->connected) { lua_pushboolean(L, 0); lua_pushstring(L, "not connected"); return 2; }
    gattdb_t *db = le_db_for(d->con_handle);
    uint8_t si, ci;
    if (!db || !db->ready || !gattdb_resolve_chr(db, id, &si, &ci)) {
        lua_pushboolean(L, 0); lua_pushstring(L, "no such characteristic"); return 2;
    }
    static uint8_t buf[512];
    size_t n = hexlen / 2;
    if (n > sizeof(buf)) { n = sizeof(buf); }
    for (size_t i = 0; i < n; i++) {
        unsigned b = 0; sscanf(hex + i * 2, "%2x", &b); buf[i] = (uint8_t)b;
    }
    lua_pushboolean(L, le_write(d->addr, si, ci, buf, (uint16_t)n, with_resp) == 0);
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

/* --- install -------------------------------------------------------------- */
static void set_table(lua_State *L, const char *name, const luaL_Reg *fns) {
    luaL_newlib(L, fns);
    lua_setglobal(L, name);
}

void lbind_open_all(lua_State *L) {
    gL = L;

    static const luaL_Reg bus[] = { { "emit", l_bus_emit }, { NULL, NULL } };
    set_table(L, "bus", bus);

    static const luaL_Reg lg[] = {
        { "info", l_log_info }, { "warn", l_log_warn }, { "error", l_log_error }, { NULL, NULL } };
    set_table(L, "log", lg);

    static const luaL_Reg ops[] = { { "register", l_ops_register }, { NULL, NULL } };
    set_table(L, "ops", ops);

    static const luaL_Reg sys[] = {
        { "ms", l_sys_ms }, { "sleep", l_sys_sleep }, { NULL, NULL } };
    set_table(L, "sys", sys);

#if CONFIG_BT_LE
    static const luaL_Reg bt[] = {
        { "devices", l_bt_devices }, { "connect", l_bt_connect },
        { "write", l_bt_write }, { "subscribe", l_bt_subscribe }, { NULL, NULL } };
    set_table(L, "bt", bt);
#endif
}
