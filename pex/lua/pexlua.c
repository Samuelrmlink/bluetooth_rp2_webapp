/* pexlua: see pexlua.h. */
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "pexlua.h"

static const pexlua_host_t *s_host;
static lua_State *s_L;
static int s_detaching;

#define ANCHOR_MT "pex.anchor"
#define ANCHORS "_PEXANCHORS"      /* registry: lightuserdata(mod) -> anchor (weak values) */
#define REQCACHE "_PEXREQUIRE"     /* registry: name -> what require returned */

/* ---- the lua group: thin, fixed-width wrappers (host_api.json group 'lua') */
int pexlua_gettop(lua_State *L) { return lua_gettop(L); }
void pexlua_settop(lua_State *L, int idx) { lua_settop(L, idx); }
void pexlua_pushvalue(lua_State *L, int idx) { lua_pushvalue(L, idx); }
int pexlua_type(lua_State *L, int idx) { return lua_type(L, idx); }
int64_t pexlua_checkint(lua_State *L, int arg) { return (int64_t)luaL_checkinteger(L, arg); }
int64_t pexlua_optint(lua_State *L, int arg, int64_t def) { return (int64_t)luaL_optinteger(L, arg, (lua_Integer)def); }
double pexlua_checknum(lua_State *L, int arg) { return (double)luaL_checknumber(L, arg); }
const char *pexlua_checkstr(lua_State *L, int arg, size_t *len) { return luaL_checklstring(L, arg, len); }
const char *pexlua_optstr(lua_State *L, int arg, const char *def, size_t *len) {
    const char *s = luaL_optlstring(L, arg, def, len);
    if (s == def && len) { *len = def ? strlen(def) : 0; }
    return s;
}
int pexlua_toboolean(lua_State *L, int idx) { return lua_toboolean(L, idx); }
void pexlua_checktype(lua_State *L, int arg, int type) { luaL_checktype(L, arg, type); }
void pexlua_pushint(lua_State *L, int64_t v) { lua_pushinteger(L, (lua_Integer)v); }
void pexlua_pushnum(lua_State *L, double v) { lua_pushnumber(L, (lua_Number)v); }
void pexlua_pushstr(lua_State *L, const char *s) { lua_pushstring(L, s); }
void pexlua_pushbytes(lua_State *L, const void *p, size_t len) { lua_pushlstring(L, (const char *)p, len); }
void pexlua_pushbool(lua_State *L, int b) { lua_pushboolean(L, b); }
void pexlua_pushnil(lua_State *L) { lua_pushnil(L); }
void pexlua_newtable(lua_State *L, int narr, int nrec) { lua_createtable(L, narr, nrec); }
void pexlua_setfield(lua_State *L, int idx, const char *k) { lua_setfield(L, idx, k); }
int pexlua_getfield(lua_State *L, int idx, const char *k) { return lua_getfield(L, idx, k); }
void pexlua_seti(lua_State *L, int idx, int64_t i) { lua_seti(L, idx, (lua_Integer)i); }
int pexlua_geti(lua_State *L, int idx, int64_t i) { return lua_geti(L, idx, (lua_Integer)i); }
size_t pexlua_rawlen(lua_State *L, int idx) { return (size_t)lua_rawlen(L, idx); }
int pexlua_error(lua_State *L, const char *msg) { return luaL_error(L, "%s", msg ? msg : "error"); }
int pexlua_argerror(lua_State *L, int arg, const char *msg) { return luaL_argerror(L, arg, msg); }

/* ---- anchors: one per module instance, holding a pex_ref while any Lua
 * function of the module is alive. Owned (require'd) modules are unloaded
 * when the anchor is collected. */
typedef struct {
    pex_mod_t *m;
    int owned;
    int warned;       /* one log line per anchor after unload */
    char name[24];
} anchor_t;

static void anchor_set_name(anchor_t *a, pex_mod_t *m) {
    const char *n = pex_name(m);
    snprintf(a->name, sizeof a->name, "%s", n ? n : "");
}

static bool mod_alive(pex_mod_t *m) {
    for (pex_mod_t *x = pex_first(); x; x = pex_next(x)) {
        if (x == m) { return true; }
    }
    return false;
}

static int anchor_gc(lua_State *L) {
    anchor_t *a = luaL_checkudata(L, 1, ANCHOR_MT);
    if (a->m && mod_alive(a->m)) {
        pex_unref(a->m);
        if (a->owned) {
            pex_stop(a->m);
            if (pex_unload(a->m) != PEX_OK) {
                printf("pexlua: %s not unloaded after its last Lua reference: %s\n", pex_name(a->m), pex_errmsg());
            }
        }
    }
    a->m = NULL;
    return 0;
}

