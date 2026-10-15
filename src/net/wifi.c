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
 * TWO THINGS live here, and they used to be one state machine.
 *
 *   - The STATION machine: ST_IDLE -> scan -> a known SSID -> ST_JOIN -> ST_UP,
 *     back to ST_IDLE on loss. Runs in auto, sta and both.
 *   - The SoftAP, a SERVICE rather than a state: wifi_poll() drives ap_up
 *     toward ap_wanted() once per pass. It exists so a device with no working
 *     station can still be configured -- see ap_wanted() for the policy.
 *
 * The CYW43439 nominally runs AP and STA at once ("apsta" is set in the
 * driver; each interface has its own itf_state bit and netif), and this file
 * no longer forbids it: start_ap() does not take the station down and
 * begin_join() does not stop the AP. But the SDK's poll-mode driver does NOT
 * survive it in practice -- the measurements are at start_ap() -- so `auto`
 * never has both on the air at once: the station is parked while the AP
 * serves, and a rejoin stops the AP first. Only the experimental `both`
 * attempts true concurrency. What the AP-as-a-service still buys is care
 * about lwIP's default route (fix_default_route), a DHCP server per
 * interface (dhcpserver.c), and hold timers between radio transitions.
 */
#include "net/wifi.h"

#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "net/mdns.h"
#include "dhcpserver.h"
#include "net/wg.h"
#include "net/uplink.h"
#if CONFIG_BT_LE
#include "bt/bt.h"
#include "bt/le/central.h"
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
#define WIFI_BOOT_MS 45000   /* scan eagerly this long after boot, then settle */
/*
 * How often to re-scan once the boot budget is spent and we are only waiting
 * for a known network to appear.
 *
 * This is not politeness, it is a shared-bus constraint. The CYW43439 carries
 * WiFi and Bluetooth over one interface, and before this the idle path
 * re-armed a scan the instant the previous one finished -- a continuous scan
 * loop for the whole life of the device. Run alongside BLE that eventually
 * wedged a cyw43 ioctl ("do_ioctl(2, 263, 82): timeout"), and because poll
 * mode's wait is a bare sleep_until() that never services the async context,
 * the main loop went with it and took USB-NCM down until a reset. Try hard
 * during the boot budget, then settle. An explicit wifi.scan is unaffected.
 */
#define WIFI_IDLE_RESCAN_MS 60000
/*
 * The SoftAP policy, see ap_wanted().
 *
 * GRACE: in auto, how long after boot the AP is held back so that the scan,
 * join and DHCP get a chance to bring the station up before a fallback is
 * offered. Measured: the station is up at 20-25 s on the bench (the AP's own
 * boot floor and the Bluetooth core come first), so 30 s covers it. The cost
 * falls on a device with no network in range: it waits 30 s for its AP.
 *
 * HOLD: the minimum spacing between AP transitions. Each one is a burst of
 * ioctls on the shared radio; a policy that flips twice a second would be a
 * DoS on ourselves. A user's explicit mode change clears the hold.
 */
#define WIFI_AP_GRACE_MS 30000
#define WIFI_AP_HOLD_MS  15000
/*
 * SETTLE: no join and no scan for this long after the AP comes up, and a short
 * gap between taking the station down and starting the AP. The committed code
 * scanned straight after bss-up for months without incident, so 5 s is
 * caution rather than measurement. (25 s was tried here to make a join OVER
 * the AP survive; it did not -- see start_ap.)
 */
#define WIFI_AP_SETTLE_MS 5000
#define WIFI_LEAVE_GAP_MS 5000
/*
 * BOOT FLOOR: the AP is never started in the first seconds of a boot, in any
 * mode. Measured: an AP init at 2-3 s after boot wedged the driver on every
 * attempt (watchdog loop), with the station interface down and idle; the
 * same init at 17 s and later always succeeded. The one thing happening on
 * the shared SPI bus in those first seconds is BTstack bringing the Bluetooth
 * core up -- firmware download and HCI init -- and the AP's ioctl burst on
 * top of it is what does not come back. A scan or a join at ~10 s survives
 * it; the AP does not. 15 s is generous by measurement, not by theory.
 */
#define WIFI_AP_BOOT_MIN_MS 15000

#define SCAN_MAX 24

/*
 * The cyw43 driver's join-state bits. They are private to cyw43_ctrl.c
 * (WIFI_JOIN_STATE_*), copied here because adopt_firmware_rejoin() has to read
 * them -- see there for why.
 */
#define JS_ACTIVE 0x0001u
#define JS_AUTH   0x0200u
#define JS_LINK   0x0400u
#define JS_KEYED  0x0800u

typedef enum { ST_IDLE = 0, ST_JOIN, ST_UP } wifi_state_t;

typedef struct {
    char ssid[CFG_SSID_MAX + 1];
    int16_t rssi;
    uint8_t chan;
    uint32_t auth;
} scan_ap_t;

static bool driver_up = false;
static wifi_state_t state = ST_IDLE;

static uint32_t join_start, last_fail, down_since, boot_start, scan_start;
static uint32_t last_scan_end;   /* when the most recent scan stopped */
static bool deferred_for_bt;     /* so the deferral is logged once, not per pass */
static uint32_t fw_rejoins;      /* links taken over from the radio's own rejoin */
static uint32_t sta_down_at;     /* when the station last went down (boot counts); 0 = up */
static char ip_str[16] = "0.0.0.0";
static char last_error[56] = "";
static char cur_ssid[CFG_SSID_MAX + 1] = "";

