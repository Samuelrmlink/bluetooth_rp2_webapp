/*
 * Host harness: the REAL Lua, the REAL C bindings, no board.
 *
 * This is the dividend of putting the protocol in Lua. The profile's frame
 * construction, its TLV specs and its telemetry decode are pure functions of
 * bytes, so they can be tested on a laptop in milliseconds -- no flash cycle,
 * no radio, no charger. What is NOT testable here is anything that talks:
 * bt.* is absent on purpose, so a test that needs a device fails to resolve
 * rather than quietly pretending.
 *
 *   ./luaproto <script.lua> [args...]
 */
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

void lbind_proto_open(lua_State *L);

/* sys.ms() is the only thing the profile needs from the runtime that the
 * harness has to stand in for. Monotonic milliseconds is all it promises. */
#include <time.h>
static int l_sys_ms(lua_State *L) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    lua_pushinteger(L, (lua_Integer)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
    return 1;
}
static int l_sys_sleep(lua_State *L) { (void)L; return 0; }

static int l_log(lua_State *L) {
    fprintf(stderr, "    log: %s\n", luaL_optstring(L, 1, ""));
    return 0;
}

static int l_bus_emit(lua_State *L) {
    /* Tests inspect emissions by overriding bus.emit themselves; the default
     * just has to exist and not crash. */
    (void)L;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <script.lua>\n", argv[0]);
        return 2;
    }
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lbind_proto_open(L);

    static const luaL_Reg sys[] = {
        { "ms", l_sys_ms }, { "sleep", l_sys_sleep }, { NULL, NULL } };
    luaL_newlib(L, sys);
    lua_setglobal(L, "sys");

    static const luaL_Reg lg[] = {
        { "info", l_log }, { "warn", l_log }, { "error", l_log }, { NULL, NULL } };
    luaL_newlib(L, lg);
    lua_setglobal(L, "log");

    static const luaL_Reg bus[] = { { "emit", l_bus_emit }, { NULL, NULL } };
    luaL_newlib(L, bus);
    lua_setglobal(L, "bus");

    static const luaL_Reg ops[] = { { "register", l_bus_emit }, { NULL, NULL } };
    luaL_newlib(L, ops);
    lua_setglobal(L, "ops");

    if (luaL_dofile(L, argv[1]) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    int rc = (int)lua_tointegerx(L, -1, NULL);
    lua_close(L);
    return rc;
}
