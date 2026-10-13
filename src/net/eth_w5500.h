#ifndef BTGW_ETH_W5500_H
#define BTGW_ETH_W5500_H

/*
 * Ethernet over a W5500: lwIP netif "e0", an optional second uplink.
 *
 * Optional the way WiFi is: with CONFIG_NET_ETH_W5500 off none of this is
 * compiled, and with it on but no module fitted the chip probe fails, the
 * interface reports "not present", and nothing else is touched.
 *
 * Addressing comes from cfg/eth.json, written by eth.config:
 *
 *   { "mode": "dhcp" }                                    (the default)
 *   { "mode": "manual", "ip": "192.168.50.2", "mask": "255.255.255.0",
 *     "gw": "192.168.50.1", "dns": "1.1.1.1" }            (gw, dns optional)
 *
 * It never calls netif_set_default(): which interface carries the default
 * route is src/net/uplink.c's decision alone.
 */
#include <stdbool.h>
#include <stddef.h>
#include "autoconf.h"

struct netif;

#if CONFIG_NET_ETH_W5500

/* After cyw43_arch_init() (so the radio has its DMA channels) and after
 * mdns_start(). False = no chip; the rest of the firmware carries on. */
bool eth_init(void);

/* Main loop: SPI completions, the chip state machine, config, persistence. */
void eth_poll(void);

bool eth_present(void);
/* The netif, or NULL when there is no chip. */
struct netif *eth_netif(void);

size_t eth_status_json(char *dst, size_t cap);

/* Validate and stage a config ({"mode":..,"ip":..,...}); applied and saved
 * from eth_poll(), not from the request. False with *why on a bad field. */
bool eth_configure(const char *json, const char **why);

#else

static inline bool eth_init(void) { return false; }
static inline void eth_poll(void) {}
static inline bool eth_present(void) { return false; }
static inline struct netif *eth_netif(void) { return NULL; }
static inline size_t eth_status_json(char *d, size_t c) { (void)d; (void)c; return 0; }

#endif
#endif