static scan_ap_t scan[SCAN_MAX];
static int scan_n = 0;
static bool scanning = false;
static bool scan_done = false;

static dhcp_server_t ap_dhcp;
static bool ap_up = false;
static uint32_t ap_changed;         /* now_ms() at the last AP transition; 0 = act at once */
static uint32_t ap_started_ms;      /* when the AP last came up: joins/scans wait WIFI_AP_SETTLE_MS */
static uint32_t left_ms;            /* when park_sta() ran: the AP starts WIFI_LEAVE_GAP_MS later */
static bool     sta_parked;         /* station interface taken down for an AP start; re-enable after settle */
static int      ap_clients_cached;  /* one ioctl every 2 s, not one per status call */
static uint32_t ap_clients_at;
static bool     ap_deferred_for_bt; /* so the deferral is logged once */

/* Deferred work — see the header. */
static bool pending_scan = false;
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
    ap_clients_cached = 0;
    mdns_remove_netif(ap_netif());
    dhcp_server_deinit(&ap_dhcp);
    printf("wifi: AP down begin @%lu ms\n", (unsigned long)now_ms());
    cyw43_arch_disable_ap_mode();
    printf("wifi: AP down end   @%lu ms\n", (unsigned long)now_ms());
}

/* Scan eagerly while the boot budget lasts, then only occasionally -- see
 * WIFI_IDLE_RESCAN_MS for why a continuous scan is not free. */
static bool want_idle_scan(uint32_t now) {
    if (!timed_out(now, boot_start, WIFI_BOOT_MS)) { return true; }
    return timed_out(now, last_scan_end, WIFI_IDLE_RESCAN_MS);
}

/*
 * cyw43_cb_tcpip_init() calls netif_set_default() for WHICHEVER interface it
 * is bringing up (SDK cyw43_lwip.c:212). Enable the AP after the station and
 * lwIP's default route becomes the AP netif -- and WireGuard's UDP pcb is
 * bound to ANY (wg.c), so the tunnel would leave through the AP. The station
 * is the only interface that reaches the Internet; it owns the default.
 *
 * Pointing the default at a station that is down is safe: ip4_route() tries
 * every up netif by subnet first (NCM and the AP keep working) and uses the
 * default only when it is up, linked and addressed. Otherwise ERR_RTE, which
 * is the honest answer while the station is away.
 */
static void fix_default_route(void) {
    /* The decision itself now lives in src/net/uplink.c -- the only caller
     * of netif_set_default() -- because with a second uplink (Ethernet) the
     * station is no longer the only interface that reaches the Internet.
     * With WiFi alone the outcome is exactly what this function used to do:
     * the station whenever it is enabled (uplink_fallback_netif() below),
     * never the AP. */
    uplink_reselect();
}

/* For src/net/uplink.c. The station as an uplink candidate: only once this
 * state machine says it is up (associated AND addressed), which is the same
 * gate WireGuard always waited behind. */
struct netif *wifi_uplink_netif(void) {
    return state == ST_UP ? sta_netif() : NULL;
}

/* Where the default route goes when no uplink is usable: the station
 * whenever it is enabled, as fix_default_route() always did. NULL when it is
 * not (AP-only, off), leaving the route where it is. */
struct netif *wifi_fallback_netif(void) {
    return (cyw43_state.itf_state & (1 << CYW43_ITF_STA)) ? sta_netif() : NULL;
}

/*
 * THE AP POLICY. Evaluated every pass; wifi_poll() drives ap_up toward it.
 *
 *   ap, both  -> always.
 *   sta, off  -> never.
 *   auto      -> after WIFI_AP_GRACE_MS, whenever the station is NOT
 *                associated. Not "unless WireGuard is confirmed", as first
 *                specified: that policy assumed the AP could run beside an
 *                associated station, and on this driver it cannot (see
 *                start_ap). Keeping the AP up would mean dropping the LAN, so
 *                an associated station wins. wg_is_confirmed() is still
 *                computed and reported (wg.status, wg.state); making the
 *                tunnel gate the AP again is one line here, if wanted.
 *                USB-NCM is deliberately absent from this decision (owner's
 *                call): the AP behaves the same on the bench as in the field.
 */
static bool ap_wanted(uint32_t now) {
    if (!timed_out(now, boot_start, WIFI_AP_BOOT_MIN_MS)) return false;   /* see WIFI_AP_BOOT_MIN_MS */
#if CONFIG_BT_LE
    /* And not until the Bluetooth core has finished coming up on the shared
     * bus (HCI_STATE_WORKING) -- the time floor is the measurement, this is
     * the mechanism it stands in for. */
    if (!bt_ready()) return false;
#endif
    const char *m = cfg_mode();
    if (strcmp(m, "ap") == 0 || strcmp(m, "both") == 0) return true;
    if (strcmp(m, "auto") != 0) return false;
    if (!timed_out(now, boot_start, WIFI_AP_GRACE_MS)) return false;
    return state != ST_UP;
}

