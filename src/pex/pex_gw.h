#ifndef BTGW_PEX_GW_H
#define BTGW_PEX_GW_H
/*
 * The PEX runtime in the gateway (docs/pex-format.md, "Gateway integration").
 *
 * Everything here is new code; the hooks into existing files are one line
 * each and listed in the spec (section 15) so they merge cleanly:
 *   src/api/ops.c        &op_module_pex in modules[]        (pex.* ops; init)
 *   src/api/api.c        pex_gw_dispatch() before lbind_dispatch (native ops)
 *   src/main.c           pex_gw_poll() in the main loop
 *   src/util/crumb.c     crumb_fault_hook() from the HardFault handler
 *   src/bt/le/central.c  le_pex_event() where values/links are reported
 *   src/lua/lfile.c      pex_gw_lua_open() at the end of lfile_open (Lua builds)
 *   CMakeLists.txt, Kconfig
 *
 * Safety for a board reachable only remotely:
 *   - nothing PEX runs at boot unless cfg/pex_autostart.json lists it, and the
 *     pool (RAM) is only taken when a module is first loaded;
 *   - a module that faults is attributed (name, build id, offset), the board
 *     reboots into the same firmware through the normal fault path, and the
 *     next boot records the build id in var/pex_faults.json: autostart skips
 *     it until pex.faults_clear. A hang (watchdog) inside module code is
 *     caught the same way through the trace word the trampoline keeps.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "api/api.h"
#include "pex_rt.h"

bool pex_gw_ensure(void);           /* runtime up, pool taken (false: no heap room) */
void pex_gw_poll(void);             /* main loop, once per pass */
bool pex_gw_dispatch(const char *name, const char *params, api_reply_fn reply, void *ctx, long id);
void crumb_fault_hook(const uint32_t *frame);

/* "pex/x.m33.pex", "lfs:/pex/x.m33.pex" */
pex_mod_t *pex_gw_load_path(const char *path, const char *as, int *err);
/* lfs:/pex/<name>.<arch>.pex if it exists */
int pex_gw_find(const char *name, char *path, size_t cap);
const char *pex_gw_arch(void);

/* groups (pex_gw_*.c) */
extern const pex_group_t pex_group_sys, pex_group_board, pex_group_bus, pex_group_fs, pex_group_net,
    pex_group_bt;
void pex_gw_bus_unload(pex_mod_t *m);
size_t pex_gw_bus_ops_json(char *dst, size_t cap);
void pex_gw_fs_unload(pex_mod_t *m);
void pex_gw_fs_poll(void);
void pex_gw_net_unload(pex_mod_t *m);
void pex_gw_net_poll(void);
void pex_gw_bt_unload(pex_mod_t *m);
void pex_gw_bt_poll(void);

#endif
