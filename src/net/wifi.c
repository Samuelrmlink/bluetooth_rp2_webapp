/* WiFi: station with SoftAP fallback, credentials from LittleFS.
 *
 * WIRING for the external module on the Feather (set in CMakeLists.txt as
 * CYW43_DEFAULT_PIN_WL_*, so moving it is a build-flag edit):
 *
 *     REG_ON -> GPIO9   CS -> GPIO10   CLK -> GPIO11   DAT -> GPIO12
 *
 * Four signals, not six: the CYW43439's gSPI is half-duplex, so DATA_OUT,
 * DATA_IN and HOST_WAKE are all one wire. Costs one PIO state machine and two
 * DMA channels; the SDK finds PIO0 full (Pico-PIO-USB uses all 32 instruction
 * slots) and lands on PIO1 by itself. A Pico W has the chip on its own pins
 * from its board header and applies none of the above.
 *
 * THE STRUCTURAL RULE, inherited from ~/Projects/pm220-pico2w and worth more
 * than anything else in this file: never touch the radio, and never write
 * flash, from a callback. Every public entry point sets a pending_* flag that
 * wifi_poll() drains. Phase 1 broke this by joining inside wifi_init() — which
 * runs before the HTTP server exists — and a misbehaving join took down the
 * whole board with no way to see why.
 *
 *     boot -> scan -+- a known SSID is visible -> join -> STA
 *                   +- none visible, or the budget expires -> SoftAP
 *
 * AP is the resting state and every failure path returns to it, so a wrong
 * password can never lock you out of the device that holds it.
 */
#include "net/wifi.h"

#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "net/mdns.h"
#include "dhcpserver.h"
#if CONFIG_NET_USB_NCM
#include "usb_ncm.h"
#endif
#include "config/config.h"

/* 192.168.4.1 is the convention the reference uses and what most phones
 * expect from a setup AP. */
#define AP_IP0 192
#define AP_IP1 168
#define AP_IP2 4
#define AP_IP3 1

#define WIFI_JOIN_MS 6000    /* association only */
#define WIFI_DHCP_MS 8000    /* ...then DHCP on top */
#define WIFI_RETRY_MS 10000  /* backoff after a failure */
#define WIFI_LOST_MS 2000    /* a link must stay down this long to count */
#define WIFI_SCAN_MS 8000    /* a scan that never completes is abandoned */
#define WIFI_BOOT_MS 45000   /* give up looking and become an AP */
#define WIFI_RESCAN_MS 30000 /* while in AP, look for a known network again */

#define SCAN_MAX 24

typedef enum { ST_IDLE = 0, ST_JOIN, ST_UP, ST_AP } wifi_state_t;

typedef struct {
    char ssid[CFG_SSID_MAX + 1];
    int16_t rssi;
    uint8_t chan;
    uint32_t auth;
} scan_ap_t;

static bool driver_up = false;
static wifi_state_t state = ST_IDLE;

static uint32_t join_start, last_fail, down_since, boot_start, scan_start, last_rescan;
static char ip_str[16] = "0.0.0.0";
static char last_error[56] = "";
static char cur_ssid[CFG_SSID_MAX + 1] = "";

static scan_ap_t scan[SCAN_MAX];
static int scan_n = 0;
static bool scanning = false;
static bool scan_done = false;

static dhcp_server_t ap_dhcp;
static bool ap_up = false;

/* Deferred work — see the header. */
static bool pending_scan = false;
static bool pending_ap = false;
static bool pending_join = false;
static char pending_ssid[CFG_SSID_MAX + 1] = "";

/* Signed compare: these are raw millisecond counters that wrap, and an
 * unsigned subtraction wraps to a huge number that reads as an instant
 * timeout. The reference hit exactly this. */
static bool timed_out(uint32_t now, uint32_t start, uint32_t ms) {
    return (int32_t)(now - start) >= (int32_t)ms;
}

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static struct netif *sta_netif(void) { return &cyw43_state.netif[CYW43_ITF_STA]; }
static struct netif *ap_netif(void)  { return &cyw43_state.netif[CYW43_ITF_AP]; }

