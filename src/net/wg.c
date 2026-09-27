#include "net/wg.h"

#if CONFIG_NET_WIREGUARD

#include <stdio.h>
#include <string.h>

#include "pico/rand.h"
#include "pico/time.h"

#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"

#include "wireguardif.h"
#include "wireguard.h"

#include "api/api.h"
#include "api/ops.h"   /* api_json_str / api_json_int */
#include "fs/fs.h"

/* ------------------------------------------------------------------ *
 * The four hooks wireguard-platform.h asks the platform to provide.
 * ------------------------------------------------------------------ */

uint32_t wireguard_sys_now(void) { return sys_now(); }

/*
 * Session keys come from here, so this must be a real entropy source and not
 * a PRNG seeded from the clock. get_rand_64() is the SDK's interface to the
 * hardware entropy on this part; using anything weaker would make the tunnel
 * look fine and be worthless.
 */
void wireguard_random_bytes(void *bytes, size_t size) {
    uint8_t *p = (uint8_t *)bytes;
    while (size) {
        uint64_t r = get_rand_64();
        size_t n = size < sizeof r ? size : sizeof r;
        memcpy(p, &r, n);
        p += n;
        size -= n;
    }
}

/*
 * TAI64N, used by the peer for handshake replay detection. It does not have
 * to be the real time -- it has to never go backwards for a given peer. We
 * have no RTC, so this is boot time plus an epoch: monotonic within a boot,
 * which is what replay detection needs. Across a REBOOT it restarts, and the
 * peer will reject initiations until our counter passes the old value; that
 * is a known limitation of a clockless device and is why the peer's
 * handshake may take a moment after a power cycle.
 */
void wireguard_tai64n_now(uint8_t *output) {
    /* 2^62 + seconds since 1970, per the TAI64 label format. */
    uint64_t millis = (uint64_t)to_ms_since_boot(get_absolute_time());
    uint64_t seconds = 0x400000000000000aULL + (millis / 1000u);
    uint32_t nanos = (uint32_t)((millis % 1000u) * 1000000u);
    for (int i = 0; i < 8; i++) { output[i] = (uint8_t)(seconds >> (56 - 8 * i)); }
    for (int i = 0; i < 4; i++) { output[8 + i] = (uint8_t)(nanos >> (24 - 8 * i)); }
}

/* We never generate cookie replies: this device is a client with one peer
 * and is not exposed to the initiation flood that mechanism defends. */
bool wireguard_is_under_load(void) { return false; }

/* ------------------------------------------------------------------ *
 * Config
 * ------------------------------------------------------------------ */

#define WG_CFG_PATH "cfg/wireguard.json"
#define WG_KEY_B64  48       /* 32 bytes base64 is 44 + nul */

typedef struct {
    bool     present;
    char     private_key[WG_KEY_B64];
    char     peer_public_key[WG_KEY_B64];
    uint8_t  psk[32];
    bool     have_psk;
    ip_addr_t address, netmask;
    ip_addr_t allowed_ip, allowed_mask;
    ip_addr_t endpoint;
    uint16_t endpoint_port;
    uint16_t listen_port;
    uint16_t keepalive;
} wg_cfg_t;

static wg_cfg_t cfg;
static struct netif wg_netif;
static bool netif_added;
static uint8_t peer_index = WIREGUARDIF_INVALID_INDEX;

/* Counters. The library tracks handshake state but not volume, so the netif's
 * input and output are wrapped rather than patching vendored code.
 *
 * These count TUNNELLED PAYLOAD -- IP packets routed into and out of the
 * tunnel. Handshake and keepalive traffic goes out over UDP inside
 * wireguardif and never passes through netif->output, so it is deliberately
 * not counted here; connect_attempts and handshakes are what to watch while
 * a tunnel is still coming up. */
static uint64_t rx_bytes, tx_bytes;
static uint32_t connect_attempts, handshakes;
static uint32_t last_handshake_ms;
static bool was_connected;
static netif_output_fn inner_output;

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') { return c - 'A'; }
    if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
    if (c >= '0' && c <= '9') { return c - '0' + 52; }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return -1;
}

/* Only ever used for the preshared key: wireguardif takes the other two as
 * base64 strings and decodes them itself. */
static bool b64_decode32(const char *in, uint8_t out[32]) {
    uint32_t acc = 0;
    int bits = 0, n = 0;
    for (const char *p = in; *p && *p != '='; p++) {
        int v = b64_val(*p);
        if (v < 0) { return false; }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= 32) { return false; }
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n == 32;
}

