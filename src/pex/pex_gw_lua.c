/* The gateway's side of pexlua (pex/lua): where modules are and how they load.
 * Hook: lfile_open() calls pex_gw_lua_open(L) for every new Lua state. */
#include "pex/pex_gw.h"
#include "pexlua.h"

static int find(const char *name, char *path, size_t cap) { return pex_gw_find(name, path, cap); }
static pex_mod_t *load(const char *path, const char *as, int *err) { return pex_gw_load_path(path, as, err); }

void pex_gw_lua_open(lua_State *L) {
    static const pexlua_host_t host = { find, load };
    pexlua_open(L, &host);
}