/* --- radio power ----------------------------------------------------------
 *
 * Applied after every event that can reset the radio's idea of either setting:
 * init, re-init, and each mode change.
 *
 * KM_WIFI_LOW_POWER trades range and latency for current, and exists for the
 * same reason as the flash guard below — the Feather RP2040 USB Host's 3V3
 * regulator has no headroom for a CYW43439 at full tilt. It is board-scoped,
 * because on a board that can supply the module this is a pure downgrade.
 *
 * Two independent knobs:
 *
 *   - TX power, via the "qtxpower" iovar. The driver exposes no named API for
 *     it, but cyw43_ioctl() is public and the wire format is the one its own
 *     static helper uses: the NUL-terminated iovar name followed by a
 *     little-endian u32. Units are QUARTER-dBm, so 8 dBm is 32. Default is the
 *     regulatory maximum, around 20 dBm — dropping to 8 is a 12 dB cut, and
 *     the PA current goes with it. Still far more than enough for a device
 *     sitting on the same desk as the AP.
 *
 *   - Power management. CYW43_NONE_PM keeps the receiver on continuously,
 *     which is the most stable and the most expensive. CYW43_PERFORMANCE_PM
 *     lets it sleep between beacons.
 *
 * Without the option this is what it always was: PM off, TX power untouched. */
static void apply_radio_power(void) {
#if KM_WIFI_LOW_POWER
    cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);

    /* "qtxpower\0" + LE32. Sized generously; the name is 9 bytes. */
    uint8_t buf[16] = {0};
    const char *var = "qtxpower";
    size_t n = strlen(var) + 1;
    memcpy(buf, var, n);
    uint32_t qdbm = (uint32_t)(KM_WIFI_TX_POWER_DBM * 4);
    buf[n + 0] = (uint8_t)(qdbm);
    buf[n + 1] = (uint8_t)(qdbm >> 8);
    buf[n + 2] = (uint8_t)(qdbm >> 16);
    buf[n + 3] = (uint8_t)(qdbm >> 24);
    int rc = cyw43_ioctl(&cyw43_state, CYW43_IOCTL_SET_VAR, n + 4, buf, CYW43_ITF_STA);
    if (rc != 0) {
        /* Not fatal: the radio still works, it just draws what it always did.
         * Worth printing, because the symptom it is here to prevent shows up
         * as something that looks nothing like WiFi. */
        printf("wifi: qtxpower %d dBm rejected (%d)\n", KM_WIFI_TX_POWER_DBM, rc);
    }
#else
    /* Power management off: it trades association stability for current. */
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
#endif
}

/* --- SoftAP --------------------------------------------------------------- */

static void stop_ap(void) {
    if (!ap_up) return;
    ap_up = false;
    mdns_remove_netif(ap_netif());
    dhcp_server_deinit(&ap_dhcp);
    cyw43_arch_disable_ap_mode();
    printf("wifi: AP down\n");
}

/*
 * Is there already a way to reach this device?
 *
 * The SoftAP exists so a device with no other route can still be configured.
 * When USB-NCM is up there IS another route, and the AP is not free: it wants
 * DHCP on port 67, which NCM already serves, and bringing it up over a live
 * NCM link killed that link outright -- measured, reproducibly, about 42
 * seconds after every boot. Trading a working path for a worse one is not a
 * fallback, so "auto" now only falls back when there is nothing to fall back
 * from. An explicit mode of "ap" still starts it unconditionally.
 */
static bool have_other_route(void) {
#if CONFIG_NET_USB_NCM
    struct netif *n = usb_ncm_netif();
    return n && netif_is_up(n) && netif_is_link_up(n);
#else
    return false;
#endif
}