static bool read_cfg_file(char *buf, size_t cap, size_t *len) {
    size_t size = 0;
    int h = fs_begin_read(WG_CFG_PATH, &size);
    if (h < 0) { return false; }
    int n = fs_read(h, (uint8_t *)buf, cap - 1);
    fs_end_read(h);
    if (n <= 0) { return false; }
    buf[n] = 0;
    *len = (size_t)n;
    return true;
}

static bool ip_field(const char *json, const char *key, ip_addr_t *out, const char *dflt) {
    char s[48];
    if (!api_json_str(json, key, s, sizeof s)) {
        if (!dflt) { return false; }
        snprintf(s, sizeof s, "%s", dflt);
    }
    return ipaddr_aton(s, out) != 0;
}

static bool load_cfg(void) {
    memset(&cfg, 0, sizeof cfg);
    static char buf[768];
    size_t len = 0;
    if (!read_cfg_file(buf, sizeof buf, &len)) { return false; }

    if (!api_json_str(buf, "private_key", cfg.private_key, sizeof cfg.private_key)) {
        printf("wg: %s has no private_key\n", WG_CFG_PATH);
        return false;
    }
    if (!api_json_str(buf, "peer_public_key", cfg.peer_public_key,
                      sizeof cfg.peer_public_key)) {
        printf("wg: %s has no peer_public_key\n", WG_CFG_PATH);
        return false;
    }
    char psk_b64[WG_KEY_B64];
    if (api_json_str(buf, "preshared_key", psk_b64, sizeof psk_b64) && psk_b64[0]) {
        if (!b64_decode32(psk_b64, cfg.psk)) {
            printf("wg: preshared_key is not 32 base64 bytes\n");
            return false;
        }
        cfg.have_psk = true;
    }

    if (!ip_field(buf, "address", &cfg.address, NULL)) {
        printf("wg: %s needs an \"address\" inside the tunnel\n", WG_CFG_PATH);
        return false;
    }
    ip_field(buf, "netmask", &cfg.netmask, "255.255.255.0");
    ip_field(buf, "allowed_ip", &cfg.allowed_ip, "0.0.0.0");
    ip_field(buf, "allowed_mask", &cfg.allowed_mask, "0.0.0.0");

    /* An IP, not a name. Resolving one would mean a DNS lookup before the
     * tunnel exists, on whichever interface happens to be up, and failing
     * that would leave the tunnel half-configured. A name belongs in a later
     * revision with an explicit resolver step, not hidden in here. */
    if (!ip_field(buf, "endpoint", &cfg.endpoint, NULL)) {
        printf("wg: \"endpoint\" must be an IP address\n");
        return false;
    }

    long v = 0;
    cfg.endpoint_port = api_json_int(buf, "endpoint_port", &v) ? (uint16_t)v
                                                               : WIREGUARDIF_DEFAULT_PORT;
    cfg.listen_port = api_json_int(buf, "listen_port", &v) ? (uint16_t)v
                                                           : WIREGUARDIF_DEFAULT_PORT;
    cfg.keepalive = api_json_int(buf, "keepalive", &v) ? (uint16_t)v : 25;
    cfg.present = true;
    return true;
}

/* ------------------------------------------------------------------ *
 * Counting wrappers
 * ------------------------------------------------------------------ */

static err_t wg_count_input(struct pbuf *p, struct netif *inp) {
    if (p) { rx_bytes += p->tot_len; }
    return ip_input(p, inp);
}

static err_t wg_count_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr) {
    if (p) { tx_bytes += p->tot_len; }
    return inner_output ? inner_output(netif, p, ipaddr) : ERR_IF;
}

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

/*
 * The netif is created ONCE and kept for the life of the boot; up/down
 * manage the PEER.
 *
 * Not a preference -- wireguard-lwip has no shutdown entry point.
 * wireguardif_init() mallocs a device, binds a UDP pcb on the listen port
 * and starts a repeating sys_timeout, and netif_remove() frees none of
 * that. Tearing the netif down and building it again therefore leaks the
 * device and leaves the port bound, so the second wireguardif_init() fails
 * on udp_bind and the tunnel never comes back: observed as "netif_add
 * failed" on the first wg.up after a wg.down.
 *
 * Changing the tunnel's own address needs a reboot. Changing the peer does
 * not.
 */
void wg_down(void) {
    if (!netif_added || peer_index == WIREGUARDIF_INVALID_INDEX) { return; }
    wireguardif_disconnect(&wg_netif, peer_index);
    wireguardif_remove_peer(&wg_netif, peer_index);
    peer_index = WIREGUARDIF_INVALID_INDEX;
    netif_set_down(&wg_netif);
    was_connected = false;
    printf("wg: peer disconnected (the netif stays; see wg.c)\n");
}