/*
 * ORDER MATTERS, and the bench decided it -- three times over.
 *
 * cyw43_ll_wifi_ap_init() writes bsscfg:ssid, WLC_SET_CHANNEL, bsscfg:wsec
 * and bss-up through the STATION interface. With the station interface
 * ENABLED -- associated, joining, or merely idle -- those ioctls stop being
 * answered (waits of 150, 310, 504, 648 ms, then nothing) and the watchdog
 * reboots the board. Measured on every attempt, including one where the AP
 * started 2 s after boot with the station idle. With the station interface
 * DISABLED the same init completes in ~370 ms.
 *
 * The other half -- re-enabling the station and joining it OVER the running
 * AP -- works only sometimes. One boot in four came up with both interfaces
 * (and 1-2 s ioctl overruns while it lasted); the other three timed out an
 * ioctl mid-join ("[CYW43] do_ioctl(2, 263, 23): timeout"), stopped answering,
 * and the watchdog rebooted the board. 5 s and 25 s of AP settle alike.
 *
 * So start_ap() is only ever called with the station interface down: the
 * policy step calls park_sta() one pass earlier (never during a join --
 * disassociating under an outstanding connect_async is the other thing that
 * wedges), waits a short gap, then starts the AP. In auto the station then
 * STAYS parked while the AP is up, and a rejoin begins with stop_ap() -- the
 * order the old AP state used for months. Only `both` re-enables the
 * station under a live AP, as the experiment it is.
 */
static void start_ap(void) {
    if (ap_up) return;

    const char *ssid = cfg_ap_ssid();
    const char *psk = cfg_ap_psk();
    printf("wifi: AP up begin @%lu ms\n", (unsigned long)now_ms());
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
    fix_default_route();

    dhcp_server_init(&ap_dhcp, nif, (const ip_addr_t *)&gw, (const ip_addr_t *)&mask);
    mdns_add_netif(nif);

    ap_up = true;
    ap_clients_cached = 0;
    ap_clients_at = now_ms();
    ap_started_ms = now_ms();
    printf("wifi: AP up end   @%lu ms -- '%s' at %d.%d.%d.%d\n",
           (unsigned long)now_ms(), ssid, AP_IP0, AP_IP1, AP_IP2, AP_IP3);
}

/* Take the station interface down so the AP can start (see start_ap).
 * Bookkeeping mirrors on_lost(), but last_fail is NOT set: the rejoin should
 * happen as soon as the AP has settled, not after the failure backoff. */
static void park_sta(void) {
    if (state == ST_UP) { mdns_remove_netif(sta_netif()); }
    printf("wifi: parking the station (%s) so the AP can start\n",
           cur_ssid[0] ? cur_ssid : "idle");
    cyw43_arch_disable_sta_mode();
    strcpy(ip_str, "0.0.0.0");
    down_since = 0;
    state = ST_IDLE;
    scan_done = false;          /* rescan once the AP has settled */
    sta_parked = true;
    left_ms = now_ms();
}

static bool sta_enabled(void) { return (cyw43_state.itf_state & (1 << CYW43_ITF_STA)) != 0; }

/* The radio has just brought the AP up; give it a moment before a join or a
 * scan asks it to do something else on the other interface. */
static bool ap_settling(uint32_t now) {
    return ap_up && ap_started_ms && !timed_out(now, ap_started_ms, WIFI_AP_SETTLE_MS);
}

/* cyw43_wifi_ap_get_stas() is an ioctl. It used to run once per status poll
 * AND once per loop pass while the AP was a state; now once per 2 s. */
static void refresh_ap_clients(uint32_t now) {
    if (!ap_up) { ap_clients_cached = 0; return; }
    if (!timed_out(now, ap_clients_at, 2000)) return;
    ap_clients_at = now;
    int n = 0;
    uint8_t macs[32 * 6];
    cyw43_wifi_ap_get_stas(&cyw43_state, &n, macs);
    ap_clients_cached = n;
}

/* Someone is on the AP, possibly mid-way through configuring us. A scan hops
 * the radio off-channel and drops them, which is the worst possible moment. */
static bool ap_busy(void) { return ap_up && ap_clients_cached > 0; }

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

#if CONFIG_BT_LE
/*
 * THE RADIO WINDOW.
 *
 * begin_join() defers while Bluetooth uses the radio (a join under BLE wedges
 * the driver), and le_radio_busy() counts every open link -- so with the
 * Anker script holding the C1000, a lost station could NEVER rejoin: not a
 * different access point, not on the user's Connect (seen 2026-10-15: the AP
 * went away, a second one was in range and saved, the station sat "idle"
 * for good). adopt_firmware_rejoin() only covers the SAME AP coming back.
 *
 * So WiFi may take the radio for a bounded window: Bluetooth stops scanning,
 * cancels a connect and closes its links (le_release_radio), new scans and
 * connects are refused (wifi_bt_hold), the join runs, and when the station is
 * up -- or the window times out -- Bluetooth is let back and its profiles
 * reconnect on their own. Opened automatically once the station has been down
 * WIFI_WIN_AFTER_MS with a saved network in range (backing off after a failed
 * window), or at once on the user's Connect.
 */