static void start_ap(void) {
    if (ap_up) return;
    /* Always disassociate first: bringing the AP up on top of a half-finished
     * join leaves the radio in a state the reference documents as needing a
     * DISASSOC every time. */
    cyw43_arch_disable_sta_mode();

    const char *ssid = cfg_ap_ssid();
    const char *psk = cfg_ap_psk();
    cyw43_arch_enable_ap_mode(ssid, psk[0] ? psk : NULL,
                              psk[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
    apply_radio_power();

    ip4_addr_t gw, mask;
    IP4_ADDR(&gw, AP_IP0, AP_IP1, AP_IP2, AP_IP3);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    struct netif *nif = ap_netif();
    netif_set_addr(nif, &gw, &mask, &gw);
    netif_set_up(nif);
    netif_set_link_up(nif);

    dhcp_server_init(&ap_dhcp, nif, (const ip_addr_t *)&gw, (const ip_addr_t *)&mask);
    mdns_add_netif(nif);

    ap_up = true;
    state = ST_AP;
    cur_ssid[0] = 0;
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", AP_IP0, AP_IP1, AP_IP2, AP_IP3);
    last_rescan = now_ms();
    printf("wifi: AP '%s' up, http://%s/\n", ssid, ip_str);
}

static int ap_clients(void) {
    if (!ap_up) return 0;
    int n = 0;
    uint8_t macs[32 * 6];
    cyw43_wifi_ap_get_stas(&cyw43_state, &n, macs);
    return n;
}

/* --- scanning ------------------------------------------------------------- */

static int scan_cb(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (!r || scan_n >= SCAN_MAX) return 0;
    char ssid[CFG_SSID_MAX + 1];
    size_t len = r->ssid_len > CFG_SSID_MAX ? CFG_SSID_MAX : r->ssid_len;
    memcpy(ssid, r->ssid, len);
    ssid[len] = 0;
    if (!ssid[0]) return 0; /* hidden network: nothing to show or match */

    /* Keep the strongest sighting of each SSID: an AP with several radios
     * appears once per band and a list of duplicates is just noise. */
    for (int i = 0; i < scan_n; i++) {
        if (strcmp(scan[i].ssid, ssid) == 0) {
            if (r->rssi > scan[i].rssi) {
                scan[i].rssi = r->rssi;
                scan[i].chan = (uint8_t)r->channel;
                scan[i].auth = r->auth_mode;
            }
            return 0;
        }
    }
    snprintf(scan[scan_n].ssid, sizeof(scan[scan_n].ssid), "%s", ssid);
    scan[scan_n].rssi = r->rssi;
    scan[scan_n].chan = (uint8_t)r->channel;
    scan[scan_n].auth = r->auth_mode;
    scan_n++;
    return 0;
}

static void begin_scan(void) {
    cyw43_wifi_scan_options_t opts = {0};
    scan_n = 0;
    scan_done = false;
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_cb) != 0) {
        snprintf(last_error, sizeof(last_error), "scan failed to start");
        scanning = false;
        return;
    }
    scanning = true;
    scan_start = now_ms();
}

/* Strongest known network that is actually on the air — joining one that
 * isn't visible just burns the association timeout. */
static const cfg_net_t *known_in_scan(void) {
    const cfg_net_t *best = NULL;
    int16_t best_rssi = -32768;
    for (int i = 0; i < scan_n; i++) {
        const cfg_net_t *k = cfg_net_find(scan[i].ssid);
        if (k && scan[i].rssi > best_rssi) {
            best = k;
            best_rssi = scan[i].rssi;
        }
    }
    return best;
}

/* --- joining -------------------------------------------------------------- */

static void begin_join(const char *ssid, const char *psk) {
    if (!ssid || !ssid[0]) {
        state = ST_IDLE;
        return;
    }
    stop_ap();
    cyw43_arch_enable_sta_mode();
    apply_radio_power();

    /* Async: the blocking variant would stall tud_task() for seconds and the
     * native USB device drops its host connection if it isn't pumped. */
    int err = cyw43_arch_wifi_connect_async(ssid, psk && psk[0] ? psk : NULL,
                                            psk && psk[0] ? CYW43_AUTH_WPA2_AES_PSK
                                                          : CYW43_AUTH_OPEN);
    if (err) {
        snprintf(last_error, sizeof(last_error), "connect_async err %d", err);
        last_fail = now_ms();
        state = ST_IDLE;
        return;
    }
    snprintf(cur_ssid, sizeof(cur_ssid), "%s", ssid);
    printf("wifi: joining '%s'\n", ssid);
    join_start = now_ms();
    state = ST_JOIN;
}

static void on_up(void) {
    struct netif *nif = sta_netif();
    snprintf(ip_str, sizeof(ip_str), "%s", ip4addr_ntoa(netif_ip4_addr(nif)));
    last_error[0] = 0;
    down_since = 0;
    state = ST_UP;
    /* Registered here, not at init: the responder announces the netif's
     * address and there isn't one until DHCP completes. */
    mdns_add_netif(nif);
    printf("wifi: up as '%s', http://%s/ and http://%s.local/\n",
           cur_ssid, ip_str, mdns_hostname());
}

static void on_lost(const char *why) {
    snprintf(last_error, sizeof(last_error), "%s", why);
    printf("wifi: %s\n", why);
    mdns_remove_netif(sta_netif());
    strcpy(ip_str, "0.0.0.0");
    last_fail = now_ms();
    down_since = 0;
    state = ST_IDLE;
}

/* --- lifecycle ------------------------------------------------------------ */

/* The radio is shared with Bluetooth now, so main() owns cyw43_arch_init()
 * and calls it exactly once -- that single call brings up the cyw43 driver,
 * lwIP and BTstack on one async_context_poll. Calling it here as well (which
 * is what this file did when Wi-Fi was the only user) hard-asserts on the
 * second gpio_add_raw_irq_handler for GPIO 24. */
