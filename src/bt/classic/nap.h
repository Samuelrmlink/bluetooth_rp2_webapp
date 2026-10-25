#ifndef BTGW_CLASSIC_NAP_H
#define BTGW_CLASSIC_NAP_H

#include "api/api.h"

struct netif;

/* BNEP. The board can dial a phone NAP, or accept a call from the phone.
 * The netif is "b0". nap_tick flushes cfg/nap.json outside the radio poll. */
void nap_init(void);
void nap_tick(void);
struct netif *nap_uplink_netif(void);

void op_classic_nap(const char *params, api_reply_fn reply, void *ctx, long id);

#endif
