/* Lua's hold on a module must not block a manual unload, and a call after
 * that unload must fail loudly. Host gcc, no board and no pico-sdk.
 *
 * The bridge stores a callback address in a uint32, which cannot hold a
 * 64-bit function pointer, so pex_call here ignores that word and calls the
 * one function this test registers.
 *
 * Numbers follow third_party/lua/luaconf.h. The firmware also passes
 * -DLUA_32BITS=1; that macro is redefined by luaconf.h (a pre-existing
 * warning). This test does not pass it.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lauxlib.h"
#include "lualib.h"
#include "pexlua.h"
#include "pex_internal.h"

static int checks, fails;
#define CHECK(c) do { \
    checks++; \
    if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } \
} while (0)

static pex_mod_t g_mod;
static pex_mod_t *g_list;
static pex_mod_t *g_cur;
static char g_err[128];

static void reset_mod(void) {
    memset(&g_mod, 0, sizeof g_mod);
    snprintf(g_mod.name, sizeof g_mod.name, "hello");
    g_list = &g_mod;
    snprintf(g_err, sizeof g_err, "ok");
}

static int l_ping(lua_State *L) {
    lua_pushinteger(L, 7);
    return 1;
}

static int host_find(const char *name, char *path, size_t cap) {
    if (!name || strcmp(name, "hello") != 0) { return 0; }
    snprintf(path, cap, "mem:hello");
    return 1;
}

static pex_mod_t *host_load(const char *path, const char *as, int *err) {
    (void)path;
    (void)as;
    if (!g_list) { reset_mod(); }
    if (err) { *err = 0; }
    return g_list;
}

pex_mod_t *pex_self(void) { return g_cur; }
void pex_ref(pex_mod_t *m) { if (m) { m->refs++; } }
void pex_unref(pex_mod_t *m) { if (m && m->refs > 0) { m->refs--; } }
const char *pex_name(pex_mod_t *m) { return m && m->name[0] ? m->name : "host"; }
const char *pex_errmsg(void) { return g_err; }
const char *pex_strerror(int err) { (void)err; return g_err; }
pex_mod_t *pex_find(const char *name) {
    if (g_list && name && strcmp(g_list->name, name) == 0) { return g_list; }
    return NULL;
}
pex_mod_t *pex_first(void) { return g_list; }
pex_mod_t *pex_next(pex_mod_t *m) { (void)m; return NULL; }
int pex_stop(pex_mod_t *m) { if (m) { m->tasks = 0; } return PEX_OK; }
int pex_start(pex_mod_t *m, const char *cfg) { (void)m; (void)cfg; return PEX_OK; }
uint32_t pex_depth(void) { return 0; }
void pex_unwind(uint32_t depth) { (void)depth; }
uint32_t pex_sym(pex_mod_t *m, const char *name) { (void)m; (void)name; return 0; }
const char *pex_state_name(pex_state_t s) { (void)s; return "loaded"; }
const char *pex_export_name(pex_mod_t *m, uint32_t i) { (void)m; (void)i; return ""; }
const pex_fault_t *pex_last_fault(void) { return NULL; }
int pex_installed(int i, pex_installed_t *out) { (void)i; (void)out; return PEX_ENOENT; }
int pex_uninstall(const char *name) { (void)name; return PEX_ENOENT; }
int pex_register_group(const pex_group_t *g) { (void)g; return PEX_OK; }
const pex_group_t pex_group_lua;

int pex_info(pex_mod_t *m, pex_info_t *out) {
    if (!m || !out) { return PEX_ENOENT; }
    memset(out, 0, sizeof *out);
    out->name = m->name;
    out->active = m->active;
    out->refs = m->refs;
    out->tasks = m->tasks;
    return PEX_OK;
}

int pex_unload(pex_mod_t *m) {
    if (!m || m != g_list) { snprintf(g_err, sizeof g_err, "no such module"); return PEX_ENOENT; }
    if (m->active > 0 || m->refs > 0 || m->tasks > 0) {
        snprintf(g_err, sizeof g_err, "busy");
        return PEX_EBUSY;
    }
    g_list = NULL;
    snprintf(g_err, sizeof g_err, "ok");
    return PEX_OK;
}

uint64_t pex_call(pex_mod_t *m, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)m; (void)fn; (void)a1; (void)a2; (void)a3;
    return (uint64_t)l_ping((lua_State *)(uintptr_t)a0);
}

int pex_entry(pex_mod_t *m, uint32_t reason, void *arg) {
    if (reason != PEX_LUA_OPEN) { return 0; }
    g_cur = m;
    lua_State *L = arg;
    lua_newtable(L);
    static const pexlua_reg_t fns[] = { { "ping", l_ping } };
    pexlua_register(L, fns, 1);
    g_cur = NULL;
    return 1;
}

static int cap_saved = -1;
static FILE *cap_fp;

static void capture_begin(void) {
    fflush(stdout);
    cap_fp = tmpfile();
    cap_saved = dup(fileno(stdout));
    dup2(fileno(cap_fp), fileno(stdout));
}

static void capture_end(char *buf, size_t n) {
    fflush(stdout);
    if (cap_saved >= 0) {
        dup2(cap_saved, fileno(stdout));
        close(cap_saved);
        cap_saved = -1;
    }
    buf[0] = 0;
    if (!cap_fp) { return; }
    rewind(cap_fp);
    size_t k = fread(buf, 1, n - 1, cap_fp);
    buf[k] = 0;
    fclose(cap_fp);
    cap_fp = NULL;
}

static int count_sub(const char *s, const char *sub) {
    int n = 0;
    size_t m = strlen(sub);
    if (!m) { return 0; }
    for (const char *p = s; (p = strstr(p, sub)); p += m) { n++; }
    return n;
}

static int lua_ok(lua_State *L, const char *src) {
    if (luaL_dostring(L, src) == LUA_OK) { return 1; }
    fprintf(stderr, "lua: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return 0;
}

static void copy_global(lua_State *L, const char *key, char *dst, size_t n) {
    lua_getglobal(L, key);
    const char *s = lua_tostring(L, -1);
    snprintf(dst, n, "%s", s ? s : "");
    lua_pop(L, 1);
}

static int global_int(lua_State *L, const char *key, lua_Integer *out) {
    lua_getglobal(L, key);
    int ok = lua_isinteger(L, -1);
    if (ok) { *out = lua_tointeger(L, -1); }
    lua_pop(L, 1);
    return ok;
}

static int global_bool(lua_State *L, const char *key) {
    lua_getglobal(L, key);
    int b = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return b;
}

/* pex_call takes the lua_State as a uint32, which is the whole pointer on
 * the device. Keep this state's blocks in the first 2 GB (Linux MAP_32BIT)
 * so that cast still round-trips here. */
