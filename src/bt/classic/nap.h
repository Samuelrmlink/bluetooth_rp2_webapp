#ifndef BTGW_CLASSIC_NAP_H
#define BTGW_CLASSIC_NAP_H

#include "api/api.h"

struct netif;

/* BNEP, and a PANU that joins a phone NAP. The netif is "b0". */
void nap_init(void);
struct netif *nap_uplink_netif(void);

void op_classic_nap(const char *params, api_reply_fn reply, void *ctx, long id);

#endif
