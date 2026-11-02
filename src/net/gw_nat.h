#ifndef BTGW_GW_NAT_H
#define BTGW_GW_NAT_H

/*
 * Masquerade from the gateway LAN out the uplink lwIP currently has as the
 * default route. Not IP forwarding, and not the Lua proxy. The hook eats a
 * packet only when it rewrites it. Replies are written back to the client.
 * Traffic is never sent back out the gateway LAN, its link-local companion,
 * or the WireGuard netif.
 */
#include "autoconf.h"

#if CONFIG_NET_NAT

struct pbuf;
struct netif;

int gw_nat_ip4_input(struct pbuf *p, struct netif *inp);

#endif

#endif