/* Pushes the module's anchor (creating it). */
static void push_anchor(lua_State *L, pex_mod_t *m, int owned) {
    lua_getfield(L, LUA_REGISTRYINDEX, ANCHORS);
    lua_pushlightuserdata(L, m);
    if (lua_rawget(L, -2) == LUA_TUSERDATA) {
        anchor_t *a = lua_touserdata(L, -1);
        if (a->m && mod_alive(a->m)) {
            if (owned) { a->owned = 1; }
        } else {
            /* Detached, or the record was freed and this address came back. */
            a->m = m;
            a->owned = owned;
            a->warned = 0;
            anchor_set_name(a, m);
            pex_ref(m);
        }
        lua_remove(L, -2);
        return;
    }
    lua_pop(L, 1);
    anchor_t *a = lua_newuserdatauv(L, sizeof *a, 0);
    a->m = m;
    a->owned = owned;
    a->warned = 0;
    anchor_set_name(a, m);
    pex_ref(m);
    luaL_setmetatable(L, ANCHOR_MT);
    lua_pushlightuserdata(L, m);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4);
    lua_remove(L, -2);
}

/* ---- calling module code from Lua, protected ---------------------------- */
/* upvalues: 1 lightuserdata module, 2 integer fn address (0 = pex_main LUA_OPEN), 3 anchor */
static int inner(lua_State *L) {
    pex_mod_t *m = lua_touserdata(L, lua_upvalueindex(1));
    uint32_t fn = (uint32_t)lua_tointeger(L, lua_upvalueindex(2));
    int r = fn ? (int)(uint32_t)pex_call(m, fn, (uint32_t)(uintptr_t)L, 0, 0, 0)
               : pex_entry(m, PEX_LUA_OPEN, L);
    if (r < 0) {
        return luaL_error(L, "%s: %s", pex_name(m), pex_strerror(r));
    }
    if (r > lua_gettop(L)) { return luaL_error(L, "%s returned %d values, pushed fewer", pex_name(m), r); }
    return r;
}

static int thunk(lua_State *L) {
    anchor_t *a = lua_touserdata(L, lua_upvalueindex(3));
    pex_mod_t *m = a ? a->m : NULL;
    if (!m || !mod_alive(m)) {
        const char *nm = (a && a->name[0]) ? a->name : "module";
        if (a && !a->warned) {
            a->warned = 1;
            printf("pexlua: Lua used %s after it was unloaded\n", nm);
        }
        return luaL_error(L, "PEX module '%s' was unloaded", nm);
    }
    int n = lua_gettop(L);
    uint32_t depth = pex_depth();
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushvalue(L, lua_upvalueindex(3));
    lua_pushcclosure(L, inner, 3);
    lua_insert(L, 1);
    if (lua_pcall(L, n, LUA_MULTRET, 0) != LUA_OK) {
        pex_unwind(depth);           /* the error left module frames behind: fix the books */
        return lua_error(L);
    }
    return lua_gettop(L);
}

void pexlua_register(lua_State *L, const pexlua_reg_t *fns, uint32_t n) {
    pex_mod_t *m = pex_self();
    if (!m) { luaL_error(L, "pexlua_register outside a module call"); return; }
    for (uint32_t i = 0; i < n; i++) {
        lua_pushlightuserdata(L, m);
        lua_pushinteger(L, (lua_Integer)(uint32_t)(uintptr_t)fns[i].fn);
        push_anchor(L, m, 0);
        lua_pushcclosure(L, thunk, 3);
        lua_setfield(L, -2, fns[i].name);
    }
}

/* ---- the launcher ------------------------------------------------------- */
static pex_mod_t *checkmod(lua_State *L, int arg) {
    const char *nm = luaL_checkstring(L, arg);
    pex_mod_t *m = pex_find(nm);
    if (!m) { luaL_error(L, "no PEX module '%s' loaded", nm); }
    return m;
}

static int fail(lua_State *L, const char *what) {
    lua_pushnil(L);
    lua_pushfstring(L, "%s: %s", what, pex_errmsg());
    return 2;
}

