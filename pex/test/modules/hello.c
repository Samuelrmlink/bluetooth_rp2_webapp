/* hello: the smallest useful module. INIT logs; as a Lua module (PEX_LUA_OPEN)
 * it returns { greet = fn, add = fn, answer = 42 }. The Lua bridge is imported
 * WEAK, so the same file loads on a host without Lua (pex_avail() says so). */
#include "pex.h"

PEX_WEAK_IMPORT(pexlua_optstr);
PEX_WEAK_IMPORT(pexlua_checkint);
PEX_WEAK_IMPORT(pexlua_pushstr);
PEX_WEAK_IMPORT(pexlua_pushint);
PEX_WEAK_IMPORT(pexlua_newtable);
PEX_WEAK_IMPORT(pexlua_setfield);
PEX_WEAK_IMPORT(pexlua_register);

static int l_greet(lua_State *L) {
    size_t n;
    const char *who = pexlua_optstr(L, 1, "world", &n);
    char buf[64];
    pex_snprintf(buf, sizeof buf, "hello, %s", who);
    pexlua_pushstr(L, buf);
    return 1;
}

static int l_add(lua_State *L) {
    pexlua_pushint(L, pexlua_checkint(L, 1) + pexlua_checkint(L, 2));
    return 1;
}

/* Function pointers in a table: RW in model B (PEX_PTR_TABLE), see pex.h. */
static PEX_PTR_TABLE pexlua_reg_t fns[] = { { "greet", l_greet }, { "add", l_add } };

int hello_answer(void) { return 42; }
PEX_EXPORT(hello_answer);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    switch (reason) {
    case PEX_INIT:
        pex_log(PEX_LOG_INFO, "hello from a .pex on %s (host api %u.%u)", host->host_name,
                (unsigned)host->api_major, (unsigned)host->api_minor);
        return PEX_OK;
    case PEX_LUA_OPEN: {
        if (!pex_avail(pexlua_newtable)) { return PEX_ENOTSUP; }
        lua_State *L = arg;
        pexlua_newtable(L, 0, 3);
        pexlua_register(L, fns, 2);
        pexlua_pushint(L, hello_answer());
        pexlua_setfield(L, -2, "answer");
        return 1;
    }
    case PEX_DEINIT:
        pex_log(PEX_LOG_INFO, "goodbye");
        return PEX_OK;
    }
    return PEX_OK;
}
