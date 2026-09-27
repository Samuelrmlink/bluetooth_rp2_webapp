#ifndef BTGW_WG_H
#define BTGW_WG_H

/*
 * WireGuard: an ADDITIONAL way to reach this device. Never the only one.
 *
 * That constraint is the whole design brief and it is enforced here rather
 * than left to discipline: the tunnel netif is never made lwIP's default
 * route, so USB-NCM and WiFi keep working exactly as they did whether the
 * tunnel is up, down, misconfigured or absent. A remote-access path that can
 * strand the device when it misbehaves is worse than no remote access, and
 * this firmware has already had one of those -- the SoftAP used to take the
 * USB link down with it.
 *
 * Config lives in cfg/wireguard.json on LittleFS, which is git-ignored and
 * which the HTTP filesystem routes refuse to serve. Keys never enter the
 * source tree, same rule as the Anker credentials.
 *
 *   {
 *     "private_key":     "<base64, 32 bytes>",
 *     "address":         "10.9.0.2",       // ours, inside the tunnel
 *     "netmask":         "255.255.255.0",
 *     "peer_public_key": "<base64, 32 bytes>",
 *     "preshared_key":   "<base64, optional>",
 *     "endpoint":        "203.0.113.7",    // an IP: see wg.c on DNS
 *     "endpoint_port":   51820,
 *     "allowed_ip":      "0.0.0.0",
 *     "allowed_mask":    "0.0.0.0",
 *     "keepalive":       25,
 *     "listen_port":     51820
 *   }
 *
 * The implementation is third_party/wireguard-lwip (BSD-3, Daniel Hope),
 * vendored straight from smartalock/wireguard-lwip -- which is the same code
 * MicroLink uses. What is here is the platform layer it asks for, the config,
 * the lifecycle and the counters.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "autoconf.h"

#if CONFIG_NET_WIREGUARD

/* Reads the config and brings the tunnel up if one is present. Safe to call
 * with no config file: it reports "not configured" and changes nothing. */
bool wg_init(void);

/* From the main loop. Retries a peer that has not completed a handshake, and
 * keeps the counters honest. Cheap when the tunnel is down or absent. */
void wg_tick(void);

bool wg_up(void);        /* (re)read config and connect */
void wg_down(void);      /* disconnect; the other routes are untouched */

bool wg_is_configured(void);
bool wg_is_connected(void);   /* a handshake has completed with the peer */

/* Everything the websocket monitors: configured, link state, peer endpoint,
 * time since the last handshake, bytes in and out, and the failure count. */
size_t wg_status_json(char *dst, size_t cap);

#else

static inline bool wg_init(void) { return false; }
static inline void wg_tick(void) {}
static inline bool wg_up(void) { return false; }
static inline void wg_down(void) {}
static inline bool wg_is_configured(void) { return false; }
static inline bool wg_is_connected(void) { return false; }
static inline size_t wg_status_json(char *dst, size_t cap) { (void)dst; (void)cap; return 0; }

#endif
#endif
