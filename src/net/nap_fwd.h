#ifndef BTGW_NAP_FWD_H
#define BTGW_NAP_FWD_H

/*
 * Masquerade for the phone on b0. Off unless Phone network's Share
 * internet switch is on. Packets from 192.168.44.0/24 that are not for
 * that subnet leave through the named interface (w0, e0, the WireGuard
 * tunnel wg, …) with that interface's address, and replies are written
 * back to the phone. wg encrypts to the configured peer and is not made
 * the default route.
 *
 * This is not src/net/fwd.c. That one terminates a connection and opens
 * another. This one rewrites addresses on the packet.
 *
 * The board's own default route is untouched: outbound packets are handed
 * to the selected netif, not to lwIP's router.
 */
#include <stdint.h>
#include "lwip/ip4_addr.h"

struct pbuf;
struct netif;

void nap_fwd_set(int on, const char *iface, struct netif *lan);
int nap_fwd_on(void);
const char *nap_fwd_iface(void);
/* "" when off or ready, otherwise "down" or "no gateway". */
const char *nap_fwd_why(void);
void nap_fwd_counts(uint32_t *tx, uint32_t *rx, uint32_t *drop);

/* True when sharing is on. *out is the resolver to put in the DHCP offer:
 * the board's own upstream DNS, or 1.1.1.1 when that is empty or, for the
 * tunnel, a private address the peer cannot reach. */
int nap_fwd_resolver(ip4_addr_t *out);

/* 1: the frame was from the phone and is not for 192.168.44.0/24. The
 * caller frees p. 0: the caller delivers p to b0 as usual. */
int nap_fwd_out(struct pbuf *p);

/* lwIP IP input hook. 1: this was a reply to a masqueraded flow and the
 * hook freed p. */
int nap_fwd_ip4_input(struct pbuf *p, struct netif *inp);

void nap_fwd_tick(void);

#endif