bool wifi_init(void) {
    driver_up = true;
    cyw43_arch_enable_sta_mode();
    apply_radio_power();
    boot_start = now_ms();
    printf("wifi: radio up, mode=%s, %d known network(s)\n", cfg_mode(), cfg_net_count());
    wifi_apply_mode();
    return true;
}

void wifi_apply_mode(void) {
    if (strcmp(cfg_mode(), "ap") == 0) {
        pending_ap = true;
    } else {
        /* "sta" and "auto" both start by looking at what's on the air. */
        pending_scan = true;
    }
}

bool wifi_scan_start(void) {
    if (!driver_up || scanning) return false;
    pending_scan = true;
    return true;
}

bool wifi_scanning(void) { return scanning; }

bool wifi_connect(const char *ssid, const char *psk) {
    if (!cfg_net_put(ssid, psk)) return false;
    snprintf(pending_ssid, sizeof(pending_ssid), "%s", ssid);
    pending_join = true;
    return true;
}

void wifi_poll(void) {
    if (!driver_up) return;

    /* Drives the cyw43 async context, which also runs lwIP's timers in poll
     * mode — this stands in for sys_check_timeouts(). */
    cyw43_arch_poll();

    uint32_t now = now_ms();

    /* Deferred work first, one action per pass so a request can never
     * re-enter the radio on top of itself. */
    if (pending_ap) {
        pending_ap = false;
        start_ap();
        return;
    }
    if (pending_join) {
        /* NOT while a scan is in flight.
         *
         * begin_join issues a burst of ioctls and the driver cannot service
         * them on top of an active scan: the ioctl times out
         * ("[CYW43] do_ioctl(2, 263, 27): timeout"), the call does not
         * return, and because poll mode's wait is a bare sleep_until() --
         * it does NOT service the async context -- the main loop is stuck
         * with it. tud_task() then stops being pumped and the host drops
         * USB-NCM, so the device goes off the network entirely and does not
         * come back without a reset.
         *
         * ST_IDLE rescans continuously, so a scan is nearly always in
         * flight when someone presses Join. This is the common case, not a
         * race. Let the scan finish and take the join on a later pass. */
        if (scanning) {
            if (!cyw43_wifi_scan_active(&cyw43_state)) {
                scanning = false;
                scan_done = true;
            } else if (timed_out(now, scan_start, WIFI_SCAN_MS)) {
                scanning = false;
                scan_done = true;
                snprintf(last_error, sizeof(last_error), "scan timed out");
            }
            return;
        }
        pending_join = false;
        const cfg_net_t *k = cfg_net_find(pending_ssid);
        begin_join(pending_ssid, k ? k->psk : NULL);
        return;
    }
    if (pending_scan && !scanning) {
        pending_scan = false;
        begin_scan();
        return;
    }

    if (scanning) {
        if (!cyw43_wifi_scan_active(&cyw43_state)) {
            scanning = false;
            scan_done = true;
        } else if (timed_out(now, scan_start, WIFI_SCAN_MS)) {
            scanning = false;
            scan_done = true;
            snprintf(last_error, sizeof(last_error), "scan timed out");
        }
    }

    const bool ap_only = strcmp(cfg_mode(), "ap") == 0;

    switch (state) {
    case ST_IDLE:
        if (ap_only) {
            pending_ap = true;
            break;
        }
        if (!timed_out(now, last_fail, WIFI_RETRY_MS)) break;
        if (scan_done && !scanning) {
            scan_done = false;
            const cfg_net_t *k = known_in_scan();
            if (k) {
                begin_join(k->ssid, k->psk);
            } else if (strcmp(cfg_mode(), "auto") == 0 &&
                       timed_out(now, boot_start, WIFI_BOOT_MS) &&
                       !have_other_route()) {
                /* Nothing we know is on the air and the budget is spent. An
                 * AP you can reach beats a station that never arrives --
                 * UNLESS there is already a way in, which have_other_route()
                 * decides. See its comment: starting the AP over a live
                 * USB-NCM link took the working path away and offered a
                 * worse one in exchange. */
                pending_ap = true;
            } else {
                pending_scan = true;
            }
        } else if (!scanning) {
            pending_scan = true;
        }
        break;

    case ST_JOIN: {
        int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link == CYW43_LINK_UP) {
            on_up();
        } else if (link == CYW43_LINK_BADAUTH) {
            on_lost("bad password");
        } else if (link == CYW43_LINK_NONET) {
            on_lost("network not found");
        } else if (link == CYW43_LINK_FAIL) {
            on_lost("association failed");
        } else if (link == CYW43_LINK_JOIN || link == CYW43_LINK_NOIP) {
            /* BOTH mean the association succeeded and only DHCP is
             * outstanding — cyw43.h calls LINK_JOIN "Connected to wifi".
             * Measured: treating JOIN as failure tore down a live association
             * at 6s and turned a ~9s connect into a reproducible 18.4s one. */
            if (timed_out(now, join_start, WIFI_JOIN_MS + WIFI_DHCP_MS)) {
                on_lost("dhcp timeout");
            }
        } else if (timed_out(now, join_start, WIFI_JOIN_MS)) {
            on_lost("association timeout");
        }
        break;
    }

    case ST_UP: {
        int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link == CYW43_LINK_UP) {
            down_since = 0;
        } else if (down_since == 0) {
            /* Require the failure to persist: link status flaps during normal
             * roaming and one bad sample is not a loss. */
            down_since = now;
        } else if (timed_out(now, down_since, WIFI_LOST_MS)) {
            on_lost("link lost");
        }
        break;
    }

    case ST_AP:
        if (ap_only) break;
        /* Keep looking for a known network, but not while someone is using
         * the AP: a scan hops the radio off-channel and drops AP clients —
         * which, if they are mid-way through configuring WiFi, is the worst
         * possible moment. */
        if (!scanning && ap_clients() == 0 && timed_out(now, last_rescan, WIFI_RESCAN_MS)) {
            last_rescan = now;
            if (scan_done) {
                scan_done = false;
                const cfg_net_t *k = known_in_scan();
                if (k) begin_join(k->ssid, k->psk);
            } else {
                pending_scan = true;
            }
        }
        break;
    }
}