#define WIFI_WIN_AFTER_MS        60000u    /* station down this long before BT is interrupted */
#define WIFI_WIN_MAX_MS          30000u    /* links closing + join + DHCP */
#define WIFI_WIN_HARD_MS         45000u    /* ...unless a join is still resolving */
#define WIFI_WIN_BACKOFF_MIN_MS 120000u
#define WIFI_WIN_BACKOFF_MAX_MS 1800000u
static uint32_t win_start;                 /* 0 = no window open */
static uint32_t win_not_before;            /* next automatic window (after a failed one) */
static uint32_t win_backoff = WIFI_WIN_BACKOFF_MIN_MS;
static uint32_t win_count, win_ok;
static const char *win_reason = "";

static void win_open(uint32_t now, const char *reason) {
    win_start = now ? now : 1;
    win_reason = reason;
    win_count++;
    printf("wifi: taking the radio from Bluetooth for a join (%s)\n", reason);
    le_release_radio(reason);
}

static void win_close(uint32_t now, bool joined) {
    if (!win_start) return;
    win_start = 0;
    if (joined) {
        win_ok++;
        win_backoff = WIFI_WIN_BACKOFF_MIN_MS;
        win_not_before = 0;
        printf("wifi: joined; Bluetooth may have the radio back\n");
    } else {
        win_not_before = now + win_backoff;
        printf("wifi: radio window ended without a join; next automatic one in %lu s\n",
               (unsigned long)(win_backoff / 1000));
        win_backoff = win_backoff * 2 > WIFI_WIN_BACKOFF_MAX_MS ? WIFI_WIN_BACKOFF_MAX_MS : win_backoff * 2;
    }
}

/* May an automatic window open now? (The user's Connect does not ask.) */
static bool win_auto_ok(uint32_t now) {
    return sta_down_at && timed_out(now, sta_down_at, WIFI_WIN_AFTER_MS)
        && (win_not_before == 0 || (int32_t)(now - win_not_before) >= 0);
}
#endif

static void begin_join(const char *ssid, const char *psk) {
    if (!ssid || !ssid[0]) {
        state = ST_IDLE;
        return;
    }
#if CONFIG_BT_LE
    /*
     * The same one-radio rule wifi_connect() enforces, applied to the paths
     * the STATE MACHINE takes -- a reconnect after a drop, or a known network
     * reappearing in a scan. Guarding only the user-facing call would leave
     * the device able to wedge itself unattended, which is worse than doing
     * it on request.
     *
     * Deferred rather than failed: the machine will come back. If BLE is busy
     * forever then WiFi waits forever, which is the right way round -- a
     * degraded network path beats a board that needs a power cycle.
     */
    if (le_radio_busy()) {
        if (!deferred_for_bt) {
            printf("wifi: deferring the join, Bluetooth has the radio\n");
            deferred_for_bt = true;
        }
        state = ST_IDLE;
        return;
    }
    deferred_for_bt = false;
#endif

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
    sta_down_at = 0;
    state = ST_UP;
#if CONFIG_BT_LE
    win_close(now_ms(), true);
#endif
    /* Registered here, not at init: the responder announces the netif's
     * address and there isn't one until DHCP completes. */
    mdns_add_netif(nif);
    fix_default_route();
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
    if (!sta_down_at) sta_down_at = last_fail ? last_fail : 1;
    state = ST_IDLE;
#if CONFIG_BT_LE
    win_close(last_fail, false);
#endif
}

/*
 * THE RADIO REJOINED BY ITSELF; TAKE IT.
 *
 * Seen 2026-09-30: the access point (a Starlink router) lost power and came
 * back. The board reassociated -- the router listed it -- but never got an
 * address, and stayed off the network until it was power cycled. Two things
 * combined:
 *
 *  1. Our own rejoin could not run. on_lost() drops to ST_IDLE, and the join
 *     from there is deferred while Bluetooth holds the radio (begin_join; a
 *     join under BLE wedges the driver). le_radio_busy() counts any open
 *     link, and the gateway's job is to hold one -- the Anker script keeps
 *     the C1000 connected -- so "deferred" meant "never".
 *
 *  2. The CYW43 firmware DID rejoin, on its own: it keeps the SSID and
 *     reassociates when the AP returns, with no ioctl from us. But the driver
 *     raises the lwIP link only when its join state is exactly
 *     ACTIVE|AUTH|LINK|KEYED (cyw43_ctrl.c), and ACTIVE is set only by a
 *     host-initiated join. A DISASSOC event zeroes the state, so after one,
 *     a firmware rejoin collects AUTH|LINK|KEYED and stops one bit short: the
 *     radio is associated, the netif link stays down, DHCP never runs.
 *
 * So, from ST_IDLE: if the firmware's events say it is associated with the
 * network we were on, raise the link it forgot (the same call the driver
 * makes), make sure DHCP is running, and let ST_JOIN's DHCP wait take it
 * from there. If the link is already up -- the driver did raise it -- adopt
 * it the same way. Neither path issues a radio ioctl; DHCP is ordinary
 * traffic. So it is safe with Bluetooth busy, which is the whole point.
 *
 * What this does NOT cover: an AP that the firmware does not rejoin (it gave
 * up, or the network changed). That still needs begin_join and still waits
 * for Bluetooth to go idle.
 */
