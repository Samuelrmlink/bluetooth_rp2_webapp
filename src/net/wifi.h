/* WiFi: station mode with a SoftAP fallback, backed by a LittleFS credential
 * store (src/config/config.c). Compiled only when CONFIG_NET_WIFI is on.
 *
 * The contract every caller has to respect: NOTHING here touches the radio
 * synchronously. Every entry point sets a pending flag that wifi_poll() drains
 * from the main loop. These are all reached from lwIP receive callbacks, and
 * restarting a radio — or writing flash — from inside one is how you get a
 * lockup that looks like a hardware fault. So a "start scan" call returns
 * immediately having started nothing; ask again via the status/scan readers.
 */
#ifndef KM003C_NET_WIFI_H
#define KM003C_NET_WIFI_H

#include <stdbool.h>
#include <stddef.h>

/* Brings up the cyw43 driver and lwIP. MUST run before http_server_start(),
 * mdns_start() and usb_ncm_init(), because cyw43_arch_init() is what
 * initialises lwIP here and all three call into it.
 *
 * Deliberately does NOT join or start an AP — that is wifi_poll()'s job, so
 * the servers are up and able to report before the radio does anything that
 * can fail. Returns false if the module didn't answer, in which case the rest
 * of the firmware carries on over USB-NCM exactly as it does with WiFi off. */
bool wifi_init(void);

/* Pump from the main loop. Drives the cyw43 async context (which also runs
 * lwIP's timers in poll mode, standing in for sys_check_timeouts()) and
 * advances the state machine. */
void wifi_poll(void);

bool wifi_is_up(void);
const char *wifi_ip(void);

/* {"ok":true,"mode":...,"ip":...,...} — the single status object the UI
 * polls. Returns the length written, like diag_report(). */
int wifi_status_json(char *buf, size_t cap);

/* Requests a scan; results arrive asynchronously. Returns false if one is
 * already running or the radio isn't up. */
bool wifi_scan_start(void);
bool wifi_scanning(void);
int wifi_scan_json(char *buf, size_t cap);

/* The stored networks, PASSWORDS INCLUDED. Deliberate: the whole point of
 * keeping credentials on the device is being able to see and edit them from
 * the UI. It is also why cfg/ is refused by the filesystem routes — this
 * endpoint is meant to be the only way they come back out. */
int wifi_networks_json(char *buf, size_t cap);

/* Saves the network (psk NULL = keep whatever is stored) and asks to join it.
 * Returns false only for input the store rejects. */
bool wifi_connect(const char *ssid, const char *psk);

/* Re-reads cfg_mode() and moves to the arrangement it asks for. Call after
 * changing the mode or the AP credentials. */
void wifi_apply_mode(void);

/* Bluetooth coexistence for joins (saved in cfg/wifi.json, default "window"):
 *   window  a join under open BLE links takes a bounded radio window (links closed)
 *   live    join with the BLE links kept up (a BLE scan still makes it wait) */
const char *wifi_coex(void);
bool wifi_set_coex(const char *name);

/* True in mode "off": the WiFi side of the shared radio is down on purpose,
 * so the CYW43439 is Bluetooth's alone. The station/AP state machine must not
 * run in this mode -- its idle rescan would put the radio back on the air. */
bool wifi_is_off(void);

/* Is the SoftAP up right now? Independent of the station's state: in auto and
 * both the two coexist, so neither mode name nor wifi_is_up() answers this. */
bool wifi_ap_up(void);

/* True while Bluetooth should defer scanning/connecting so WiFi can bring the
 * shared radio up first (see wifi.c). False once WiFi is up, past the grace
 * window, or in mode "off". */
bool wifi_bt_hold(void);

/* For uplink selection (src/net/uplink.c), which owns the default route. */
struct netif;
struct netif *wifi_uplink_netif(void);    /* the station, once up; else NULL */
struct netif *wifi_fallback_netif(void);  /* the station while enabled; else NULL */

#endif /* KM003C_NET_WIFI_H */