static void *low_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (nsize == 0) {
        if (ptr) { munmap(ptr, osize); }
        return NULL;
    }
    void *p = mmap(NULL, nsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) { return NULL; }
    if (ptr) {
        memcpy(p, ptr, osize < nsize ? osize : nsize);
        munmap(ptr, osize);
    }
    return p;
}

static lua_State *new_state(void) {
    lua_State *L = lua_newstate(low_alloc, NULL);
    if (!L) { return NULL; }
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    static const pexlua_host_t host = { host_find, host_load, NULL };
    pexlua_open(L, &host);
    return L;
}

static void close_state(lua_State *L) {
    pexlua_close();
    if (L) { lua_close(L); }
}

static const char *PING =
    "ping_ok, ping_err = pcall(function() return mod.ping() end)\n";

static void expect_dead(lua_State *L, char *log, size_t n) {
    capture_begin();
    CHECK(lua_ok(L, PING));
    CHECK(lua_ok(L, PING));
    capture_end(log, n);
    CHECK(global_bool(L, "ping_ok") == 0);
    char err[160];
    copy_global(L, "ping_err", err, sizeof err);
    CHECK(strstr(err, "was unloaded") != NULL);
    CHECK(count_sub(log, "Lua used hello after it was unloaded") == 1);
}

static void test_handler_then_use_after(void) {
    reset_mod();
    lua_State *L = new_state();
    CHECK(L != NULL);
    CHECK(lua_ok(L,
        "mod = require('hello')\n"
        "noted = nil\n"
        "during = nil\n"
        "function pex.on_unload(name)\n"
        "  noted = name\n"
        "  during = mod.ping()\n"
        "end\n"));
    CHECK(g_mod.refs == 1);
    char log[512];
    capture_begin();
    CHECK(pexlua_before_unload(&g_mod) == 1);
    capture_end(log, sizeof log);
    CHECK(strstr(log, "no pex.on_unload") == NULL);
    char noted[32];
    copy_global(L, "noted", noted, sizeof noted);
    CHECK(strcmp(noted, "hello") == 0);
    lua_Integer during = 0;
    CHECK(global_int(L, "during", &during) && during == 7);
    CHECK(g_mod.refs == 0);
    CHECK(pex_unload(&g_mod) == PEX_OK);
    CHECK(g_list == NULL);
    expect_dead(L, log, sizeof log);
    close_state(L);
}

static void test_no_handler(void) {
    reset_mod();
    lua_State *L = new_state();
    CHECK(lua_ok(L, "mod = require('hello')\n"));
    char log[512];
    capture_begin();
    CHECK(pexlua_before_unload(&g_mod) == 1);
    capture_end(log, sizeof log);
    CHECK(strstr(log, "no pex.on_unload handler for hello") != NULL);
    CHECK(g_mod.refs == 0);
    CHECK(pex_unload(&g_mod) == PEX_OK);
    expect_dead(L, log, sizeof log);
    close_state(L);
}

