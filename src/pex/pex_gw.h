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
 *   - nothing PEX runs at boot unless the launch list says so
 *     (cfg/launch.json, or cfg/pex_autostart.json while that file is absent),
 *     and the runtime (pool, flash index) only starts when a module is first used;
 *   - a module that faults is attributed (name, build id, offset), the board
 *     reboots into the same firmware through the normal fault path, and the
 *     next boot records the build id in var/pex_faults.json: autostart skips
 *     it until pex.faults_clear. A callback that runs longer than
 *     CONFIG_PEX_SLICE_MS is stopped in place (that module faulted, the loop
 *     continues) and recorded the same way. A hang with interrupts masked
 *     still resets the board; the trace word names it on the next boot.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "api/api.h"
#include "pex_rt.h"

bool pex_gw_ensure(void);           /* runtime up, pool taken (false: no heap room) */
void pex_gw_poll(void);             /* main loop, once per pass */
/* Note a previous-boot fault into var/pex_faults.json. Once per boot. */
void pex_gw_record_boot_fault(void);
/* Start one app. path NULL: installed copy, else pex/<name>.<arch>.pex.
 * cfg NULL: no JSON. 0 started, 1 already running, 2 skipped, -1 failed. */
int pex_gw_launch_one(const char *name, const char *path_or_null, const char *cfg_or_null);
bool pex_gw_dispatch(const char *name, const char *params, api_reply_fn reply, void *ctx, long id);
void crumb_fault_hook(const uint32_t *frame);

/* "pex/x.m33.pex", "lfs:/pex/x.m33.pex", "xip:<name>" (installed, run in place) */
pex_mod_t *pex_gw_load_path(const char *path, const char *as, int *err);
/* "xip:<name>" when installed, else lfs:/pex/<name>.<arch>.pex if it exists */
int pex_gw_find(const char *name, char *path, size_t cap);
/* lfs file rel: load into RAM (*out) or install (returns the slot) */
int pex_gw_with_file(const char *rel, const char *as, pex_mod_t **out, bool install);

/* XIP (pex_gw_xip.c): the PEX flash partition */
const pex_flash_t *pex_gw_flash(void);         /* NULL when the build has none */
void pex_gw_xip_window(void);                  /* ATRANS1 -> the partition (RP2350) */
uint32_t pex_gw_xip_flash_ops(void);
int pex_gw_install_path(const char *name_or_path);   /* a name, lfs:/... ; slot or PEX_E* */
/* sd:/ : a load reads the file into the pool. An install pushes each read
 * through pex_install_write and does not copy the file. PEX_EPENDING, then
 * the event pex.sd_done. */
int pex_gw_sd_begin(const char *path, const char *as, bool install);
void pex_gw_sd_poll(void);
bool pex_gw_sd_busy(void);
const char *pex_gw_arch(void);

/* groups (pex_gw_*.c) */
extern const pex_group_t pex_group_sys, pex_group_board, pex_group_bus, pex_group_fs, pex_group_net,
    pex_group_bt, pex_group_codec, pex_group_classic;
void pex_gw_bus_unload(pex_mod_t *m);
size_t pex_gw_bus_ops_json(char *dst, size_t cap);
void pex_gw_fs_unload(pex_mod_t *m);
void pex_gw_fs_poll(void);
void pex_gw_net_unload(pex_mod_t *m);
void pex_gw_net_poll(void);
void pex_gw_bt_unload(pex_mod_t *m);
void pex_gw_bt_poll(void);
void pex_gw_classic_unload(pex_mod_t *m);
void pex_gw_classic_poll(void);

#endif
