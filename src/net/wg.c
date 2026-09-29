#include "net/wg.h"

#if CONFIG_NET_WIREGUARD

#include <stdio.h>
#include <string.h>

#include "pico/rand.h"
#include "pico/time.h"

#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"

#include "autoconf.h"
#if CONFIG_NET_WIFI
#include "net/wifi.h"   /* wifi_is_up(): only reach for the tunnel once WiFi has an IP */
#endif

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
 * A boot counter, persisted, turned into a TAI64N offset.
 *
 * See wireguard_tai64n_now() for why the timestamp must never go backwards
 * for a given peer, and why a clockless device otherwise restarts it low on
 * every boot -- locking itself out of the tunnel for as long as its PREVIOUS
 * uptime, which is exactly what an OTA reboot of a long-running gateway
 * triggers. The fix: each configured boot reads a counter from flash,
 * increments it, and offsets the epoch by count << SHIFT, so a fresh boot
 * always starts strictly above where any earlier boot could have reached.
 *
 * SHIFT = 26 gives each boot a 2^26 s (~2.1 year) window before its
 * timestamps could overrun the next boot's start -- longer than any
 * plausible continuous uptime -- and 2^(62-26) = ~68 billion boots before
 * the seconds field is exhausted. A single boot that ran past 2.1 years, or
 * a wiped filesystem resetting the counter, would cost one rejection window;
 * both are self-healing and not worth more code.
 *
 * The counter is a 4-byte file, which LittleFS stores INLINE in a directory's
 * metadata (cache_size 256 >= 4): no data block, so nothing to fragment, and
 * each bump is a small metadata-log COMMIT (a page program), with a full
 * sector erase only when the log compacts -- ~once per 50-100 boots, then
 * wear-levelled by block_cycles. It lives in its OWN directory (var/), not
 * cfg/, so that churn does not compact the config directory's metadata; and
 * on the LittleFS partition, which is shared by both OTA slots, so it
 * survives an update.
 */
#define WG_EPOCH_PATH  "var/wg_epoch.bin"
#define WG_EPOCH_SHIFT 26

static uint64_t tai64n_offset_s;
static uint32_t wg_boot_count;   /* exposed in wg.status so the epoch can be checked without an SWD probe */

static void wg_bump_epoch(void) {
    uint32_t count = 0;
    int h = fs_begin_read(WG_EPOCH_PATH, NULL);
    if (h >= 0) {
        uint8_t b[4];
        if (fs_read(h, b, 4) == 4) {
            count = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        }
        fs_end_read(h);
    }
    count++;
    uint8_t b[4] = { (uint8_t)count, (uint8_t)(count >> 8), (uint8_t)(count >> 16), (uint8_t)(count >> 24) };
    /* fs_write() returns the BYTE COUNT, not 0 (see fs.h). Testing it for 0
     * meant the success branch never ran: the counter was never committed AND
     * fs_end_write() was never called, so fs.c's `wopen` stayed set and every
     * later fs_begin_write() in the firmware returned -1. That is why
     * boot_count sat at 1 for ever, which in turn made the TAI64N timestamp
     * restart low on every boot and left WireGuard's anti-replay silently
     * dropping our handshakes. */
    bool persisted = false;
    if (fs_begin_write(WG_EPOCH_PATH) == 0) {
        if (fs_write(b, 4) < 0) {
            fs_abort_write();
        } else {
            persisted = (fs_end_write() == 0);
        }
    }
    if (!persisted) {
        /* A write failure only means the NEXT boot may reuse this count and
         * be rejected for one window; the tunnel this boot still works. */
        printf("wg: could not persist boot counter (tai64n offset still applied)\n");
    }
    wg_boot_count = count;
    tai64n_offset_s = (uint64_t)count << WG_EPOCH_SHIFT;
    printf("wg: boot #%lu, tai64n epoch offset %llu s\n",
           (unsigned long)count, (unsigned long long)tai64n_offset_s);
}

