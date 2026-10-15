#ifndef BTGW_ETH_CFG_H
#define BTGW_ETH_CFG_H

/*
 * Ethernet (e0) addressing configuration: parse, validate, serialise, and the
 * link-local timing decision -- pure C, no lwIP, so test/host/eth_cfg_test.c
 * checks it directly.
 *
 * Modes:
 *   dhcp    (the default) DHCP client; with no server answering, AutoIP
 *           link-local (169.254/16) after ETH_LL_AFTER_MS while DHCP keeps
 *           retrying; a lease replaces it, a lost lease falls back to it.
 *   manual  ip/mask (+ optional gw, dns).
 *   server  e0 takes a static address (default 192.168.9.1/24) and serves
 *           DHCP on it. NEVER entered automatically: the ONLY way in is
 *           eth_cfg_parse() of an explicit {"mode":"server"} -- from the
 *           eth.config op or a valid cfg/eth.json. A missing, corrupt or
 *           unknown config is the DHCP client (eth_cfg_load). A tier-0 guard
 *           checks that nothing else ever sets ETH_MODE_SERVER.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { ETH_MODE_DHCP = 0, ETH_MODE_MANUAL, ETH_MODE_SERVER } eth_mode_t;

typedef struct {
    eth_mode_t mode;
    uint8_t    ip[4], mask[4], gw[4], dns[4];   /* 0.0.0.0 = unset */
    bool       dhcp_router;   /* server: advertise e0 as router + DNS (off by default) */
} eth_cfg_t;

#define ETH_SERVER_DEFAULT_IP   { 192, 168, 9, 1 }
#define ETH_SERVER_DEFAULT_MASK { 255, 255, 255, 0 }

/* How long DHCP gets alone before link-local is added beside it. */
#define ETH_LL_AFTER_MS 8000
/* While on link-local, restart DHCP this often. lwIP's own discover backoff
 * grows to one attempt a minute; restarting keeps "a DHCP server appeared"
 * to within about half a minute. A few broadcasts per period, on e0 only. */
#define ETH_DHCP_KICK_MS 30000

void eth_cfg_default(eth_cfg_t *c);                       /* DHCP client */
/* Strict: an explicit config (eth.config, or the file). False + *why, and
 * *out untouched, on anything wrong. */
bool eth_cfg_parse(const char *json, eth_cfg_t *out, const char **why);
/* The file at boot: NULL, empty, corrupt or unknown -> the DHCP client. */
void eth_cfg_load(const char *json, eth_cfg_t *out);
size_t eth_cfg_json(const eth_cfg_t *c, char *dst, size_t cap);
const char *eth_mode_name(eth_mode_t m);

/* Should link-local be started now? DHCP mode, link up, no lease, not
 * already link-local, and ETH_LL_AFTER_MS since the link came up or the lease
 * was lost (`since_ms`). */
bool eth_ll_due(eth_mode_t mode, bool link, bool have_lease, bool ll_active,
                uint32_t since_ms, uint32_t now_ms);

/* While link-local without a lease: time to restart DHCP? */
bool eth_dhcp_kick_due(eth_mode_t mode, bool ll_active, bool have_lease,
                       uint32_t last_kick_ms, uint32_t now_ms);

#endif
