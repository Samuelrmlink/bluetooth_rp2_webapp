#ifndef BTGW_GW_LAN_H
#define BTGW_GW_LAN_H

/*
 * Gateway LAN runtime. cfg/gw.json picks the carrier (e0, another iface, or
 * none). The link-local companion on e0 is eth_w5500.c's; this file decides
 * when that companion is held, announces the other uplinks, and remembers
 * what it hears. Websocket ops stage work here and gw_lan_poll applies it
 * from the main loop.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "autoconf.h"

#if CONFIG_NET_GW_LAN

void gw_lan_init(void);
void gw_lan_poll(void);

/* True when cfg says the carrier is e0, so Ethernet keeps link-local for
 * the life of the link beside any DHCP lease. */
bool gw_lan_hold_ll(void);
/* The configured carrier. "none" advertises nothing and NATs nothing. */
const char *gw_lan_iface(void);
bool gw_is_lan_iface(const char *name);

/* Best heard connection, or false when the table is empty. */
bool gw_best_peer(char *name, size_t name_cap, char *iface, size_t iface_cap,
                  int32_t *metric, char *ll, size_t ll_cap);

/* A name written by gw.hostname or Lua mdns.hostname. Staged; the file is
 * written from the main loop. The serial formula never comes through here. */
void gw_hostname_written(const char *name);

const char *gw_active_name(void);
/* "file" or "serial". */
const char *gw_name_source(void);
size_t gw_peers_json(char *dst, size_t cap);

bool gw_hostname_stage(const char *name, const char **why);
bool gw_forward_stage(const char *proto, long listen_port, const char *to,
                      long dest_port, const char **why);
bool gw_unforward_stage(const char *proto, long listen_port, const char **why);

#else

static inline void gw_lan_init(void) {}
static inline void gw_lan_poll(void) {}
static inline bool gw_lan_hold_ll(void) { return false; }
static inline const char *gw_lan_iface(void) { return "none"; }
static inline bool gw_is_lan_iface(const char *name) { (void)name; return false; }
static inline bool gw_best_peer(char *a, size_t b, char *c, size_t d,
                                int32_t *e, char *f, size_t g) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    return false;
}
static inline void gw_hostname_written(const char *name) { (void)name; }

#endif

#endif
