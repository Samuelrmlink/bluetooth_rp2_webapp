/* Device-side persistent configuration, stored in LittleFS.
 *
 * The first thing in this firmware to remember anything across a power cycle.
 * Everything the device "knew" before this lived in the browser (cookies, see
 * web/js/prefs.js) — fine for a layout preference, useless for credentials the
 * device itself needs before any browser is attached.
 *
 * Writes are DEFERRED. Every setter marks the store dirty and returns; the
 * actual flash write happens in cfg_tick() from the main loop. That is not an
 * optimisation: fs.c writes via flash_safe_execute(), which parks core1 and
 * takes hundreds of milliseconds, and calling it from an lwIP receive callback
 * (which is where every one of these setters is reached from) would stall the
 * TCP stack mid-request at best.
 */
#ifndef KM003C_CONFIG_H
#define KM003C_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Matching the reference implementation and the WPA2 limits: an SSID is at
 * most 32 bytes and a PSK at most 63 (a 64-character entry is a raw PMK,
 * which the cyw43 driver does not take). */
#define CFG_MAX_NET  8
#define CFG_SSID_MAX 32
#define CFG_PSK_MAX  63

typedef struct {
    char ssid[CFG_SSID_MAX + 1];
    char psk[CFG_PSK_MAX + 1];
} cfg_net_t;

/* Reads the store off LittleFS. Safe to call with no filesystem — everything
 * falls back to defaults and stays in RAM. */
void cfg_load(void);

/* Pump from the main loop. Flushes a dirty store to flash. */
void cfg_tick(void);

/* Call after cfg_load(). If the store came up with no networks and the build
 * baked in KM_WIFI_SSID, adds it in RAM so a freshly-flashed board can reach
 * the network without an API round-trip over USB first. Never written to
 * flash — see the definition. */
void cfg_seed_defaults(void);

int cfg_net_count(void);
const cfg_net_t *cfg_net_at(int i);
const cfg_net_t *cfg_net_find(const char *ssid);

/* Adds or updates. A NULL psk on an existing entry leaves its password alone
 * — that is what lets the UI show a network's stored password, let the user
 * edit only the SSID, and not have to send the password back. Returns false
 * if the store is full or the SSID is invalid. */
bool cfg_net_put(const char *ssid, const char *psk);
bool cfg_net_forget(const char *ssid);

const char *cfg_ap_ssid(void);
const char *cfg_ap_psk(void);
bool cfg_set_ap(const char *ssid, const char *psk);

/* "auto" (join a known network, fall back to SoftAP), "sta" or "ap". */
const char *cfg_mode(void);
bool cfg_set_mode(const char *mode);

/* How a WiFi join treats open Bluetooth links (cfg/wifi.json "coex"):
 * "window" (default) or "live". wifi.c reads it on every decision. */
const char *cfg_coex(void);
bool cfg_set_coex(const char *coex);

/* --- notifications (cfg/notify.json) --------------------------------------
 *
 * A second record in its own file, so a WiFi save never rewrites the
 * notification config and vice versa — each is a flash erase.
 *
 * Strings are sized for what they hold, not generously: this whole struct is
 * copied across cores through km_ipc, and the JSON form has to fit the HTTP
 * body buffer in one PUT. A URL of 96 covers "http://ntfy.sh/" plus a topic
 * of the maximum length ntfy allows (64). */
#define CFG_URL_MAX   96
#define CFG_HOST_MAX  64
#define CFG_TOPIC_MAX 64
#define CFG_USER_MAX  32
#define CFG_PASS_MAX  64
#define CFG_NAME_MAX  32

/* One threshold trigger, bound to a meter by serial. An empty serial is
 * the legacy record — the single trigger the firmware had before it drove
 * several meters — and binds to the default meter. */
/* Threshold triggers came from the meter project's notification feature, which
 * this firmware does not have. Kept at 1 so the record parser still round-trips
 * an old cfg file rather than choking on it. */
#define CFG_TRIGGERS 1
#define CFG_SERIAL_MAX 16
typedef struct {
    char serial[CFG_SERIAL_MAX + 1];
    bool enabled;
    char source[8];   /* "current" | "power" | "voltage" — see notify/trigger.h */
    char dir[6];      /* "below" | "above" */
    int32_t threshold;
    int32_t hysteresis;
    uint32_t dwell_ms;
} cfg_trigger_t;

typedef struct {
    cfg_trigger_t trig[CFG_TRIGGERS];
    char name[CFG_NAME_MAX + 1];       /* how the device names itself in a message */
    char ntfy_url[CFG_URL_MAX + 1];    /* full URL incl. topic; "" = ntfy off */
    char mqtt_host[CFG_HOST_MAX + 1];  /* "" = MQTT off */
    uint16_t mqtt_port;
    char mqtt_topic[CFG_TOPIC_MAX + 1];
    char mqtt_user[CFG_USER_MAX + 1];
    char mqtt_pass[CFG_PASS_MAX + 1];
} cfg_notify_t;

const cfg_notify_t *cfg_notify(void);

/* Validates and replaces the whole record. Returns false — changing nothing —
 * if any field is out of range, with `why` pointing at a static string
 * naming the field. Whole-record rather than per-field because the UI PUTs
 * the whole form, and half-applying a form that failed validation on its
 * last field is worse than rejecting it. No-op if nothing changed. */
bool cfg_set_notify(const cfg_notify_t *n, const char **why);

/* The trigger entry for `serial` (an existing binding, else the first
 * unused entry), or the legacy entry for "". NULL when all entries are
 * bound to other meters. */
cfg_trigger_t *cfg_notify_trigger_for(cfg_notify_t *n, const char *serial);

/* Whole-file read/write of a small record (cfg/eth.json, cfg/uplink.json).
 * Write only from the main loop; false = busy or failed, retry next tick. */
bool cfg_file_read(const char *path, char *buf, size_t cap);
bool cfg_file_write(const char *path, const char *data);

#endif /* KM003C_CONFIG_H */
