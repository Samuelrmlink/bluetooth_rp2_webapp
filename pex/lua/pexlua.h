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
 *       pex.install(name | path)       -> info | nil, err     (into the flash partition: XIP)
 *       pex.installed()                -> { info, ... }       (what is installed)
 *       pex.uninstall(name)            -> true | nil, err
 *     pex.load(name) and require prefer an installed copy (the host's find).
 *     A module loaded this way belongs to the firmware, not to the Lua VM: an
 *     app keeps running when the script that started it ends.
 *
 *  2. `require 'x'` finding x.<arch>.pex (before the Lua source, spec 9) and
 *     the `lua` API group (pexlua_* in host_api.json) so a module can build
 *     the table it returns. Functions registered with pexlua_register() run
 *     the module through the trampoline under lua_pcall, so a Lua error raised
 *     inside module code unwinds safely. A module loaded by require belongs to
 *     the VM: it is unloaded when the last of its functions is collected.
 *
 * Manual unload (pex.unload, or the gateway's pex.unload op) notifies Lua
 * first. Assign a global function:
 *
 *     function pex.on_unload(name) ... end
 *
 * It runs while the module is still loaded, so the script can drop its own
 * copies. The anchor is then released and the module unloads even if a
 * closure is still reachable. A later call through that closure raises
 * "PEX module '<name>' was unloaded" and the firmware logs that once.
 * With no handler, and Lua holding the module, the log says so.
 * A module that still has tasks, or that is on a call stack, is left alone;
 * pex_unload then reports why. pex.stop does not unload.
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
    /* Load from a path the host understands (lfs:/..., xip:<name>, ...). */
    pex_mod_t *(*load)(const char *path, const char *as_name, int *err);
    /* Optional (0.7): install a module (a name or a path) into the host's
     * flash partition; the slot or a PEX_E* code. NULL: pex.install refuses. */
    int (*install)(const char *name_or_path);
} pexlua_host_t;

extern const pex_group_t pex_group_lua;

/* Registers the `lua` group with the runtime (once), installs the `pex`
 * table and wraps the global `require`. Call after the state's own globals
 * (incl. require) exist; call again for a new state after a VM restart. */
void pexlua_open(lua_State *L, const pexlua_host_t *host);

/* The state from pexlua_open is closed. Call before lua_close. */
void pexlua_close(void);

/* Notify pex.on_unload(name) and drop this state's anchor so pex_unload can
 * proceed. Returns 1 when an anchor was released, 0 when there was nothing
 * to drop or the module is busy (tasks or active). Safe to call with no Lua
 * state. Does not itself unload. */
int pexlua_before_unload(pex_mod_t *m);

#endif