bool wifi_is_up(void) { return state == ST_UP; }

const char *wifi_ip(void) { return ip_str; }

/* --- JSON for the API ----------------------------------------------------- */

static const char *auth_name(uint32_t a) {
    switch (a) {
    case CYW43_AUTH_OPEN: return "open";
    case CYW43_AUTH_WPA_TKIP_PSK: return "wpa";
    case CYW43_AUTH_WPA2_AES_PSK: return "wpa2";
    case CYW43_AUTH_WPA2_MIXED_PSK: return "wpa2-mixed";
    default: return "unknown";
    }
}

static const char *mode_name(void) {
    if (!driver_up) return "down";
    switch (state) {
    case ST_UP: return "sta";
    case ST_JOIN: return "connecting";
    case ST_AP: return "ap";
    default: return "idle";
    }
}

int wifi_status_json(char *buf, size_t cap) {
    return snprintf(buf, cap,
                    "{\"ok\":true,\"driver\":%s,\"mode\":\"%s\",\"want\":\"%s\","
                    "\"ssid\":\"%s\",\"ip\":\"%s\",\"ap_ssid\":\"%s\",\"ap_psk\":\"%s\","
                    "\"ap_clients\":%d,\"scanning\":%s,\"error\":\"%s\"}",
                    driver_up ? "true" : "false", mode_name(), cfg_mode(),
                    cur_ssid, ip_str, cfg_ap_ssid(), cfg_ap_psk(),
                    ap_clients(), scanning ? "true" : "false", last_error);
}

int wifi_networks_json(char *buf, size_t cap) {
    size_t n = 0;
    n += (size_t)snprintf(buf + n, cap - n, "{\"ok\":true,\"networks\":[");
    for (int i = 0; i < cfg_net_count() && n + 128 < cap; i++) {
        const cfg_net_t *k = cfg_net_at(i);
        n += (size_t)snprintf(buf + n, cap - n, "%s{\"ssid\":\"%s\",\"psk\":\"%s\",\"current\":%s}",
                              i ? "," : "", k->ssid, k->psk,
                              strcmp(k->ssid, cur_ssid) == 0 ? "true" : "false");
    }
    n += (size_t)snprintf(buf + n, cap - n, "]}");
    return (int)n;
}

int wifi_scan_json(char *buf, size_t cap) {
    size_t n = 0;
    n += (size_t)snprintf(buf + n, cap - n, "{\"ok\":true,\"scanning\":%s,\"aps\":[",
                          scanning ? "true" : "false");
    for (int i = 0; i < scan_n && n + 96 < cap; i++) {
        n += (size_t)snprintf(buf + n, cap - n,
                              "%s{\"ssid\":\"%s\",\"rssi\":%d,\"chan\":%u,\"auth\":\"%s\","
                              "\"known\":%s}",
                              i ? "," : "", scan[i].ssid, scan[i].rssi, scan[i].chan,
                              auth_name(scan[i].auth),
                              cfg_net_find(scan[i].ssid) ? "true" : "false");
    }
    n += (size_t)snprintf(buf + n, cap - n, "]}");
    return (int)n;
}
