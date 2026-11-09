#ifndef BTGW_PEX_SCAN_SCHED_H
#define BTGW_PEX_SCAN_SCHED_H
/* The scan scheduler (pex_scan_sched.c). The pex_bt_scan* and pex_bt_quiet*
 * calls themselves are declared in pex_api.h. */
#include <stddef.h>
#include "pex_rt.h"

void pex_scan_sched_apply(void);               /* every poll, and inside each call */
void pex_scan_sched_unload(pex_mod_t *m);      /* a module unloads: its request and slot go */
int pex_gw_bt_sched_json(char *buf, size_t cap);
int pex_gw_bt_quiet_revoke(void);
#endif
