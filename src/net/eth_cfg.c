/* Ethernet addressing config. See eth_cfg.h. */
#include "net/eth_cfg.h"

#include <stdio.h>
#include <string.h>

/* Minimal JSON field readers: flat objects of strings and booleans, which is
 * all eth.config and cfg/eth.json carry. */
static const char *field(const char *json, const char *key) {
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json ? strstr(json, pat) : NULL;
    if (!p) { return NULL; }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { p++; }
    if (*p != ':') { return NULL; }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { p++; }
    return p;
}

static bool get_str(const char *json, const char *key, char *out, size_t cap) {
    const char *p = field(json, key);
    if (!p || *p != '"') { return false; }
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        if (n + 1 >= cap) { return false; }
        out[n++] = *p++;
    }
    if (*p != '"') { return false; }
    out[n] = 0;
    return true;
}

static bool get_bool(const char *json, const char *key, bool *out) {
    const char *p = field(json, key);
    if (!p) { return false; }
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool parse_ip4(const char *s, uint8_t out[4]) {
    unsigned v[4];
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &v[0], &v[1], &v[2], &v[3], &tail) != 4) { return false; }
    for (int i = 0; i < 4; i++) {
        if (v[i] > 255) { return false; }
        out[i] = (uint8_t)v[i];
    }
    return true;
}

static bool zero(const uint8_t a[4]) { return !(a[0] | a[1] | a[2] | a[3]); }

static uint32_t u32(const uint8_t a[4]) {
    return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
}

static bool mask_ok(const uint8_t m[4]) {
    uint32_t v = u32(m);
    return v != 0 && ((~v + 1) & ~v) == 0;
}

/* Optional address field: absent/empty -> 0.0.0.0; present -> must parse. */
static bool opt_ip(const char *json, const char *key, uint8_t out[4]) {
    char s[24];
    memset(out, 0, 4);
    if (!get_str(json, key, s, sizeof s) || !s[0]) { return true; }
    return parse_ip4(s, out);
}

void eth_cfg_default(eth_cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->mode = ETH_MODE_DHCP;
}

const char *eth_mode_name(eth_mode_t m) {
    return m == ETH_MODE_MANUAL ? "manual" : m == ETH_MODE_SERVER ? "server" : "dhcp";
}

bool eth_cfg_parse(const char *json, eth_cfg_t *out, const char **why) {
    eth_cfg_t c;
    eth_cfg_default(&c);
    char mode[12] = "dhcp";
    if (field(json, "mode") && !get_str(json, "mode", mode, sizeof mode)) {
        *why = "mode is dhcp, manual or server"; return false;
    }
    if (!opt_ip(json, "ip", c.ip) || !opt_ip(json, "mask", c.mask) ||
        !opt_ip(json, "gw", c.gw) || !opt_ip(json, "dns", c.dns)) {
        *why = "an address field is not a dotted IPv4 address"; return false;
    }
    bool b;
    if (get_bool(json, "dhcp_router", &b)) { c.dhcp_router = b; }

    if (strcmp(mode, "dhcp") == 0) {
        c.mode = ETH_MODE_DHCP;          /* manual fields kept, for the UI */
    } else if (strcmp(mode, "manual") == 0) {
        c.mode = ETH_MODE_MANUAL;
        if (zero(c.ip)) { *why = "manual needs a valid ip"; return false; }
        if (!mask_ok(c.mask)) { *why = "manual needs a valid mask"; return false; }
        if (!zero(c.gw) && (u32(c.gw) & u32(c.mask)) != (u32(c.ip) & u32(c.mask))) {
            *why = "gw is not on the ip/mask subnet"; return false;
        }
    } else if (strcmp(mode, "server") == 0) {
        /* The one place server mode is ever set (see eth_cfg.h). */
        c.mode = ETH_MODE_SERVER;
        static const uint8_t dip[4] = ETH_SERVER_DEFAULT_IP, dmask[4] = ETH_SERVER_DEFAULT_MASK;
        if (zero(c.ip)) { memcpy(c.ip, dip, 4); }
        if (zero(c.mask)) { memcpy(c.mask, dmask, 4); }
        if (!mask_ok(c.mask)) { *why = "server needs a valid mask"; return false; }
        /* The server hands out .16 upwards (dhcpserver.c) and owns the
         * address: a /24 or larger, and not .16-.23 itself. */
        if (u32(c.mask) > 0xFFFFFF00u) { *why = "server needs a /24 or larger subnet"; return false; }
        if (c.ip[3] == 0 || c.ip[3] == 255 || (c.ip[3] >= 16 && c.ip[3] < 24)) {
            *why = "server address must not be .0, .255 or in the .16-.23 lease range"; return false;
        }
        memset(c.gw, 0, 4);              /* e0 is no uplink in server mode */
    } else {
        *why = "mode is dhcp, manual or server"; return false;
    }
    *out = c;
    return true;
}

void eth_cfg_load(const char *json, eth_cfg_t *out) {
    const char *why = NULL;
    eth_cfg_t c;
    if (json && json[0] && eth_cfg_parse(json, &c, &why)) { *out = c; return; }
    eth_cfg_default(out);
}

static void fmt_ip(const uint8_t a[4], char *s, size_t cap) {
    if (zero(a)) { s[0] = 0; return; }
    snprintf(s, cap, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

size_t eth_cfg_json(const eth_cfg_t *c, char *dst, size_t cap) {
    char ip[16], m[16], g[16], d[16];
    fmt_ip(c->ip, ip, sizeof ip); fmt_ip(c->mask, m, sizeof m);
    fmt_ip(c->gw, g, sizeof g); fmt_ip(c->dns, d, sizeof d);
    return (size_t)snprintf(dst, cap,
        "{\"mode\":\"%s\",\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\",\"dhcp_router\":%s}",
        eth_mode_name(c->mode), ip, m, g, d, c->dhcp_router ? "true" : "false");
}

bool eth_ll_due(eth_mode_t mode, bool link, bool have_lease, bool ll_active,
                uint32_t since_ms, uint32_t now_ms) {
    return mode == ETH_MODE_DHCP && link && !have_lease && !ll_active &&
           (uint32_t)(now_ms - since_ms) >= ETH_LL_AFTER_MS;
}