static bool adopt_firmware_rejoin(uint32_t now) {
    if (!cur_ssid[0] || !sta_enabled() || sta_parked) { return false; }
    struct netif *nif = sta_netif();
    const cfg_net_t *k = cfg_net_find(cur_ssid);
    const bool open = !k || !k->psk[0];
    /* An open network has no key exchange; the driver presets KEYED for it
     * on a host join, and a firmware rejoin never sets it. */
    const uint32_t need = JS_AUTH | JS_LINK | (open ? 0 : JS_KEYED);
    const uint32_t js = cyw43_state.wifi_join_state;

    cyw43_arch_lwip_begin();
    const bool link_up = netif_is_link_up(nif);
    if (!link_up && (js & need) != need) {
        cyw43_arch_lwip_end();
        return false;
    }
    if (!link_up) {
        printf("wifi: the radio rejoined '%s' by itself; raising the link the driver "
               "left down (join state 0x%04lx)\n", cur_ssid, (unsigned long)js);
        cyw43_state.wifi_join_state = JS_ACTIVE;   /* as after a host join */
        cyw43_cb_tcpip_set_link_up(&cyw43_state, CYW43_ITF_STA);
    } else {
        printf("wifi: the radio rejoined '%s' by itself\n", cur_ssid);
    }
    /* Raising the link restarts DHCP (dhcp_network_changed_link_up) unless
     * it was stopped outright; then it needs starting. */
    struct dhcp *d = netif_dhcp_data(nif);
    if (!d || d->state == DHCP_STATE_OFF) { dhcp_start(nif); }
    cyw43_arch_lwip_end();

    fw_rejoins++;
    snprintf(last_error, sizeof(last_error), "rejoined by the radio; waiting for DHCP");
    join_start = now;
    state = ST_JOIN;
    return true;
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
    sta_down_at = boot_start ? boot_start : 1;
    printf("wifi: radio up, mode=%s, %d known network(s)\n", cfg_mode(), cfg_net_count());
    wifi_apply_mode();
    return true;
}

/* Put the WiFi side of the radio down and leave it down.
 *
 * This exists because the CYW43439 carries WiFi and Bluetooth on one
 * interface: with WiFi associated or scanning there are windows where a BLE
 * operation has to wait, and until now there was no way to give the radio
 * back short of forgetting the network or rebooting. "off" was named in
 * wifi.mode's own error message and in the UI the whole time, and did
 * nothing. */
static void go_off(void) {
    stop_ap();
    if (scanning) { scanning = false; }
    pending_scan = pending_join = false;
    if (state == ST_UP) { mdns_remove_netif(sta_netif()); }
    printf("wifi: off: disable_sta_mode begin @%lu ms\n", (unsigned long)now_ms());
    cyw43_arch_disable_sta_mode();
    printf("wifi: off: disable_sta_mode end   @%lu ms\n", (unsigned long)now_ms());
    strcpy(ip_str, "0.0.0.0");
    cur_ssid[0] = 0;
    last_error[0] = 0;
    down_since = 0;
    sta_parked = false;
    state = ST_IDLE;
    printf("wifi: off -- the radio is Bluetooth's now\n");
}

bool wifi_is_off(void) { return strcmp(cfg_mode(), "off") == 0; }

void wifi_apply_mode(void) {
    if (wifi_is_off()) {
        go_off();
        return;
    }

    /* A user asked for this arrangement; do not make them wait out the hold
     * that protects against the POLICY flapping. */
    ap_changed = 0;

    if (strcmp(cfg_mode(), "ap") == 0) {
        /* AP only: the station is deliberately down. This is the one place
         * that still disables it, and it is a user's explicit choice. */
        if (state == ST_UP) { mdns_remove_netif(sta_netif()); }
        cyw43_arch_disable_sta_mode();
        strcpy(ip_str, "0.0.0.0");
        cur_ssid[0] = 0;
        state = ST_IDLE;
        sta_parked = false;
        pending_scan = pending_join = false;
        return;
    }

    /* Coming back from "off" or "ap", the station interface is disabled and
     * nothing else turns it on again. A scan issued against a disabled
     * station finds nothing, so the machine sat at "idle 0.0.0.0" forever and
     * off became a one-way door. */
    printf("wifi: on: enable_sta_mode begin @%lu ms\n", (unsigned long)now_ms());
    cyw43_arch_enable_sta_mode();
    printf("wifi: on: enable_sta_mode end   @%lu ms\n", (unsigned long)now_ms());
    apply_radio_power();
    fix_default_route();

    /* sta, auto and both all start by looking at what's on the air. */
    pending_scan = true;
}

bool wifi_scan_start(void) {
    if (!driver_up || scanning) return false;
    pending_scan = true;
    return true;
}

bool wifi_scanning(void) { return scanning; }

