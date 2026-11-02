#ifndef BTGW_PEER_FWD_H
#define BTGW_PEER_FWD_H

#include <stdbool.h>

/* One-hop forwarder for a browser websocket on this board. A leading `node`
 * that names another board is stripped and sent on one outgoing websocket
 * to that mDNS name. Replies and events gain `node` on the way back. A
 * frame with no leading node, an empty node, or this board's own hostname
 * is not taken. Returns true when this call owns the text. */
bool peer_fwd_handle(const char *text, void *conn);

/* Abort a dial that is still down after 300 ms. */
void peer_fwd_poll(void);

#endif