static void test_handler_error_still_detaches(void) {
    reset_mod();
    lua_State *L = new_state();
    CHECK(lua_ok(L,
        "mod = require('hello')\n"
        "function pex.on_unload(name) error('boom ' .. name) end\n"));
    char log[512];
    capture_begin();
    CHECK(pexlua_before_unload(&g_mod) == 1);
    capture_end(log, sizeof log);
    if (!strstr(log, "boom hello")) { fprintf(stderr, "log: %s\n", log); }
    CHECK(strstr(log, "boom hello") != NULL);
    CHECK(g_mod.refs == 0);
    CHECK(g_list == &g_mod);
    CHECK(pex_unload(&g_mod) == PEX_OK);
    expect_dead(L, log, sizeof log);
    close_state(L);
}

static void test_busy_stays_attached(int which) {
    reset_mod();
    lua_State *L = new_state();
    CHECK(lua_ok(L,
        "mod = require('hello')\n"
        "noted = nil\n"
        "function pex.on_unload(name) noted = name end\n"));
    if (which == 0) { g_mod.tasks = 1; }
    else { g_mod.active = 1; }
    char log[512];
    capture_begin();
    CHECK(pexlua_before_unload(&g_mod) == 0);
    capture_end(log, sizeof log);
    CHECK(log[0] == 0);
    CHECK(g_mod.refs == 1);
    char noted[32];
    copy_global(L, "noted", noted, sizeof noted);
    CHECK(noted[0] == 0);
    CHECK(lua_ok(L, PING));
    CHECK(global_bool(L, "ping_ok") == 1);
    lua_Integer v = 0;
    CHECK(global_int(L, "ping_err", &v) && v == 7);
    CHECK(pex_unload(&g_mod) == PEX_EBUSY);
    CHECK(g_list == &g_mod);
    g_mod.tasks = 0;
    g_mod.active = 0;
    close_state(L);
}

static void test_extra_ref_blocks_unload_but_lua_is_dead(void) {
    reset_mod();
    lua_State *L = new_state();
    CHECK(lua_ok(L, "mod = require('hello')\n"));
    pex_ref(&g_mod);
    CHECK(g_mod.refs == 2);
    char log[512];
    capture_begin();
    CHECK(pexlua_before_unload(&g_mod) == 1);
    capture_end(log, sizeof log);
    CHECK(strstr(log, "1 reference remains") != NULL);
    CHECK(g_mod.refs == 1);
    CHECK(pex_unload(&g_mod) == PEX_EBUSY);
    CHECK(g_list == &g_mod);
    expect_dead(L, log, sizeof log);
    close_state(L);
}

static void test_notify_without_an_anchor(void) {
    reset_mod();
    lua_State *L = new_state();
    CHECK(lua_ok(L, "noted = nil\nfunction pex.on_unload(name) noted = name end\n"));
    char log[512];
    capture_begin();
    CHECK(pexlua_before_unload(&g_mod) == 0);
    capture_end(log, sizeof log);
    CHECK(strstr(log, "no pex.on_unload") == NULL);
    char noted[32];
    copy_global(L, "noted", noted, sizeof noted);
    CHECK(strcmp(noted, "hello") == 0);
    CHECK(g_mod.refs == 0);
    close_state(L);
}

static void test_silent_when_lua_never_held_it(void) {
    reset_mod();
    lua_State *L = new_state();
    char log[512];
    capture_begin();
    CHECK(pexlua_before_unload(&g_mod) == 0);
    capture_end(log, sizeof log);
    CHECK(log[0] == 0);
    CHECK(pex_unload(&g_mod) == PEX_OK);
    close_state(L);
}

static void test_nested_unload_does_not_recurse(void) {
    reset_mod();
    lua_State *L = new_state();
    CHECK(lua_ok(L,
        "mod = require('hello')\n"
        "nest_ok = nil\n"
        "function pex.on_unload(name)\n"
        "  nest_ok = pex.unload(name)\n"
        "end\n"
        "top_ok, top_err = pex.unload('hello')\n"));
    CHECK(global_bool(L, "top_ok") == 1);
    CHECK(global_bool(L, "nest_ok") == 1);
    CHECK(g_list == NULL);
    char log[512];
    expect_dead(L, log, sizeof log);
    close_state(L);
}

static void test_no_state(void) {
    reset_mod();
    pexlua_close();
    CHECK(pexlua_before_unload(&g_mod) == 0);
    CHECK(g_mod.refs == 0);
    CHECK(pex_unload(&g_mod) == PEX_OK);
}

int main(void) {
    test_handler_then_use_after();
    test_no_handler();
    test_handler_error_still_detaches();
    test_busy_stays_attached(0);
    test_busy_stays_attached(1);
    test_extra_ref_blocks_unload_but_lua_is_dead();
    test_notify_without_an_anchor();
    test_silent_when_lua_never_held_it();
    test_nested_unload_does_not_recurse();
    test_no_state();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
