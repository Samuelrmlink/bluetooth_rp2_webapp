#ifndef BTGW_MDNS_H
#define BTGW_MDNS_H

/*
 * mDNS responder: a ".local" hostname plus advertised services.
 *
 * Which record types come from where, because it is not obvious and it is
 * the whole of what "supports A, PTR, SRV and TXT" means here:
 *
 *   A     the hostname -> the netif's address. One per bound interface, so
 *         the device answers on USB-NCM and on WiFi with the right address
 *         for each rather than with whichever came up first.
 *   PTR   "_http._tcp.local" -> "<instance>._http._tcp.local". This is what
 *         a browser like avahi-browse enumerates.
 *   SRV   the instance -> host, port, priority, weight.
 *   TXT   key=value pairs attached to the instance.
 *
 * lwIP emits all four; what this adds is that the hostname, the services and
 * their TXT items are all runtime state rather than compile-time constants,
 * settable from Lua. Device-specific naming does not belong in C -- the same
 * rule the tier-0 guard enforces for protocol knowledge.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "autoconf.h"

struct netif;

#define BTGW_MDNS_HOSTNAME CONFIG_NET_HOSTNAME

/* lwIP's MDNS_MAX_SERVICES is the per-netif cap; this is ours, and must not
 * exceed it (include/lwipopts.h). */
#define MDNS_SERVICES_MAX 4
#define MDNS_TXT_MAX      6
#define MDNS_TXT_LEN      63   /* a TXT item is length-prefixed by one byte */

#if CONFIG_NET_MDNS

void mdns_start(void);
void mdns_add_netif(struct netif *netif);
void mdns_remove_netif(struct netif *netif);

/* Lowercases, and refuses anything that is not a legal DNS label. Renames
 * every bound interface and every service instance that was tracking the
 * hostname. */
bool mdns_set_hostname(const char *name);
const char *mdns_hostname(void);

/*
 * Advertise a service. `instance` NULL or empty means "track the hostname",
 * which is what you want for the device's own web UI. `type` is the bare
 * protocol label with its underscore, e.g. "_http". Replacing an existing
 * (type, proto) pair updates it in place rather than duplicating it.
 *
 * `txt` is n_txt strings, each a whole "key=value" item.
 */
bool mdns_service_add(const char *instance, const char *type, bool udp,
                      uint16_t port, const char *const *txt, int n_txt);
bool mdns_service_remove(const char *type, bool udp);
void mdns_services_clear(void);

/* Re-announce on every bound interface. Needed after a change that lwIP does
 * not announce for itself. */
void mdns_announce_all(void);

size_t mdns_status_json(char *dst, size_t cap);

#else

static inline void mdns_start(void) {}
static inline void mdns_add_netif(struct netif *n) { (void)n; }
static inline void mdns_remove_netif(struct netif *n) { (void)n; }
static inline bool mdns_set_hostname(const char *name) { (void)name; return false; }
static inline const char *mdns_hostname(void) { return BTGW_MDNS_HOSTNAME; }
static inline bool mdns_service_add(const char *i, const char *t, bool u, uint16_t p,
                                    const char *const *x, int n) {
    (void)i; (void)t; (void)u; (void)p; (void)x; (void)n; return false;
}
static inline bool mdns_service_remove(const char *t, bool u) { (void)t; (void)u; return false; }
static inline void mdns_services_clear(void) {}
static inline void mdns_announce_all(void) {}
static inline size_t mdns_status_json(char *dst, size_t cap) {
    (void)dst; (void)cap; return 0;
}

#endif
#endif