static void push_info(lua_State *L, pex_mod_t *m) {
    pex_info_t in;
    pex_info(m, &in);
    lua_createtable(L, 0, 20);
    lua_pushstring(L, in.name); lua_setfield(L, -2, "name");
    lua_pushstring(L, in.desc ? in.desc : ""); lua_setfield(L, -2, "desc");
    lua_pushstring(L, pex_state_name(in.state)); lua_setfield(L, -2, "state");
    char v[24];
    snprintf(v, sizeof v, "%lu.%lu.%lu", (unsigned long)(in.version >> 24), (unsigned long)((in.version >> 16) & 0xFF),
             (unsigned long)(in.version & 0xFFFF));
    lua_pushstring(L, v); lua_setfield(L, -2, "version");
    char bid[33];
    for (int i = 0; i < 16; i++) { snprintf(bid + 2 * i, 3, "%02x", in.build_id[i]); }
    lua_pushstring(L, bid); lua_setfield(L, -2, "build_id");
    lua_pushstring(L, (in.flags & PEX_F_PIC) ? "pic" : "reloc"); lua_setfield(L, -2, "model");
    lua_pushboolean(L, (in.flags & PEX_F_APP) != 0); lua_setfield(L, -2, "app");
    lua_pushboolean(L, in.xip); lua_setfield(L, -2, "xip");
    lua_pushinteger(L, in.code_size); lua_setfield(L, -2, "code");
    lua_pushinteger(L, in.rw_size); lua_setfield(L, -2, "rw");
    lua_pushboolean(L, in.code_shared); lua_setfield(L, -2, "shared");
    lua_pushinteger(L, in.active); lua_setfield(L, -2, "active");
    lua_pushinteger(L, in.refs); lua_setfield(L, -2, "refs");
    lua_pushinteger(L, in.tasks); lua_setfield(L, -2, "tasks");
    lua_pushinteger(L, in.exit_code); lua_setfield(L, -2, "exit_code");
    lua_pushinteger(L, in.mem.long_bytes); lua_setfield(L, -2, "long_bytes");
    lua_pushinteger(L, in.mem.long_blocks); lua_setfield(L, -2, "long_blocks");
    lua_pushinteger(L, in.mem.scratch_peak); lua_setfield(L, -2, "scratch_peak");
    lua_createtable(L, (int)in.n_exports, 0);
    for (uint32_t i = 0; i < in.n_exports; i++) {
        lua_pushstring(L, pex_export_name(m, i));
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "exports");
    const pex_fault_t *f = pex_last_fault();
    if (in.state == PEX_ST_FAULTED && f && strcmp(f->name, in.name) == 0) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, f->offset); lua_setfield(L, -2, "offset");
        lua_pushinteger(L, f->pc); lua_setfield(L, -2, "pc");
        lua_pushboolean(L, f->in_host); lua_setfield(L, -2, "in_host");
        lua_setfield(L, -2, "fault");
    }
}

static int l_load(lua_State *L) {
    const char *nm = luaL_checkstring(L, 1);
    const char *path = luaL_optstring(L, 2, NULL);
    char buf[96];
    if (!s_host) { return luaL_error(L, "pex: no host loader"); }
    if (pex_find(nm)) { lua_pushnil(L); lua_pushfstring(L, "'%s' is already loaded", nm); return 2; }
    if (!path) {
        if (!s_host->find(nm, buf, sizeof buf)) {
            lua_pushnil(L);
            lua_pushfstring(L, "no %s.<arch>.pex found", nm);
            return 2;
        }
        path = buf;
    }
    int err;
    pex_mod_t *m = s_host->load(path, nm, &err);
    if (!m) { return fail(L, path); }
    push_info(L, m);
    return 1;
}

