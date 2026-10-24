#ifndef BTGW_CLASSIC_SPP_H
#define BTGW_CLASSIC_SPP_H

#include "api/api.h"

/* Inquiry, SDP, one RFCOMM channel. Called from bt_init when BT_CLASSIC
 * is on. Does not power the controller and does not start an inquiry. */
void classic_init(void);

void op_classic_scan(const char *params, api_reply_fn reply, void *ctx, long id);
void op_classic_connect(const char *params, api_reply_fn reply, void *ctx, long id);
void op_classic_state(const char *params, api_reply_fn reply, void *ctx, long id);

#endif
