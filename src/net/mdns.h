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
#include "net/mdns_records.h"

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
/* The same, recording the script slot that added it so the service is
 * withdrawn when that script stops (mdns_release_owner). -1 = nobody's. */
bool mdns_service_add_owned(int owner, const char *instance, const char *type, bool udp,
                            uint16_t port, const char *const *txt, int n_txt);
bool mdns_service_remove(const char *type, bool udp);
void mdns_services_clear(void);

/* Record groups for names other than ours (src/net/mdns_records.h): added
 * and announced, removed with a goodbye. owner as above. */
int    mdns_group_add(int owner, const mrec_t *recs, int n, const char **why);
bool   mdns_group_remove(int id);
size_t mdns_groups_json(char *dst, size_t cap);
/* Withdraw everything a script declared (groups and services); returns how
 * many. Called when it stops. */
int    mdns_release_owner(int owner);
int    mdns_release_all_owned(void);

/* Re-announce on every bound interface. Needed after a change that lwIP does
 * not announce for itself. */
void mdns_announce_all(void);

size_t mdns_status_json(char *dst, size_t cap);

/* _btgw._tcp peers heard on any interface. The table is the querier in
 * mdns_browse.c. age_ms is how long since the last record of that peer. */
void mdns_browse_poll(void);
void mdns_browse_kick(void);
size_t mdns_peers_json(char *dst, size_t cap);
/* ip is a dotted quad. False when the name is unknown or has gone quiet. */
bool mdns_peer_endpoint(const char *name, char *ip, size_t ip_cap, uint16_t *port);

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
static inline bool mdns_service_add_owned(int o, const char *i, const char *t, bool u, uint16_t p,
                                          const char *const *x, int n) {
    (void)o; (void)i; (void)t; (void)u; (void)p; (void)x; (void)n; return false;
}
static inline int mdns_group_add(int o, const mrec_t *r, int n, const char **why) {
    (void)o; (void)r; (void)n; *why = "mDNS is not in this build"; return -1;
}
static inline bool mdns_group_remove(int id) { (void)id; return false; }
static inline size_t mdns_groups_json(char *d, size_t c) { (void)d; (void)c; return 0; }
static inline int mdns_release_owner(int o) { (void)o; return 0; }
static inline int mdns_release_all_owned(void) { return 0; }
static inline void mdns_announce_all(void) {}
static inline size_t mdns_status_json(char *dst, size_t cap) {
    (void)dst; (void)cap; return 0;
}
static inline void mdns_browse_poll(void) {}
static inline void mdns_browse_kick(void) {}
static inline size_t mdns_peers_json(char *dst, size_t cap) {
    (void)dst; (void)cap; return 0;
}
static inline bool mdns_peer_endpoint(const char *name, char *ip, size_t ip_cap, uint16_t *port) {
    (void)name; (void)ip; (void)ip_cap; (void)port; return false;
}

#endif
#endif