/*
 * ONE RADIO, AND THE JOIN LOSES.
 *
 * The CYW43439 carries WiFi and Bluetooth over a single interface. A WiFi
 * join issued while the BLE side is active wedges the driver: measured
 * repeatedly, the call never returns, the main loop never runs again, USB-NCM
 * drops because tud_task() stops being pumped, and RTT goes quiet too. Only a
 * reset recovers it, and halting the target each time put the PC in
 * timer_time_reached -- a busy-wait inside the driver, below anything this
 * layer can reach.
 *
 * Two orderings were tried and both wedged: stopping the BLE scan inline
 * before the join, and stopping it one pass earlier with a settle window.
 * gap_stop_scan() is itself an HCI command over the same bus, so "stop then
 * join" is not the serialisation it looks like.
 *
 * So the join is REFUSED while BLE is busy, rather than attempted and hoped
 * for. A clear error the caller can act on beats a device that falls off the
 * network and needs a power cycle. Idle, the same join takes 10-34 ms and is
 * completely reliable -- including at boot, which is why a saved network
 * still comes up by itself.
 *
 * The underlying contention is unresolved; see the task notes. Fixing it
 * properly means either finding the blocking wait in the driver or moving off
 * the poll arch, and neither belongs in a bug fix.
 */
bool wifi_connect(const char *ssid, const char *psk) {
    /* Save the credential FIRST, unconditionally. Adding a known network must
     * not depend on the shared radio being free -- Bluetooth is often busy
     * (scanning or holding a link), and gating the save on that was silently
     * dropping every network the user tried to add. */
    if (!cfg_net_put(ssid, psk)) return false;
#if CONFIG_BT_LE
    /* Bluetooth holds the radio: the network is saved, but leave the join to
     * the state machine (known_in_scan) once the radio is free. */
    /* Bluetooth holding the radio no longer stops a requested join: the poll
     * opens a radio window for it (see THE RADIO WINDOW). */
#endif
    snprintf(pending_ssid, sizeof(pending_ssid), "%s", ssid);
    pending_join = true;
    return true;
}

/*
 * Onboard LED (a GPIO on the CYW43 chip) as a link-state indicator. It
 * escalates with how far up the connectivity stack we are, and it is NEVER
 * dark while the loop runs -- any blink at all means the main loop is alive,
 * so the USB-NCM fallback should be reachable even when WiFi is not:
 *   slow blink (~1 Hz)  -> alive: idle / AP / connecting / wifi-off (use NCM)
 *   fast blink (~3 Hz)  -> WiFi associated (LAN reachable)
 *   solid on            -> WiFi + WireGuard confirmed (remotely reachable)
 * Written only on an edge, so it costs a couple of chip GPIO writes per second
 * at most. Driven from wifi_poll(), so it needs no separate tick and only runs
 * while the radio driver is up.
 */
#define LED_SLOW_MS 1000   /* alive / idle / AP */
#define LED_FAST_MS  320   /* station associated */
#define LED_REASSERT_MS 2000   /* re-write the LED at least this often, even
                                * without an edge, so a missed chip GPIO write
                                * self-heals and a lost link is never latched */
static void led_tick(void) {
    static bool inited, last_on;
    static uint32_t last_write;
    /* This used to return early while `scanning || state == ST_JOIN`, because a
     * chip GPIO write during a join turned the recoverable ~10 s join stall
     * into a hard radio wedge. That was a workaround for task #37, whose real
     * cause is fixed: cyw43_wait.c now bounds every ioctl back-off on the
     * free-running timer, so no SPI transaction can hang the loop regardless of
     * when it is issued.
     *
     * Keeping the gate actively broke the contract above. Scan + join is ~37 s
     * on a marginal AP (first attempt "network not found", retry ~20 s later),
     * and the LED was held dark for ALL of it -- exactly the window where "is
     * it alive?" matters most, and exactly what it looked like from outside:
     * nothing for half a minute, then blinking. The slow blink now runs from
     * the moment the radio is up, which is what "never dark while the loop
     * runs" was always supposed to mean. It costs two chip GPIO writes a
     * second, since the write below happens only on an edge. */
    /* Re-evaluated every call (wifi_poll runs it every loop pass), so the
     * cadence follows the CURRENT state -- lose WiFi and it drops straight
     * back to the slow blink; lose the tunnel and solid drops to fast. */
    bool want_on;
    if (wifi_is_up() && wg_is_confirmed()) {
        want_on = true;                    /* fully up: solid */
    } else {
        uint32_t period = wifi_is_up() ? LED_FAST_MS : LED_SLOW_MS;
        want_on = (now_ms() % period) < (period / 2);
    }
    uint32_t now = now_ms();
    if (!inited || want_on != last_on || (uint32_t)(now - last_write) >= LED_REASSERT_MS) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, want_on);
        last_on = want_on;
        last_write = now;
        inited = true;
    }
}

void wifi_poll(void) {
    if (!driver_up) return;

    /* Drives the cyw43 async context, which also runs lwIP's timers in poll
     * mode — this stands in for sys_check_timeouts(). */
    cyw43_arch_poll();

    led_tick();   /* link-state blink; see above */

    /* Mode "off" still polls the context above -- lwIP's timers and the
     * Bluetooth side both ride on it -- but the station/AP machine below
     * must not run, or the idle rescan would put the radio straight back on
     * the air and undo the whole point. */
    if (wifi_is_off()) { return; }

    uint32_t now = now_ms();
    refresh_ap_clients(now);

#if CONFIG_BT_LE
    if (win_start && timed_out(now, win_start, state == ST_JOIN ? WIFI_WIN_HARD_MS : WIFI_WIN_MAX_MS)) {
        if (pending_join) {
            pending_join = false;
            snprintf(last_error, sizeof(last_error), "Bluetooth did not release the radio");
        }
        win_close(now, false);
    }
#endif

    /* Deferred work first, one action per pass so a request can never
     * re-enter the radio on top of itself. */
    if (pending_join && !ap_settling(now)) {

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
                last_scan_end = now;
            } else if (timed_out(now, scan_start, WIFI_SCAN_MS)) {
                scanning = false;
                scan_done = true;
                last_scan_end = now;
                snprintf(last_error, sizeof(last_error), "scan timed out");
            }
            return;
        }
