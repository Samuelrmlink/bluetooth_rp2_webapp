#ifndef BTGW_MDNS_H
#define BTGW_MDNS_H

#include <stdbool.h>
#include "autoconf.h"

struct netif;

#define BTGW_MDNS_HOSTNAME CONFIG_NET_HOSTNAME

#if CONFIG_NET_MDNS

void mdns_start(void);
void mdns_add_netif(struct netif *netif);
void mdns_remove_netif(struct netif *netif);
bool mdns_set_hostname(const char *name);
const char *mdns_hostname(void);

#else

/* Gated out: no-ops rather than #if at every call site. usb_ncm.c and wifi.c
 * register their netifs as they come up and should not have to care whether
 * anyone is listening -- a minimal build that drops mDNS must still link. */
static inline void mdns_start(void) {}
static inline void mdns_add_netif(struct netif *n) { (void)n; }
static inline void mdns_remove_netif(struct netif *n) { (void)n; }
static inline bool mdns_set_hostname(const char *name) { (void)name; return false; }
static inline const char *mdns_hostname(void) { return BTGW_MDNS_HOSTNAME; }

#endif
#endif
