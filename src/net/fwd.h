#ifndef BTGW_FWD_H
#define BTGW_FWD_H

/*
 * Forwarding rules: a tiny iptables, declared from Lua.
 *
 *   id = net.forward{ proto='tcp'|'udp', listen=6767, iface=nil|'e0'|'w0'|...,
 *                     to='127.0.0.1', port=80, fail_fast=true }
 *   net.unforward(id)      net.forwards() -> { {id=.., ...}, ... }
 *
 * NOT IP forwarding and no NAT: the gateway terminates the client's
 * connection (or datagram) and re-originates it to the target, so replies
 * come back through the same pair whatever interfaces are involved -- WiFi
 * to Ethernet, a tunnel to a local port, a port to itself. TCP is spliced
 * with backpressure (no buffers of its own); UDP keeps a session per client
 * (60 s idle) and relays replies back from the listening port.
 *
 * Rules belong to the script that made them and are removed when it stops.
 * Limits: FWD_RULES rules, FWD_TCP_CONNS spliced connections and
 * FWD_UDP_SESSIONS UDP sessions in all. See fwd_core.h for "fail fast".
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "autoconf.h"

#define FWD_RULES 8

typedef struct {
    int         id;
    bool        udp;
    uint16_t    listen;
    char        iface[4];          /* "" = every interface */
    char        to[16];
    uint16_t    port;
    bool        fail_fast;
    const char *state;             /* "unknown" | "up" | "down" */
    char        why[24];
    uint32_t    active, total, refused, errors;
    uint32_t    bytes_in, bytes_out;   /* client -> target, target -> client */
    int         owner;
} fwd_info_t;

#if CONFIG_NET_FORWARD

void fwd_init(void);
void fwd_tick(void);

/* Returns the rule id (> 0) or -1 with *why. owner: a script slot, -1 none. */
int  fwd_add(int owner, bool udp, uint16_t listen, const char *iface, const char *to,
             uint16_t port, bool fail_fast, const char **why);
bool fwd_remove(int id);
int  fwd_release_owner(int owner);
int  fwd_release_all_owned(void);

int  fwd_count(void);
bool fwd_info(int i, fwd_info_t *out);     /* i-th live rule */
size_t fwd_status_json(char *dst, size_t cap);

#else

static inline void fwd_init(void) {}
static inline void fwd_tick(void) {}
static inline int fwd_release_owner(int o) { (void)o; return 0; }
static inline int fwd_release_all_owned(void) { return 0; }

#endif
#endif
