#ifndef BTGW_PING_H
#define BTGW_PING_H

/*
 * One ICMP echo at a time, for an address on the interface that owns it.
 * Link-local works: the uplink probe skips any interface without a gateway,
 * and this does not. The echo id is outside the uplink probe's range, so
 * the two raw pcbs do not eat each other's replies.
 *
 * Ops and Lua run inside the cooperative loop and cannot wait. ping_start
 * sends and returns. ping_tick, from the main loop, records a timeout.
 * A reply or a timeout is also emitted as "net.ping".
 */
#include <stdbool.h>
#include <stddef.h>
#include "api/api.h"

bool ping_start(const char *ip, const char **why);
void ping_op(const char *params, api_reply_fn reply, void *ctx, long id);
/* Fills dst with the current result. Safe to call with no ping in flight. */
size_t ping_status_json(char *dst, size_t cap);
void ping_tick(void);

#endif