/* Add the configured peer to an existing netif and start connecting. */
static bool attach_peer(void) {
    struct wireguardif_peer peer;
    wireguardif_peer_init(&peer);
    peer.public_key = cfg.peer_public_key;
    peer.preshared_key = cfg.have_psk ? cfg.psk : NULL;
    peer.allowed_ip = cfg.allowed_ip;
    peer.allowed_mask = cfg.allowed_mask;
    peer.endpoint_ip = cfg.endpoint;
    peer.endport_port = cfg.endpoint_port;
    peer.keep_alive = cfg.keepalive;

    if (wireguardif_add_peer(&wg_netif, &peer, &peer_index) != ERR_OK ||
        peer_index == WIREGUARDIF_INVALID_INDEX) {
        printf("wg: add_peer failed\n");
        peer_index = WIREGUARDIF_INVALID_INDEX;
        return false;
    }
    netif_set_up(&wg_netif);
    if (!ip_addr_isany(&cfg.endpoint)) {
        wireguardif_connect(&wg_netif, peer_index);
        connect_attempts++;
    }
    return true;
}

bool wg_up(void) {
    wg_down();
    if (!load_cfg()) { return false; }

    if (!netif_added) {
        static struct wireguardif_init_data init;
        init.private_key = cfg.private_key;
        init.listen_port = cfg.listen_port;
        init.bind_netif = NULL;          /* listen on whatever is up */

        if (!netif_add(&wg_netif, ip_2_ip4(&cfg.address), ip_2_ip4(&cfg.netmask),
                       ip_2_ip4(&cfg.address), &init, &wireguardif_init,
                       &wg_count_input)) {
            printf("wg: netif_add failed\n");
            return false;
        }
        netif_added = true;

        /* Count what leaves, by wrapping what wireguardif_init installed. */
        inner_output = wg_netif.output;
        wg_netif.output = wg_count_output;

        /*
         * DELIBERATELY NOT netif_set_default(&wg_netif).
         *
         * This is the one line that would turn an extra path into the only
         * path. With the tunnel as the default route, anything that broke it
         * -- a bad key, a peer that moved, a dead upstream -- would take the
         * device off the network entirely, including the USB link used to
         * fix it. The tunnel is reachable at its own address and routed to
         * by its own netmask; nothing else changes. tools/run_tests.py
         * asserts this file contains no such call.
         */
    }

    if (!attach_peer()) { return false; }

    {
        char a[IP4ADDR_STRLEN_MAX], e[IP4ADDR_STRLEN_MAX], m[IP4ADDR_STRLEN_MAX];
        ip4addr_ntoa_r(ip_2_ip4(&cfg.address), a, sizeof a);
        ip4addr_ntoa_r(ip_2_ip4(&cfg.netmask), m, sizeof m);
        ip4addr_ntoa_r(ip_2_ip4(&cfg.endpoint), e, sizeof e);
        printf("wg: tunnel up, local %s/%s, peer %s:%u, keepalive %us "
               "(NOT the default route)\n",
               a, m, e, (unsigned)cfg.endpoint_port, (unsigned)cfg.keepalive);
    }
    return true;
}

bool wg_init(void) {
    rx_bytes = tx_bytes = 0;
    connect_attempts = handshakes = 0;
    peer_index = WIREGUARDIF_INVALID_INDEX;
    if (!fs_stat(WG_CFG_PATH, NULL)) {
        printf("wg: no %s, tunnel not configured\n", WG_CFG_PATH);
        return false;
    }
    return wg_up();
}

bool wg_is_configured(void) { return cfg.present && netif_added; }

bool wg_is_connected(void) {
    if (!netif_added || peer_index == WIREGUARDIF_INVALID_INDEX) { return false; }
    return wireguardif_peer_is_up(&wg_netif, peer_index, NULL, NULL) == ERR_OK;
}

#define WG_RETRY_MS 15000

