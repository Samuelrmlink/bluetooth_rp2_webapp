#ifndef C5_MDNS_ADV_H
#define C5_MDNS_ADV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void mdns_adv_start(void);
void mdns_adv_poll(void);
const char *mdns_adv_hostname(void);

/* IPv4 and _btgw._tcp port for a hostname. False when it is unknown or quiet.
 * A missing SRV port is reported as 80. */
bool mdns_adv_peer(const char *name, char *ip, size_t ip_cap, uint16_t *port);

/* One PTR query for _btgw._tcp.local on each interface that has an address. */
void mdns_adv_browse_kick(void);

#endif
