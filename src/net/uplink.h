#ifndef BTGW_UPLINK_H
#define BTGW_UPLINK_H

/*
 * Uplink health and selection: which interface carries the WireGuard tunnel
 * and NEW outbound connections. The ONLY caller of netif_set_default() in
 * the firmware (a tier-0 guard enforces it).
 *
 * Candidates are the WiFi station ("w0") and the W5500 ("e0") -- any of them
 * that has a gateway; one without is not checked and not chosen. Each is
 * pinged (ICMP echo, outside the tunnel, from that interface's own address)
 * every 2 s at the WireGuard endpoint, or at cfg/uplink.json's probe_ip for a
 * server that drops ICMP. RTT, jitter and loss feed src/net/uplink_core.c,
 * which decides; this file measures and applies.
 *
 * Policy (cfg/uplink.json, Settings -> Uplinks):
 *   "best"        (default) switch when another uplink is 30% better for
 *                 20 s, or at once when the active one fails
 *   "failover"    stay until the active one fails
 *   "fixed:<if>"  that interface whenever it has a gateway
 *
 * A switch is make-before-break: the tunnel's outer UDP moves to the new
 * interface and sends a keepalive at once (the server roams on it), while
 * the old interface stays up and keeps receiving. Existing non-tunnel
 * connections stay on the interface that owns their source address
 * (uplink_route_src, the lwIP source-routing hook) for as long as it is up.
 */
#include <stdbool.h>
#include <stddef.h>
#include "autoconf.h"

struct netif;
struct ip4_addr;

#if CONFIG_NET_UPLINK

void uplink_init(void);
void uplink_tick(void);

/* An interface changed state: re-evaluate now (and re-assert the default
 * route, which the cyw43 driver moves whenever it brings an interface up). */
void uplink_reselect(void);

/* Is any uplink usable (an interface with a gateway)? WireGuard waits for it. */
bool uplink_any_up(void);
struct netif *uplink_active(void);

size_t uplink_status_json(char *dst, size_t cap);
/* "best" | "failover" | "fixed:<iface>"; probe_ip "" = the WireGuard endpoint.
 * NULL leaves a field unchanged. */
bool uplink_configure(const char *policy, const char *probe_ip, const char **why);
/* Switch to <iface> now (the experiment's lever); the policy resumes after. */
bool uplink_force(const char *iface, const char **why);

/* One uplink, for Lua's net.uplinks(). rtt/jitter/metric -1 = not measured. */
typedef struct {
    const char *name, *kind, *state;
    bool active;
    char ip[16], gw[16];
    long rtt_ms, jitter_ms, metric;
    int loss_pct;
} uplink_info_t;
int  uplink_count(void);
bool uplink_info(int i, uplink_info_t *out);
const char *uplink_policy_name(void);

/* LWIP_HOOK_IP4_ROUTE_SRC (include/lwipopts.h). */
struct netif *uplink_route_src(const struct ip4_addr *src, const struct ip4_addr *dest);

#else

static inline void uplink_init(void) {}
static inline void uplink_tick(void) {}
static inline void uplink_reselect(void) {}
static inline bool uplink_any_up(void) { return false; }
static inline struct netif *uplink_active(void) { return NULL; }

#endif
#endif