void wg_tick(void) {
    if (!netif_added || peer_index == WIREGUARDIF_INVALID_INDEX) { return; }

    bool up = wg_is_connected();
    if (up && !was_connected) {
        handshakes++;
        last_handshake_ms = to_ms_since_boot(get_absolute_time());
        printf("wg: handshake complete with %s:%u\n",
               ipaddr_ntoa(&cfg.endpoint), (unsigned)cfg.endpoint_port);
        api_emit("wg.state", "{\"connected\":true}");
    } else if (!up && was_connected) {
        printf("wg: peer went away\n");
        api_emit("wg.state", "{\"connected\":false}");
    }
    was_connected = up;

    /* Nudge a peer that has never come up. wireguardif retries its own
     * handshake, but a connect that was refused outright needs re-issuing,
     * and doing it on a timer costs nothing when the tunnel is healthy. */
    static uint32_t last_try;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!up && (uint32_t)(now - last_try) > WG_RETRY_MS) {
        last_try = now;
        wireguardif_connect(&wg_netif, peer_index);
        connect_attempts++;
    }
}

size_t wg_status_json(char *dst, size_t cap) {
    ip_addr_t cur_ip;
    u16_t cur_port = 0;
    bool up = false;
    if (netif_added && peer_index != WIREGUARDIF_INVALID_INDEX) {
        up = wireguardif_peer_is_up(&wg_netif, peer_index, &cur_ip, &cur_port) == ERR_OK;
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());

    /* ipaddr_ntoa returns a pointer to ONE static buffer, so three of them in
     * a single snprintf all render the last conversion. The status reported
     * the endpoint as the local address until this was split out -- a
     * plausible-looking wrong answer, which is the kind that survives. */
    char s_addr[IP4ADDR_STRLEN_MAX], s_ep[IP4ADDR_STRLEN_MAX], s_cur[IP4ADDR_STRLEN_MAX];
    ip4addr_ntoa_r(ip_2_ip4(&cfg.address), s_addr, sizeof s_addr);
    ip4addr_ntoa_r(ip_2_ip4(&cfg.endpoint), s_ep, sizeof s_ep);
    s_cur[0] = 0;
    if (up) { ip4addr_ntoa_r(ip_2_ip4(&cur_ip), s_cur, sizeof s_cur); }

    /*
     * Peer state, read from the device struct the netif carries.
     *
     * Worth the reach into the library's own types: "did we actually send a
     * handshake initiation" is the first question when a tunnel does not
     * come up, and without it the only evidence is a packet capture on the
     * far end -- which is exactly what you cannot get when the tunnel is
     * the thing that is broken. last_initiation_tx answers it directly.
     */
    bool peer_active = false, hs_valid = false, keypair_valid = false;
    uint32_t last_init_tx = 0, last_rx = 0;
    if (netif_added && wg_netif.state) {
        const struct wireguard_device *dev =
            (const struct wireguard_device *)wg_netif.state;
        const struct wireguard_peer *pr = &dev->peers[0];
        if (pr->valid) {
            peer_active    = pr->active;
            hs_valid       = pr->handshake.valid;
            keypair_valid  = pr->curr_keypair.valid;
            last_init_tx   = pr->last_initiation_tx;
            last_rx        = pr->last_rx;
        }
    }

    return (size_t)snprintf(dst, cap,
        "{\"configured\":%s,\"netif_up\":%s,\"connected\":%s,"
        "\"address\":\"%s\",\"endpoint\":\"%s\",\"endpoint_port\":%u,"
        "\"current_endpoint\":\"%s\",\"current_port\":%u,"
        "\"handshakes\":%lu,\"connect_attempts\":%lu,"
        "\"last_handshake_age_ms\":%lu,"
        "\"rx_bytes\":%llu,\"tx_bytes\":%llu,"
        "\"peer_active\":%s,\"handshake_valid\":%s,\"keypair_valid\":%s,"
        "\"initiation_sent_ms_ago\":%ld,\"last_rx_ms_ago\":%ld,"
        "\"is_default_route\":%s}",
        cfg.present ? "true" : "false",
        (netif_added && netif_is_up(&wg_netif)) ? "true" : "false",
        up ? "true" : "false",
        cfg.present ? s_addr : "",
        cfg.present ? s_ep : "",
        (unsigned)cfg.endpoint_port,
        s_cur,
        (unsigned)cur_port,
        (unsigned long)handshakes, (unsigned long)connect_attempts,
        (unsigned long)(handshakes ? now - last_handshake_ms : 0),
        (unsigned long long)rx_bytes, (unsigned long long)tx_bytes,
        peer_active ? "true" : "false",
        hs_valid ? "true" : "false",
        keypair_valid ? "true" : "false",
        last_init_tx ? (long)(now - last_init_tx) : -1L,
        last_rx ? (long)(now - last_rx) : -1L,
        /* Reported so a monitor can assert it, not just trust the comment
         * in wg_up(). If this is ever true, something regressed badly. */
        (netif_added && netif_default == &wg_netif) ? "true" : "false");
}

#endif /* CONFIG_NET_WIREGUARD */