/*
 * TAI64N, used by the peer for handshake replay detection. It does not have
 * to be the real time -- it has to never go backwards for a given peer. We
 * have no RTC, so this is boot time plus an epoch. Within a boot it is
 * monotonic, which is what replay detection needs; ACROSS a reboot the
 * persisted boot counter (wg_bump_epoch) lifts the epoch so it stays
 * monotonic there too, instead of restarting low and locking us out for the
 * length of the previous uptime.
 */
void wireguard_tai64n_now(uint8_t *output) {
    /* 2^62 + seconds since 1970, per the TAI64 label format. */
    uint64_t millis = (uint64_t)to_ms_since_boot(get_absolute_time());
    uint64_t seconds = 0x400000000000000aULL + tai64n_offset_s + (millis / 1000u);
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
    ip_addr_t probe_ip;      /* liveness probe target inside the tunnel; ANY = disabled */
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
static bool was_confirmed;
static netif_output_fn inner_output;

/* The liveness probe -- see wg_is_confirmed() for why it has to exist. */
static struct udp_pcb *probe_pcb;
static uint32_t last_probe_ms;

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

    /* Where the liveness probe goes: by default the first host of our own
     * tunnel subnet, which on a conventional server is the server itself. It
     * must be inside allowed_ip or wireguardif_output() drops it silently. If
     * that works out to our own address the probe is pointless; disable it. */
    {
        ip4_addr_t d;
        ip4_addr_set_u32(&d, (ip4_addr_get_u32(ip_2_ip4(&cfg.address)) &
                              ip4_addr_get_u32(ip_2_ip4(&cfg.netmask))) | PP_HTONL(1u));
        char dflt[IP4ADDR_STRLEN_MAX];
        ip4addr_ntoa_r(&d, dflt, sizeof dflt);
        ip_field(buf, "probe_ip", &cfg.probe_ip, dflt);
        if (ip_addr_cmp(&cfg.probe_ip, &cfg.address)) {
            printf("wg: probe_ip equals our own address; liveness probe disabled\n");
            ip_addr_set_any(false, &cfg.probe_ip);
        }
    }
    if (cfg.keepalive == 0) {
        printf("wg: keepalive 0 -- nothing periodic proves the server is there, so "
               "the tunnel is never CONFIRMED and auto mode keeps its AP\n");
    }
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
    was_confirmed = false;
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
    /* Handshake initiation deferred to wg_tick(), gated on WiFi being up with
     * an IP (task #37): WireGuard must not contend for the shared radio while
     * WiFi is still associating. */
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

        /* One pcb for the liveness probe, for the life of the boot, like the
         * netif. Bound to ANY:0; udp_sendto_if() pins the egress netif, so
         * the probe never depends on lwIP's default route. */
        probe_pcb = udp_new();
        if (probe_pcb) { udp_bind(probe_pcb, IP_ADDR_ANY, 0); }

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
    wg_bump_epoch();
    return wg_up();
}

bool wg_is_configured(void) { return cfg.present && netif_added; }

bool wg_is_connected(void) {
    if (!netif_added || peer_index == WIREGUARDIF_INVALID_INDEX) { return false; }
    return wireguardif_peer_is_up(&wg_netif, peer_index, NULL, NULL) == ERR_OK;
}

/* The one peer, reached through the device struct the netif carries. */
static const struct wireguard_peer *peer0(void) {
    if (!netif_added || !wg_netif.state) { return NULL; }
    const struct wireguard_device *dev = (const struct wireguard_device *)wg_netif.state;
    const struct wireguard_peer *pr = &dev->peers[0];
    return pr->valid ? pr : NULL;
}

