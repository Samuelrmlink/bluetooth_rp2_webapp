#ifndef BTGW_BT_PERIPHERAL_H
#define BTGW_BT_PERIPHERAL_H

#include "api/api.h"

/* Advertise beside the central. The controller must already be up before
 * advertisements actually start; this only registers for that moment. */
void bt_peripheral_init(const char *name);

/* GAP plus the MCUmgr SMP service, then commit. Replaces the local list. */
void op_bt_smp(const char *params, api_reply_fn reply, void *ctx, long id);

#endif