static int l_start(lua_State *L) {
    pex_mod_t *m = checkmod(L, 1);
    const char *cfg = NULL;
    if (lua_istable(L, 2)) {
        /* the firmware's json.encode, if the state has it */
        if (lua_getglobal(L, "json") != LUA_TTABLE || lua_getfield(L, -1, "encode") != LUA_TFUNCTION) {
            return luaL_error(L, "pex.start: a table config needs json.encode (pass a JSON string instead)");
        }
        lua_pushvalue(L, 2);
        lua_call(L, 1, 1);
        cfg = lua_tostring(L, -1);
    } else if (!lua_isnoneornil(L, 2)) {
        cfg = luaL_checkstring(L, 2);
    }
    if (pex_start(m, cfg) != PEX_OK) { return fail(L, pex_name(m)); }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_stop(lua_State *L) {
    pex_mod_t *m = checkmod(L, 1);
    if (pex_stop(m) != PEX_OK) { return fail(L, pex_name(m)); }
    lua_pushboolean(L, 1);
    return 1;
}

/* Tell Lua, then drop the anchor. The module stays loaded. A nested
 * pex.unload from the handler returns true and does not run again: this
 * call is already unloading it. */
static int detach(lua_State *L, pex_mod_t *m) {
    if (!L || !m || s_detaching) { return 0; }
    pex_info_t in;
    if (pex_info(m, &in) != PEX_OK) { return 0; }
    if (in.tasks > 0 || in.active > 0) { return 0; }

    char name[24];
    const char *nm = pex_name(m);
    snprintf(name, sizeof name, "%s", nm ? nm : "");

    int top = lua_gettop(L);
    s_detaching = 1;
    int had_handler = 0;
    int had_anchor = 0;

    if (lua_getglobal(L, "pex") == LUA_TTABLE) {
        if (lua_getfield(L, -1, "on_unload") == LUA_TFUNCTION) {
            had_handler = 1;
            lua_pushstring(L, name);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                const char *e = lua_tostring(L, -1);
                printf("pexlua: pex.on_unload(%s): %s\n", name, e ? e : "error");
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }

    lua_getfield(L, LUA_REGISTRYINDEX, REQCACHE);
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, ANCHORS);
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_pushlightuserdata(L, m);
        if (lua_rawget(L, -2) == LUA_TUSERDATA) {
            anchor_t *a = lua_touserdata(L, -1);
            if (a && a->m) {
                had_anchor = 1;
                snprintf(a->name, sizeof a->name, "%s", name);
                pex_unref(a->m);
                a->m = NULL;
                a->owned = 0;
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    if (had_anchor && !had_handler) {
        printf("pexlua: no pex.on_unload handler for %s\n", name);
    }
    if (had_anchor && pex_info(m, &in) == PEX_OK && in.refs > 0) {
        printf("pexlua: Lua was detached from %s but %d reference%s remain%s\n",
               name, in.refs, in.refs == 1 ? "" : "s", in.refs == 1 ? "s" : "");
    }
    lua_settop(L, top);
    s_detaching = 0;
    return had_anchor;
}

static int l_unload(lua_State *L) {
    const char *nm = luaL_checkstring(L, 1);
    if (s_detaching) { lua_pushboolean(L, 1); return 1; }
    pex_mod_t *m = pex_find(nm);
    if (!m) { lua_pushboolean(L, 1); return 1; }
    int detached = detach(L, m);
    lua_gc(L, LUA_GCCOLLECT, 0);
    m = pex_find(nm);
    if (!m) { lua_pushboolean(L, 1); return 1; }
    /* refused while it runs, as pex.unload over the API is: pex.stop first */
    if (pex_unload(m) != PEX_OK) {
        if (detached) {
            printf("pexlua: Lua was detached from %s but unload failed: %s\n", nm, pex_errmsg());
        }
        return fail(L, nm);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_list(lua_State *L) {
    lua_newtable(L);
    int i = 1;
    for (pex_mod_t *m = pex_first(); m; m = pex_next(m)) {
        push_info(L, m);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int l_info(lua_State *L) {
    pex_mod_t *m = pex_find(luaL_checkstring(L, 1));
    if (!m) { lua_pushnil(L); lua_pushstring(L, "not loaded"); return 2; }
    push_info(L, m);
    return 1;
}

static int l_call(lua_State *L) {
    pex_mod_t *m = checkmod(L, 1);
    const char *fn = luaL_checkstring(L, 2);
    uint32_t a = pex_sym(m, fn);
    if (!a) { return luaL_error(L, "%s exports no '%s'", pex_name(m), fn); }
    uint32_t v[4] = { 0 };
    for (int i = 0; i < 4; i++) { v[i] = (uint32_t)luaL_optinteger(L, 3 + i, 0); }
    uint64_t r = pex_call(m, a, v[0], v[1], v[2], v[3]);
    lua_pushinteger(L, (lua_Integer)(int32_t)(uint32_t)r);
    return 1;
}

/* ---- XIP: the flash partition (0.7) -------------------------------------- */
static void push_installed(lua_State *L, const pex_installed_t *in) {
    lua_createtable(L, 0, 12);
    lua_pushstring(L, in->name); lua_setfield(L, -2, "name");
    char v[24];
    snprintf(v, sizeof v, "%lu.%lu.%lu", (unsigned long)(in->version >> 24), (unsigned long)((in->version >> 16) & 0xFF),
             (unsigned long)(in->version & 0xFFFF));
    lua_pushstring(L, v); lua_setfield(L, -2, "version");
    char bid[33];
    for (int i = 0; i < 16; i++) { snprintf(bid + 2 * i, 3, "%02x", in->build_id[i]); }
    lua_pushstring(L, bid); lua_setfield(L, -2, "build_id");
    lua_pushstring(L, (in->flags & PEX_F_PIC) ? "pic" : "reloc"); lua_setfield(L, -2, "model");
    lua_pushinteger(L, in->offset); lua_setfield(L, -2, "offset");
    lua_pushinteger(L, in->file_size); lua_setfield(L, -2, "size");
    lua_pushinteger(L, in->code_size); lua_setfield(L, -2, "code");
    lua_pushinteger(L, in->rw_size); lua_setfield(L, -2, "rw");
    lua_pushboolean(L, in->signed_); lua_setfield(L, -2, "signed");
    lua_pushboolean(L, in->loaded); lua_setfield(L, -2, "loaded");
    lua_pushboolean(L, in->xip_ready); lua_setfield(L, -2, "xip_ready");
}

static int l_install(lua_State *L) {
    const char *what = luaL_checkstring(L, 1);
    if (!s_host || !s_host->install) { lua_pushnil(L); lua_pushstring(L, "this host cannot install modules"); return 2; }
    int slot = s_host->install(what);
    pex_installed_t in;
    if (slot < 0 || pex_installed(slot, &in) != PEX_OK) { return fail(L, what); }
    push_installed(L, &in);
    return 1;
}

static int l_installed(lua_State *L) {
    lua_newtable(L);
    pex_installed_t in;
    for (int i = 0; pex_installed(i, &in) == PEX_OK; i++) {
        push_installed(L, &in);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_uninstall(lua_State *L) {
    const char *nm = luaL_checkstring(L, 1);
    if (pex_uninstall(nm) != PEX_OK) { return fail(L, nm); }
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- require: x.<arch>.pex before the Lua source (spec 9) -------------------
 * pex_require(name) returns the module's value, or nothing when there is no
 * loadable PEX for this target. The global require becomes a small LUA
 * function that TAIL-calls the firmware's own require otherwise: a tail call
 * keeps the script's frame at level 1, which is how lrt's require finds the
 * caller's environment (sandbox). A C wrapper would hide it. */
static int l_pex_require(lua_State *L) {
    const char *nm = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, REQCACHE);
    if (lua_getfield(L, -1, nm) != LUA_TNIL) { return 1; }
    lua_pop(L, 1);
    char path[96];
    if (!s_host || !s_host->find(nm, path, sizeof path)) { return 0; }
    pex_mod_t *m = pex_find(nm);
    int err = 0;
    if (!m) { m = s_host->load(path, nm, &err); }
    if (!m) {
        printf("pexlua: require '%s': %s -- trying the Lua source\n", nm, pex_errmsg());
        return 0;
    }
    /* PEX_LUA_OPEN, protected like any other call into the module */
    lua_pushlightuserdata(L, m);
    lua_pushinteger(L, 0);
    push_anchor(L, m, 1);
    lua_pushcclosure(L, thunk, 3);
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, nm);
    return 1;
}

static const char REQUIRE_WRAPPER[] =
    "local orig, pexreq = ...\n"
    "return function(name)\n"
    "  local r = pexreq(name)\n"
    "  if r ~= nil then return r end\n"
    "  return orig(name)\n"
    "end\n";

void pexlua_close(void) {
    s_L = NULL;
    s_detaching = 0;
}

int pexlua_before_unload(pex_mod_t *m) {
    if (!s_L) { return 0; }
    return detach(s_L, m);
}

void pexlua_open(lua_State *L, const pexlua_host_t *host) {
    static bool registered;
    if (!registered) {
        pex_register_group(&pex_group_lua);
        registered = true;
    }
    s_host = host;
    s_L = L;
    luaL_newmetatable(L, ANCHOR_MT);
    lua_pushcfunction(L, anchor_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_createtable(L, 0, 1);
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, ANCHORS);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, REQCACHE);

    static const luaL_Reg fns[] = {
        { "load", l_load }, { "start", l_start }, { "stop", l_stop }, { "unload", l_unload },
        { "list", l_list }, { "info", l_info }, { "call", l_call }, { "install", l_install },
        { "installed", l_installed }, { "uninstall", l_uninstall }, { NULL, NULL } };
    luaL_newlib(L, fns);
    lua_setglobal(L, "pex");

    if (luaL_loadbufferx(L, REQUIRE_WRAPPER, sizeof REQUIRE_WRAPPER - 1, "=pexrequire", "t") == LUA_OK) {
        lua_getglobal(L, "require");
        lua_pushcfunction(L, l_pex_require);
        lua_call(L, 2, 1);
        lua_setglobal(L, "require");
    } else {
        printf("pexlua: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}
