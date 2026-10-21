/*
 * pexlua -- the OPTIONAL Lua layer over the PEX runtime (spec 7.3 option C, 9).
 * Compiled only into firmware that has Lua; the runtime never needs it.
 *
 * Two things, in priority order (native apps first; Lua is a launcher/glue):
 *
 *  1. the launcher API, a global table `pex`:
 *       pex.load(name [, path])        -> info | nil, err     (loads; PEX_INIT runs)
 *       pex.start(name [, cfg])        -> true | nil, err     (cfg: table -> JSON, or a JSON string)
 *       pex.stop(name)                 -> true | nil, err
 *       pex.unload(name)               -> true | nil, err
 *       pex.list()                     -> { info, ... }
 *       pex.info(name)                 -> info | nil, err
 *       pex.call(name, export, ...)    -> integer             (up to 4 integer arguments)
 *     A module loaded this way belongs to the firmware, not to the Lua VM: an
 *     app keeps running when the script that started it ends.
 *
 *  2. `require 'x'` finding x.<arch>.pex (before the Lua source, spec 9) and
 *     the `lua` API group (pexlua_* in host_api.json) so a module can build
 *     the table it returns. Functions registered with pexlua_register() run
 *     the module through the trampoline under lua_pcall, so a Lua error raised
 *     inside module code unwinds safely. A module loaded by require belongs to
 *     the VM: it is unloaded when the last of its functions is collected
 *     (pex.unload(name) drops require's cache entry first).
 */
#ifndef PEXLUA_H
#define PEXLUA_H

#include <stddef.h>
#include "lua.h"
#include "pex_rt.h"

typedef struct {
    /* Locate a module: fill path for <name> (search order is the host's);
     * return 1 if found. */
    int (*find)(const char *name, char *path, size_t cap);
    /* Load from a path the host understands (lfs:/..., sd:/...). */
    pex_mod_t *(*load)(const char *path, const char *as_name, int *err);
} pexlua_host_t;

extern const pex_group_t pex_group_lua;

/* Registers the `lua` group with the runtime (once), installs the `pex`
 * table and wraps the global `require`. Call after the state's own globals
 * (incl. require) exist; call again for a new state after a VM restart. */
void pexlua_open(lua_State *L, const pexlua_host_t *host);

#endif
