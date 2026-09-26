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
#include "fs/fs.h"
#include "pico/time.h"
#include "autoconf.h"

#if CONFIG_BT_LE
#include "bt/le/central.h"
#include "bt/le/gattdb.h"
#include "btstack.h"
#include "bt/le/gattserver.h"
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

/* Drain one queued notification, non-blocking. nil when the inbox is empty.
 *
 * This is the primitive under the profile's link:await(): a Lua helper polls
 * it and sys.sleep()s between tries, which is why the scheduler needed no new
 * yield discipline for the handshake to read as straight-line code. The poll
 * interval costs one coroutine resume every few milliseconds against a device
 * that takes ~900 ms to answer its key exchange. */
static int l_bt_notify_take(lua_State *L) {
    char addr[18], id[8];
    static uint8_t val[256];
    int n = le_notify_take(addr, sizeof addr, id, sizeof id, val, sizeof val);
    if (n < 0) { lua_pushnil(L); return 1; }
    lua_createtable(L, 0, 3);
    lua_pushstring(L, addr);                  lua_setfield(L, -2, "addr");
    lua_pushstring(L, id);                    lua_setfield(L, -2, "id");
    lua_pushlstring(L, (const char *)val, (size_t)n); lua_setfield(L, -2, "value");
    return 1;
}

static int l_bt_notify_dropped(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)le_notify_dropped());
    return 1;
}

static int l_bt_notify_flush(lua_State *L) { (void)L; le_notify_flush(); return 0; }

#endif

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

    static const luaL_Reg bus[] = { { "emit", l_bus_emit }, { NULL, NULL } };
    set_table(L, "bus", bus);

    static const luaL_Reg lg[] = {
        { "info", l_log_info }, { "warn", l_log_warn }, { "error", l_log_error }, { NULL, NULL } };
    set_table(L, "log", lg);

    static const luaL_Reg ops[] = { { "register", l_ops_register }, { NULL, NULL } };
    set_table(L, "ops", ops);

    static const luaL_Reg sys[] = {
        { "ms", l_sys_ms }, { "sleep", l_sys_sleep },
        { "secret", l_sys_secret }, { NULL, NULL } };
    set_table(L, "sys", sys);

#if CONFIG_BT_LE
    static const luaL_Reg gs[] = {
        { "begin", l_gs_begin }, { "service", l_gs_service },
        { "characteristic", l_gs_characteristic }, { "commit", l_gs_commit },
        { "notify", l_gs_notify }, { NULL, NULL } };
    set_table(L, "gatt_server", gs);

    static const luaL_Reg bt[] = {
        { "devices", l_bt_devices }, { "connect", l_bt_connect },
        { "write", l_bt_write }, { "subscribe", l_bt_subscribe },
        { "find", l_bt_find },
        { "notify_take", l_bt_notify_take },
        { "notify_dropped", l_bt_notify_dropped },
        { "notify_flush", l_bt_notify_flush },
        { NULL, NULL } };
    set_table(L, "bt", bt);
#endif

#if CONFIG_PROTO_ANKER
    lbind_proto_open(L);
#endif
}
