#ifndef BTGW_GW_CORE_H
#define BTGW_GW_CORE_H

/*
 * Gateway-LAN decisions with no lwIP and no autoconf, so the host test can
 * link this file alone. The runtime in gw_lan.c applies what these return.
 *
 * A missing cfg/gw.json is "e0" when the W5500 is in the image and "none"
 * otherwise. A stored "none" stays none. Link-local stays beside a DHCP
 * lease only while the feature is on. An uplink is advertised only when it
 * has a gateway and it is not the gateway LAN itself. Silence of 6 s drops
 * that one connection. NAT never goes back out the gateway LAN, the
 * link-local companion, or the WireGuard netif.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_AD_MS     2000u
#define GW_DROP_MS   6000u
#define GW_PEERS     8
#define GW_CONNS     4
/* "none" plus a NUL. Netif names are two characters. */
#define GW_IFACE_LEN 8
#define GW_NAME_LEN  64
#define GW_LL_LEN    16

typedef struct {
    bool    used;
    char    iface[GW_IFACE_LEN];
    int32_t metric;
    char    ll[GW_LL_LEN];
    uint32_t seen_ms;
} gw_conn_t;

typedef struct {
    bool      used;
    char      name[GW_NAME_LEN];
    gw_conn_t conns[GW_CONNS];
} gw_peer_t;

typedef struct {
    gw_peer_t peers[GW_PEERS];
} gw_table_t;

/* Strict: unknown or missing iface fails and leaves *iface untouched.
 * Known names are none, e0, w0, us, b0. */
bool gw_cfg_parse(const char *json, char *iface, size_t cap, const char **why);
/* Missing, empty, or anything parse rejects becomes the default. */
void gw_cfg_load(const char *json, bool have_w5500, char *iface, size_t cap);

/* DNS label: letters, digits, hyphens, not empty, not starting with '-'. */
bool gw_hostname_ok(const char *name);
/* {"name":"..."} or false. Does not accept the serial formula by omission. */
bool gw_hostname_parse(const char *json, char *name, size_t cap, const char **why);
/* prefix + '-' + lowercase hex of id. RP2- on the Pico, ESP32C5- on the C5. */
void gw_default_hostname(const char *prefix, const uint8_t *id, size_t id_len,
                         char *out, size_t cap);

/* Link-local stays up across a lease only when the gateway LAN is on. */
bool gw_ll_kept(bool gw_on, bool have_lease);

/* Advertise when the connection has a gateway and is not the gateway LAN. */
bool gw_advertise(bool has_gw, bool iface_is_gw_lan);

/* "<host>-<iface>". False when the result would not fit in a DNS label. */
bool gw_instance_name(const char *host, const char *iface, char *out, size_t cap);
/* Split the last hyphen. Iface is the two-character netif name. */
bool gw_split_instance(const char *instance, char *host, size_t hcap,
                       char *iface, size_t icap);
/* TXT items "iface=<iface>" and "metric=<n>". */
void gw_ad_txt(const char *iface, int32_t metric,
               char *iface_item, size_t icap, char *metric_item, size_t mcap);

void gw_table_init(gw_table_t *t);
/* Upsert one connection under name. metric < 0 leaves an existing metric and
 * stores INT32_MAX-1 on a new one. ll NULL or empty leaves any stored address.
 * A connection with no link-local is not a next hop. */
bool gw_peer_note(gw_table_t *t, const char *name, const char *iface,
                  int32_t metric, const char *ll, uint32_t now_ms);
/* Refresh seen_ms on an existing connection. Does not create one. */
bool gw_peer_touch(gw_table_t *t, const char *name, const char *iface,
                   uint32_t now_ms);
/* Drop connections silent for GW_DROP_MS. Other connections of that peer stay.
 * Returns how many connections dropped. */
int gw_peer_expire(gw_table_t *t, uint32_t now_ms);
/* Lowest metric connection that still has a link-local address. */
bool gw_peer_best(const gw_table_t *t, uint32_t now_ms,
                  char *name, size_t name_cap, char *iface, size_t iface_cap,
                  int32_t *metric, char *ll, size_t ll_cap);
/* Link-local of one named peer, from its best live connection. */
bool gw_peer_ll(const gw_table_t *t, const char *name, uint32_t now_ms,
                char *ll, size_t ll_cap);

/* False for an empty out iface, the gateway LAN, the "el" companion, or "wg". */
bool gw_nat_out_ok(const char *out_iface, const char *lan_iface);

#endif