#if CONFIG_BT_LE
        /* Bluetooth has the radio: take it for a bounded window (see THE RADIO
         * WINDOW) and keep the join pending until its links have closed. */
        if (le_radio_busy()) {
            if (!win_start) win_open(now, "connect");
            return;
        }
#endif
        pending_join = false;
        const cfg_net_t *k = cfg_net_find(pending_ssid);
        begin_join(pending_ssid, k ? k->psk : NULL);
        return;
    }
    if (pending_scan && !scanning && !ap_settling(now) && !le_scanning()) {
        pending_scan = false;
        begin_scan();
        return;
    }

    if (scanning) {
        if (!cyw43_wifi_scan_active(&cyw43_state)) {
            scanning = false;
            scan_done = true;
            last_scan_end = now;
        } else if (timed_out(now, scan_start, WIFI_SCAN_MS)) {
            scanning = false;
            scan_done = true;
            last_scan_end = now;
            snprintf(last_error, sizeof(last_error), "scan timed out");
        }
    }

    const bool ap_only = strcmp(cfg_mode(), "ap") == 0;

    /*
     * The AP, driven toward the policy. One radio action per pass, never on
     * top of a scan, and never twice inside WIFI_AP_HOLD_MS. Bring-up is
     * deferred while Bluetooth has the radio, for the same reason begin_join
     * defers: it is the same class of ioctl burst, and a degraded network path
     * beats a board that needs a power cycle. Tear-down is never deferred --
     * it is two ioctls, and it is the safe direction.
     */
    /*
     * ap_changed == 0 means "no hold": at boot, and after a user's mode change.
     * It has to be tested explicitly -- timed_out(now, 0, HOLD) is false for
     * the first HOLD ms of uptime, and that silent 15 s hold let the station's
     * first join race ahead of the AP on every boot in `both`, so the AP
     * request landed mid-join. Which is the one thing that must not happen:
     */
    bool want = ap_wanted(now);
    bool hold_ok = (ap_changed == 0) || timed_out(now, ap_changed, WIFI_AP_HOLD_MS);
    if (want != ap_up && !scanning && hold_ok && state != ST_JOIN) {
        /* state != ST_JOIN: NEVER take the station down under an in-flight
         * join -- cyw43_wifi_leave with connect_async outstanding, followed by
         * the AP init, stopped the driver answering ioctls and the watchdog
         * rebooted the board, every boot, at the same second. And do not
         * `return` while waiting: that skipped the machine below and left the
         * join unable to resolve, ever ("connecting" for a full minute). */
#if CONFIG_BT_LE
        if (want && le_radio_busy()) {
            if (!ap_deferred_for_bt) {
                printf("wifi: deferring the AP, Bluetooth has the radio\n");
                ap_deferred_for_bt = true;
            }
        } else
#endif
        {
            ap_deferred_for_bt = false;
            if (want && sta_enabled()) {
                park_sta();             /* this pass; the AP starts after the gap */
                return;
            }
            if (!(want && left_ms && !timed_out(now, left_ms, WIFI_LEAVE_GAP_MS))) {
                if (want) { start_ap(); } else { stop_ap(); }
                ap_changed = now;
                return;
            }
            /* else: let the interface go down first, then start next pass */
        }
    }

    switch (state) {
    case ST_IDLE:
        if (ap_only) break;                 /* the station stays down on purpose */
        if (ap_settling(now)) break;        /* the AP just came up; see WIFI_AP_SETTLE_MS */
        if (sta_parked) {
            /* Parked for an AP start. Stay down until the AP is actually UP
             * and settled -- re-enabling as soon as we were merely "not
             * settling" (i.e. still inside the leave gap, AP not yet started)
             * parked and unparked the station every pass, never let the AP
             * start, and the ioctl churn tripped the watchdog. */
            if (!ap_up) break;
            if (strcmp(cfg_mode(), "both") == 0) {
                /* EXPERIMENTAL, and the reason `both` exists: the station
                 * comes back under the live AP and joins over it. It wedges
                 * the driver more often than not (see start_ap); the
                 * watchdog brings the board back. */
                printf("wifi: station back up over the AP (both: experimental)\n");
                cyw43_arch_enable_sta_mode();
                apply_radio_power();
                fix_default_route();
                sta_parked = false;
                pending_scan = true;
                break;
            }
            /* auto: the station STAYS parked while the AP serves. The rescan
             * below runs on the AP interface alone (the driver needs some
             * interface up, not the station), and a known network triggers
             * the flip in the order that works: AP down, then join. */
        }
        if (!timed_out(now, last_fail, WIFI_RETRY_MS)) break;
        if (adopt_firmware_rejoin(now)) break;
        /* AP FIRST when the mode wants it unconditionally. Joining and THEN
         * making room for the AP means a disassociation followed by the AP
         * init, and that pair is flaky (it wedged one boot in three). The AP
         * on a never-associated station, then a join over it, has not failed
         * once since Bluetooth is up. So in `both` the first join waits. */
        if (strcmp(cfg_mode(), "both") == 0 && !ap_up) break;
        if (scan_done && !scanning) {
            scan_done = false;
            const cfg_net_t *k = known_in_scan();
#if CONFIG_BT_LE
            /* A saved network in range but Bluetooth holds the radio: after
             * WIFI_WIN_AFTER_MS down, take a window and let the pending-join
             * path run once the links have closed. */
            if (k && le_radio_busy() && !win_start && !sta_parked && win_auto_ok(now)) {
                snprintf(pending_ssid, sizeof(pending_ssid), "%s", k->ssid);
                pending_join = true;
                win_open(now, "rejoin");
                break;
            }
#endif
            if (k) {
                if (sta_parked) {
                    /* The flip. stop_ap() and begin_join() in the same pass
                     * is exactly what the old AP state did, and it never
                     * wedged. ap_changed starts the hold so a failed join
                     * does not slam the AP straight back up. */
                    printf("wifi: '%s' in range, AP down for the rejoin\n", k->ssid);
                    stop_ap();
                    ap_changed = now;
                    sta_parked = false;
                }
                begin_join(k->ssid, k->psk);
            } else if (want_idle_scan(now) && !ap_busy()) {
                pending_scan = true;
            }
        } else if (!scanning && want_idle_scan(now) && !ap_busy()) {
            /* With the AP up and idle this is the old AP-state rescan, at the
             * idle cadence (60 s) rather than the old 30 s: the AP is no
             * longer a state that has to be escaped, just a service. */
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
    }
}