/*
 * CONFIRMED: the server has proven itself recently.
 *
 * wg_is_connected() is not that. It is true whenever a keypair exists, and a
 * keypair lives 180 s (540 s counting prev_keypair) after the last handshake
 * with no traffic at all. Confirmation needs evidence the far end is alive
 * and holds our keys:
 *
 *   - peer->last_rx, which the library sets ONLY after a transport message
 *     authenticates and decrypts (wireguardif.c, process_data_message) --
 *     including zero-length keepalives from the server; or
 *   - curr_keypair.keypair_millis, set by wireguard_start_session() once the
 *     server's static key has authenticated a handshake response. Counting
 *     it means a fresh boot confirms at the handshake instead of waiting for
 *     the first data reply.
 *
 * Whichever is newer must be inside the window.
 *
 * THE WINDOW, and why there is a probe. A standard server does NOT answer a
 * keepalive with a keepalive: Linux and wireguard-go take the "length 0"
 * branch before timers_data_received(), the only thing that arms the 10 s
 * passive keepalive. A client that only ever sends keepalives therefore
 * receives nothing between handshakes, and last_rx would stall. So when the
 * tunnel has been silent for one keepalive interval we send one byte of DATA
 * through it (send_probe); the server answers at once with a tunnelled ICMP
 * unreachable or, if that is firewalled, with its passive keepalive within
 * 10 s. Either advances last_rx. The window is therefore
 * keepalive + 10 s + 3 s margin: tighter than that reads false for part of
 * every cycle on a healthy tunnel, and each false edge is an AP transition.
 *
 * keepalive == 0 disables the persistent keepalive, so nothing periodic can
 * prove anything; the honest answer is "never confirmed", not a window that
 * tracks the user's own traffic.
 */
#define WG_PASSIVE_KEEPALIVE_S 10
#define WG_CONFIRM_MARGIN_S     3

bool wg_is_confirmed(void) {
    if (!wg_is_configured() || peer_index == WIREGUARDIF_INVALID_INDEX) { return false; }
    if (cfg.keepalive == 0) { return false; }
    const struct wireguard_peer *pr = peer0();
    if (!pr) { return false; }
    uint32_t last = pr->last_rx;
    if (pr->curr_keypair.valid && (int32_t)(pr->curr_keypair.keypair_millis - last) > 0) {
        last = pr->curr_keypair.keypair_millis;
    }
    if (!last) { return false; }
    uint32_t window = ((uint32_t)cfg.keepalive + WG_PASSIVE_KEEPALIVE_S + WG_CONFIRM_MARGIN_S) * 1000u;
    return (uint32_t)(sys_now() - last) < window;   /* sys_now() is the library's clock too */
}

static void send_probe(void) {
    if (!probe_pcb || ip_addr_isany(&cfg.probe_ip)) { return; }
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 1, PBUF_RAM);
    if (!p) { return; }
    ((uint8_t *)p->payload)[0] = 0;
    /* Port 9 (discard). A closed port is fine -- the ICMP that comes back is
     * itself tunnelled data, which is the whole point. */
    udp_sendto_if(probe_pcb, p, &cfg.probe_ip, 9, &wg_netif);
    pbuf_free(p);
}

static void emit_state(bool connected, bool confirmed) {
    char buf[64];
    snprintf(buf, sizeof buf, "{\"connected\":%s,\"confirmed\":%s}",
             connected ? "true" : "false", confirmed ? "true" : "false");
    api_emit("wg.state", buf);
}

#define WG_RETRY_MS 15000