bool wifi_ap_up(void) { return ap_up; }

bool wifi_is_up(void) { return state == ST_UP; }

/*
 * True while Bluetooth should hold off scanning/connecting, so WiFi gets the
 * shared radio to itself for its initial scan + join. Released as soon as
 * WiFi is up, or when the grace window expires (WiFi failing must never block
 * BT forever), or immediately in mode "off" (no WiFi to wait for). Consumed
 * by src/bt/le/central.c's scan/connect gates. See task #37: bringing WiFi,
 * BT and WireGuard onto the one CYW43 at once intermittently wedged it.
 */
#define WIFI_BT_GRACE_MS 60000
bool wifi_bt_hold(void) {
    if (wifi_is_off()) return false;
#if CONFIG_BT_LE
    if (win_start) return true;          /* a radio window: no new scans/connects */
#endif
    if (wifi_is_up())  return false;
    return !timed_out(now_ms(), boot_start, WIFI_BT_GRACE_MS);
}

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

/* The STATION's state. The AP is reported separately as "ap": true|false,
 * because they are independent now; "ap" as a MODE name survives only for the
 * ap-only arrangement, where it is the whole story. */
static const char *mode_name(void) {
    if (!driver_up) return "down";
    switch (state) {
    case ST_UP: return "sta";
    case ST_JOIN: return "connecting";
    default: return (ap_up && (strcmp(cfg_mode(), "ap") == 0 || sta_parked)) ? "ap" : "idle";
    }
}

int wifi_status_json(char *buf, size_t cap) {
    return snprintf(buf, cap,
                    "{\"ok\":true,\"driver\":%s,\"mode\":\"%s\",\"want\":\"%s\","
                    "\"ssid\":\"%s\",\"ip\":\"%s\",\"ap\":%s,\"ap_ip\":\"%s\","
                    "\"ap_ssid\":\"%s\",\"ap_psk\":\"%s\","
                    "\"ap_clients\":%d,\"scanning\":%s,\"error\":\"%s\","
                    /* Enough of the layers below to tell, remotely, where a
                     * station that is not up is stuck: the driver's join
                     * state, the lwIP link, DHCP's state (lwip/prot/dhcp.h),
                     * and whether the join is waiting on Bluetooth. */
                    "\"radio_join\":\"0x%04lx\",\"link\":%s,\"dhcp\":%d,"
                    "\"join_deferred\":%s,\"fw_rejoins\":%lu,"
                    "\"bt_window\":\"%s\",\"bt_windows\":%lu,\"bt_windows_ok\":%lu,"
                    "\"bt_window_next_s\":%ld}",
                    driver_up ? "true" : "false", mode_name(), cfg_mode(),
                    cur_ssid, ip_str,
                    ap_up ? "true" : "false", ap_up ? "192.168.4.1" : "",
                    cfg_ap_ssid(), cfg_ap_psk(),
                    ap_clients_cached, scanning ? "true" : "false", last_error,
                    (unsigned long)cyw43_state.wifi_join_state,
                    netif_is_link_up(sta_netif()) ? "true" : "false",
                    netif_dhcp_data(sta_netif()) ? (int)netif_dhcp_data(sta_netif())->state : -1,
                    deferred_for_bt ? "true" : "false", (unsigned long)fw_rejoins,
#if CONFIG_BT_LE
                    win_start ? win_reason : "closed", (unsigned long)win_count, (unsigned long)win_ok,
                    win_not_before && (int32_t)(win_not_before - now_ms()) > 0
                        ? (long)((win_not_before - now_ms()) / 1000) : 0L
#else
                    "n/a", 0UL, 0UL, 0L
#endif
                    );
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