void wg_tick(void) {
    if (!netif_added || peer_index == WIREGUARDIF_INVALID_INDEX) { return; }
#if CONFIG_NET_WIFI
    /* No tunnel activity -- not even a handshake attempt -- until WiFi is
     * associated AND has a DHCP address. A WiFi drop pauses new attempts. */
    if (!wifi_is_up()) { return; }
#else
    return;   /* no-WiFi build: WireGuard is disabled */
#endif

    bool up = wg_is_connected();
    bool confirmed = wg_is_confirmed();
    if (up && !was_connected) {
        handshakes++;
        last_handshake_ms = to_ms_since_boot(get_absolute_time());
        printf("wg: handshake complete with %s:%u\n",
               ipaddr_ntoa(&cfg.endpoint), (unsigned)cfg.endpoint_port);
    } else if (!up && was_connected) {
        printf("wg: peer went away\n");
    }
    if (confirmed != was_confirmed) {
        printf("wg: %s\n", confirmed ? "CONFIRMED -- the server is answering"
                                     : "no longer confirmed -- nothing valid received in the window");
    }
    if (up != was_connected || confirmed != was_confirmed) { emit_state(up, confirmed); }
    was_connected = up;
    was_confirmed = confirmed;

    /* Probe when the tunnel has been quiet for a keepalive interval, at most
     * once per interval. No traffic while data is already flowing. */
    uint32_t now_sys = sys_now();
    if (up && cfg.keepalive) {
        const struct wireguard_peer *pr = peer0();
        uint32_t ka = (uint32_t)cfg.keepalive * 1000u;
        if (pr && (uint32_t)(now_sys - pr->last_rx) >= ka &&
            (uint32_t)(now_sys - last_probe_ms) >= ka) {
            last_probe_ms = now_sys;
            send_probe();
        }
    }

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
    /* handshake.valid means a handshake is IN FLIGHT, not that the tunnel has
     * one -- it goes false the moment a keypair is derived. Reported as
     * handshake_valid it read as "this tunnel is broken" on a tunnel that was
     * carrying traffic perfectly well. The field that answers "is there a
     * usable session" is the keypair. */
    bool peer_active = false, hs_in_progress = false, session_valid = false;
    uint32_t last_init_tx = 0, last_rx = 0;
    if (netif_added && wg_netif.state) {
        const struct wireguard_device *dev =
            (const struct wireguard_device *)wg_netif.state;
        const struct wireguard_peer *pr = &dev->peers[0];
        if (pr->valid) {
            peer_active    = pr->active;
            hs_in_progress = pr->handshake.valid;
            session_valid  = pr->curr_keypair.valid || pr->prev_keypair.valid;
            last_init_tx   = pr->last_initiation_tx;
            last_rx        = pr->last_rx;
        }
    }

    return (size_t)snprintf(dst, cap,
        "{\"configured\":%s,\"netif_up\":%s,\"connected\":%s,\"confirmed\":%s,"
        "\"address\":\"%s\",\"endpoint\":\"%s\",\"endpoint_port\":%u,"
        "\"current_endpoint\":\"%s\",\"current_port\":%u,"
        "\"handshakes\":%lu,\"connect_attempts\":%lu,"
        "\"last_handshake_age_ms\":%lu,"
        "\"rx_bytes\":%llu,\"tx_bytes\":%llu,"
        "\"peer_active\":%s,\"handshake_in_progress\":%s,\"session_valid\":%s,"
        "\"initiation_sent_ms_ago\":%ld,\"last_rx_ms_ago\":%ld,"
        "\"boot_count\":%lu,\"tai64n_offset_s\":%llu,"
        "\"is_default_route\":%s}",
        cfg.present ? "true" : "false",
        (netif_added && netif_is_up(&wg_netif)) ? "true" : "false",
        up ? "true" : "false",
        wg_is_confirmed() ? "true" : "false",
        cfg.present ? s_addr : "",
        cfg.present ? s_ep : "",
        (unsigned)cfg.endpoint_port,
        s_cur,
        (unsigned)cur_port,
        (unsigned long)handshakes, (unsigned long)connect_attempts,
        (unsigned long)(handshakes ? now - last_handshake_ms : 0),
        (unsigned long long)rx_bytes, (unsigned long long)tx_bytes,
        peer_active ? "true" : "false",
        hs_in_progress ? "true" : "false",
        session_valid ? "true" : "false",
        last_init_tx ? (long)(now - last_init_tx) : -1L,
        last_rx ? (long)(now - last_rx) : -1L,
        /* The persisted boot counter and the TAI64N epoch offset it produces.
         * boot_count must INCREMENT across reboots (proof the counter file
         * persists) and tai64n_offset_s must equal boot_count << 26. If the
         * count is stuck or these read 0, the anti-lockout fix is not in
         * force and WireGuard will be slow to re-handshake after a reboot. */
        (unsigned long)wg_boot_count,
        (unsigned long long)tai64n_offset_s,
        /* Reported so a monitor can assert it, not just trust the comment
         * in wg_up(). If this is ever true, something regressed badly. */
        (netif_added && netif_default == &wg_netif) ? "true" : "false");
}

#endif /* CONFIG_NET_WIREGUARD */
