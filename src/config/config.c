/* Persistent device configuration — see config.h for the deferred-write rule,
 * which is the only subtle thing in here.
 *
 * Stored as JSON under the reserved "cfg/" prefix. That prefix is refused by
 * the /api/fs routes (src/net/http.c) on purpose: the filesystem API serves
 * anything it is asked for, with no authentication and CORS wide open, so a
 * credentials file sitting in the same namespace as the web assets would be
 * readable by anyone who can reach the device. Once WiFi is up that is the
 * whole LAN. The WiFi API is then the only reader, which keeps the deliberate
 * choice to show passwords in the UI deliberate rather than accidental.
 *
 * Hand-rolled JSON, matching the rest of this codebase (src/net/http.c's
 * json_str_field, and the reference implementation's json_str_from) — there
 * is no JSON library here and a fixed-shape record does not need one. The
 * parser is positional and forgiving: anything it cannot make sense of leaves
 * the defaults in place rather than failing to boot.
 */
#include "config/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs/fs.h"

#define CFG_PATH "cfg/wifi.json"
#define CFG_NOTIFY_PATH "cfg/notify.json"

/* 8 networks at ~110 bytes each, plus AP credentials and the mode. The tail
 * margin below is what stops a truncated write from producing a file that
 * parses as valid-but-short. */
#define CFG_BUF 1536
#define CFG_TAIL_MARGIN 96

static cfg_net_t nets[CFG_MAX_NET];
static int net_n = 0;
static char ap_ssid[CFG_SSID_MAX + 1] = "BTGW-Setup";
static char ap_psk[CFG_PSK_MAX + 1] = "btgw-setup";
static char mode[8] = "auto";
static bool dirty = false;
static bool loaded = false;

/* Defaults are what a first-time user most plausibly wants: current falling
 * under 100mA for five seconds means the load went away. Off until they say
 * otherwise, and with no transport configured it could not deliver anyway. */
#define TRIG_DEFAULT {.serial = "", .enabled = false, .source = "current", .dir = "below", .threshold = 100, \
                      .hysteresis = 20, .dwell_ms = 5000}
static cfg_notify_t notify = {
    .trig = {TRIG_DEFAULT, TRIG_DEFAULT, TRIG_DEFAULT, TRIG_DEFAULT},
    .name = "km003c",
    .ntfy_url = "",
    .mqtt_host = "",
    .mqtt_port = 1883,
    .mqtt_topic = "km003c/alert",
    .mqtt_user = "",
    .mqtt_pass = "",
};
static bool notify_dirty = false;

static void load_notify(void);
static void persist_notify(void);

/* --- tiny JSON scanning ---------------------------------------------------
 *
 * Finds "key" AFTER `from`, then copies the quoted value. Positional by
 * design: the writer below emits a fixed shape, so "find ssid, then find the
 * password after it, repeat" is enough and needs no object model. */
static const char *json_str_from(const char *start, const char *key, char *out, size_t cap) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = start ? strstr(start, pat) : NULL;
    if (!p) {
        return NULL;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < cap) n++;
    if (p[n] != '"') return NULL;
    memcpy(out, p, n);
    out[n] = 0;
    return p + n + 1;
}

/* Same positional contract as json_str_from(), for a bare integer. Accepts a
 * leading minus; anything else non-numeric ends the number. */
static const char *json_int_from(const char *start, const char *key, long *out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = start ? strstr(start, pat) : NULL;
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) return NULL;
    *out = v;
    return end;
}

/* Everything the hand-written JSON writer cannot escape, plus control
 * characters, which have no business in a hostname or a topic. */
static bool plain_text(const char *s, size_t max) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n > max) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\' || c < 0x20) return false;
    }
    return true;
}

static bool valid_ssid(const char *s) {
    if (!s || !s[0]) return false;
    size_t n = strlen(s);
    if (n > CFG_SSID_MAX) return false;
    /* A quote or backslash would break the hand-written JSON both here and in
     * the HTTP responses, neither of which escapes. Rejecting is honest;
     * silently mangling the SSID would produce a network that can never be
     * joined and no clue why. */
    return strchr(s, '"') == NULL && strchr(s, '\\') == NULL;
}

static bool valid_mode(const char *m) {
    /* "off" was advertised by wifi.mode's own error message and by the UI but
     * was rejected here and handled nowhere, so the one mode that frees the
     * shared radio for Bluetooth did not exist. */
    return m && (strcmp(m, "auto") == 0 || strcmp(m, "sta") == 0 ||
                 strcmp(m, "ap") == 0   || strcmp(m, "off") == 0);
}

static bool valid_psk(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n > CFG_PSK_MAX) return false;
    if (n > 0 && n < 8) return false; /* WPA2 minimum; "" means an open network */
    return strchr(s, '"') == NULL && strchr(s, '\\') == NULL;
}

/* --- load / store --------------------------------------------------------- */

static void persist(void) {
    static char buf[CFG_BUF];
    size_t n = 0;
    n += (size_t)snprintf(buf + n, sizeof(buf) - n,
                          "{\"mode\":\"%s\",\"ap_ssid\":\"%s\",\"ap_psk\":\"%s\",\"nets\":[",
                          mode, ap_ssid, ap_psk);
    for (int i = 0; i < net_n; i++) {
        if (n + CFG_TAIL_MARGIN >= sizeof(buf)) {
            break; /* refuse to emit a half-written entry */
        }
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s{\"ssid\":\"%s\",\"psk\":\"%s\"}",
                              i ? "," : "", nets[i].ssid, nets[i].psk);
    }
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "]}");
    if (n >= sizeof(buf)) {
        printf("cfg: record too large, not written\n");
        return;
    }

    if (fs_begin_write(CFG_PATH) != 0) {
        printf("cfg: write busy\n");
        return;
    }
    if (fs_write((const uint8_t *)buf, n) != 0) {
        fs_abort_write();
        printf("cfg: write failed\n");
        return;
    }
    if (fs_end_write() != 0) {
        printf("cfg: close failed\n");
    }
}

void cfg_load(void) {
    loaded = true;
    size_t size = 0;
    if (!fs_stat(CFG_PATH, &size) || size == 0 || size >= CFG_BUF) {
        printf("cfg: no stored config, using defaults\n");
        load_notify();
        return;
    }
    int h = fs_begin_read(CFG_PATH, NULL);
    if (h < 0) {
        printf("cfg: unreadable, using defaults\n");
        load_notify();
        return;
    }
    static char buf[CFG_BUF];
    int got = fs_read(h, (uint8_t *)buf, sizeof(buf) - 1);
    fs_end_read(h);
    if (got <= 0) {
        load_notify();
        return;
    }
    buf[got] = 0;

    char tmp[CFG_PSK_MAX + 1];
    if (json_str_from(buf, "mode", tmp, sizeof(tmp)) && valid_mode(tmp)) {
        /* NOT cfg_set_mode(): that marks the store dirty, and a dirty store is
         * written back by the next cfg_tick(). Loading would then rewrite an
         * identical file on every single boot. Harmless-looking, and on the
         * board with KM_FLASH_RADIO_GUARD it is not harmless at all — a write
         * there takes the radio down until the next reset, so the device came
         * up, wrote the config it had just read, and killed its own WiFi
         * before it finished booting. */
        snprintf(mode, sizeof(mode), "%s", tmp);
    }
    if (json_str_from(buf, "ap_ssid", tmp, sizeof(tmp)) && valid_ssid(tmp)) {
        snprintf(ap_ssid, sizeof(ap_ssid), "%s", tmp);
    }
    if (json_str_from(buf, "ap_psk", tmp, sizeof(tmp)) && valid_psk(tmp)) {
        snprintf(ap_psk, sizeof(ap_psk), "%s", tmp);
    }

    /* Walk the array positionally: each ssid, then the psk that follows it. */
    const char *p = strstr(buf, "\"nets\"");
    net_n = 0;
    while (p && net_n < CFG_MAX_NET) {
        char ssid[CFG_SSID_MAX + 1];
        const char *after = json_str_from(p, "ssid", ssid, sizeof(ssid));
        if (!after) break;
        char psk[CFG_PSK_MAX + 1] = "";
        const char *after_psk = json_str_from(after, "psk", psk, sizeof(psk));
        if (valid_ssid(ssid) && valid_psk(psk)) {
            snprintf(nets[net_n].ssid, sizeof(nets[net_n].ssid), "%s", ssid);
            snprintf(nets[net_n].psk, sizeof(nets[net_n].psk), "%s", psk);
            net_n++;
        }
        p = after_psk ? after_psk : after;
    }
    /* dirty stays false: loading is not a change, and marking it would write
     * flash on every boot for nothing. */
    printf("cfg: %d network(s), mode=%s, ap=%s\n", net_n, mode, ap_ssid);
    load_notify();
}

/* Puts the build's KM_WIFI_SSID into an empty store, IN RAM ONLY.
 *
 * Bring-up convenience, not a feature: without it a freshly-flashed board
 * knows no networks, so the only way in is USB-NCM — which is fine as a
 * workflow but tedious as the first step of every test cycle.
 *
 * Deliberately does not set dirty, for two reasons. A compile-time credential
 * that silently wrote itself to flash would outlive the build that introduced
 * it and be a puzzle to find later. And on the guarded board a write here
 * happens during boot, which is the worst moment for it. Anything saved
 * through the API persists normally and takes precedence, because this only
 * runs when the store came up empty. */
void cfg_seed_defaults(void) {
#if defined(KM_WIFI_SSID)
    if (net_n != 0 || !KM_WIFI_SSID[0]) {
        return;
    }
    if (valid_ssid(KM_WIFI_SSID) && valid_psk(KM_WIFI_PASSWORD)) {
        snprintf(nets[0].ssid, sizeof(nets[0].ssid), "%s", KM_WIFI_SSID);
        snprintf(nets[0].psk, sizeof(nets[0].psk), "%s", KM_WIFI_PASSWORD);
        net_n = 1;
        printf("cfg: seeded '%s' from the build (not saved)\n", nets[0].ssid);
    }
#endif
}

void cfg_tick(void) {
    if (!loaded) {
        return;
    }
    /* One write per tick: two dirty records mean two erases, and spacing
     * them out costs nothing here. */
    if (dirty) {
        dirty = false;
        persist();
    } else if (notify_dirty) {
        notify_dirty = false;
        persist_notify();
    }
}

/* --- notifications --------------------------------------------------------- */

/* An entry worth writing: bound to a meter, or the legacy entry when it
 * says something other than the default. */
static bool trig_in_use(const cfg_trigger_t *t, int i) {
    return t->serial[0] || t->enabled || i == 0;
}

static void persist_notify(void) {
    static char buf[CFG_BUF];
    int n = snprintf(buf, sizeof(buf),
                     "{\"name\":\"%s\",\"ntfy_url\":\"%s\","
                     "\"mqtt_host\":\"%s\",\"mqtt_port\":%u,\"mqtt_topic\":\"%s\","
                     "\"mqtt_user\":\"%s\",\"mqtt_pass\":\"%s\",\"triggers\":[",
                     notify.name, notify.ntfy_url, notify.mqtt_host, (unsigned)notify.mqtt_port,
                     notify.mqtt_topic, notify.mqtt_user, notify.mqtt_pass);
    bool first = true;
    for (int i = 0; i < CFG_TRIGGERS && n > 0 && (size_t)n < sizeof(buf); i++) {
        const cfg_trigger_t *t = &notify.trig[i];
        if (!trig_in_use(t, i)) {
            continue;
        }
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "%s{\"serial\":\"%s\",\"enabled\":%s,\"source\":\"%s\",\"dir\":\"%s\","
                      "\"threshold\":%ld,\"hysteresis\":%ld,\"dwell_ms\":%lu}",
                      first ? "" : ",", t->serial, t->enabled ? "true" : "false", t->source, t->dir,
                      (long)t->threshold, (long)t->hysteresis, (unsigned long)t->dwell_ms);
        first = false;
    }
    if (n > 0 && (size_t)n < sizeof(buf)) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "]}");
    }
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        printf("cfg: notify record too large, not written\n");
        return;
    }
    if (fs_begin_write(CFG_NOTIFY_PATH) != 0) {
        printf("cfg: notify write busy\n");
        return;
    }
    if (fs_write((const uint8_t *)buf, (size_t)n) != 0) {
        fs_abort_write();
        printf("cfg: notify write failed\n");
        return;
    }
    if (fs_end_write() != 0) {
        printf("cfg: notify close failed\n");
    }
}

/* One trigger's fields out of `obj` (a JSON object's text) into `t`. */
static void parse_trigger(const char *obj, cfg_trigger_t *t) {
    char tmp[CFG_URL_MAX + 1];
    long v;
    const char *p = strstr(obj, "\"enabled\"");
    if (p) {
        p += 9;
        while (*p == ' ' || *p == ':' || *p == '\t') p++;
        t->enabled = strncmp(p, "true", 4) == 0;
    }
    if (json_str_from(obj, "serial", tmp, sizeof(tmp))) snprintf(t->serial, sizeof(t->serial), "%s", tmp);
    if (json_str_from(obj, "source", tmp, sizeof(tmp))) snprintf(t->source, sizeof(t->source), "%s", tmp);
    if (json_str_from(obj, "dir", tmp, sizeof(tmp))) snprintf(t->dir, sizeof(t->dir), "%s", tmp);
    if (json_int_from(obj, "threshold", &v)) t->threshold = (int32_t)v;
    if (json_int_from(obj, "hysteresis", &v)) t->hysteresis = (int32_t)v;
    if (json_int_from(obj, "dwell_ms", &v)) t->dwell_ms = (uint32_t)v;
}

static void load_notify(void) {
    size_t size = 0;
    if (!fs_stat(CFG_NOTIFY_PATH, &size) || size == 0 || size >= CFG_BUF) {
        return; /* defaults stand */
    }
    int h = fs_begin_read(CFG_NOTIFY_PATH, NULL);
    if (h < 0) return;
    static char buf[CFG_BUF];
    int got = fs_read(h, (uint8_t *)buf, sizeof(buf) - 1);
    fs_end_read(h);
    if (got <= 0) return;
    buf[got] = 0;

    /* Parse into a copy and validate it as one record, so a corrupt file
     * leaves the defaults intact rather than half-applying. */
    cfg_notify_t n = notify;
    char tmp[CFG_URL_MAX + 1];
    long v;
    if (json_str_from(buf, "name", tmp, sizeof(tmp))) snprintf(n.name, sizeof(n.name), "%s", tmp);
    if (json_str_from(buf, "ntfy_url", tmp, sizeof(tmp))) snprintf(n.ntfy_url, sizeof(n.ntfy_url), "%s", tmp);
    if (json_str_from(buf, "mqtt_host", tmp, sizeof(tmp))) snprintf(n.mqtt_host, sizeof(n.mqtt_host), "%s", tmp);
    if (json_int_from(buf, "mqtt_port", &v)) n.mqtt_port = (uint16_t)v;
    if (json_str_from(buf, "mqtt_topic", tmp, sizeof(tmp))) snprintf(n.mqtt_topic, sizeof(n.mqtt_topic), "%s", tmp);
    if (json_str_from(buf, "mqtt_user", tmp, sizeof(tmp))) snprintf(n.mqtt_user, sizeof(n.mqtt_user), "%s", tmp);
    if (json_str_from(buf, "mqtt_pass", tmp, sizeof(tmp))) snprintf(n.mqtt_pass, sizeof(n.mqtt_pass), "%s", tmp);

    const char *arr = strstr(buf, "\"triggers\"");
    if (arr) {
        /* The current shape: one object per trigger. Objects are flat, so
         * the next '}' closes the one the '{' opened. */
        const char *p = arr;
        int i = 0;
        while (i < CFG_TRIGGERS && (p = strchr(p, '{')) != NULL) {
            const char *e = strchr(p, '}');
            if (!e) break;
            static char obj[256];
            size_t len = (size_t)(e - p + 1);
            if (len >= sizeof(obj)) len = sizeof(obj) - 1;
            memcpy(obj, p, len);
            obj[len] = 0;
            n.trig[i] = (cfg_trigger_t)TRIG_DEFAULT;
            parse_trigger(obj, &n.trig[i]);
            i++;
            p = e + 1;
        }
    } else {
        /* The record as written before there was more than one meter: the
         * trigger's fields at the top level. It becomes the legacy entry,
         * bound to the default meter. */
        parse_trigger(buf, &n.trig[0]);
        n.trig[0].serial[0] = 0;
    }

    const char *why = NULL;
    bool was_dirty = notify_dirty;
    if (!cfg_set_notify(&n, &why)) {
        printf("cfg: stored notify config rejected (%s), using defaults\n", why ? why : "?");
    }
    /* Loading is not a change — same rule as the WiFi record. */
    notify_dirty = was_dirty;
    printf("cfg: notify %s, %s %s %ld, ntfy=%s mqtt=%s\n", notify.trig[0].enabled ? "on" : "off", notify.trig[0].source,
           notify.trig[0].dir, (long)notify.trig[0].threshold, notify.ntfy_url[0] ? "set" : "-",
           notify.mqtt_host[0] ? "set" : "-");
}

const cfg_notify_t *cfg_notify(void) {
    return &notify;
}

static const char *trigger_invalid(const cfg_trigger_t *t) {
    if (strcmp(t->source, "current") != 0 && strcmp(t->source, "power") != 0 && strcmp(t->source, "voltage") != 0) {
        return "source";
    }
    if (strcmp(t->dir, "below") != 0 && strcmp(t->dir, "above") != 0) {
        return "dir";
    }
    if (t->threshold < 0 || t->threshold > 1000000) {
        return "threshold";
    }
    if (t->hysteresis < 0 || t->hysteresis > 1000000) {
        return "hysteresis";
    }
    if (t->dwell_ms > 3600000u) {
        return "dwell_ms";
    }
    if (!plain_text(t->serial, CFG_SERIAL_MAX)) {
        return "serial";
    }
    return NULL;
}

cfg_trigger_t *cfg_notify_trigger_for(cfg_notify_t *n, const char *serial) {
    if (!serial || !serial[0]) {
        return &n->trig[0].serial[0] ? NULL : &n->trig[0];
    }
    for (int i = 0; i < CFG_TRIGGERS; i++) {
        if (strcmp(n->trig[i].serial, serial) == 0) {
            return &n->trig[i];
        }
    }
    /* Entry 0 is the legacy record while its serial is empty; a bound
     * meter takes the first entry after it that nobody holds. */
    for (int i = 1; i < CFG_TRIGGERS; i++) {
        if (!n->trig[i].serial[0] && !n->trig[i].enabled) {
            return &n->trig[i];
        }
    }
    return NULL;
}

bool cfg_set_notify(const cfg_notify_t *n, const char **why) {
    const char *reason = NULL;
    for (int i = 0; i < CFG_TRIGGERS && !reason; i++) {
        reason = trigger_invalid(&n->trig[i]);
    }
    if (reason) {
        /* fall through to the report below */
    } else if (!plain_text(n->name, CFG_NAME_MAX) || !n->name[0]) {
        reason = "name";
    } else if (!plain_text(n->ntfy_url, CFG_URL_MAX)) {
        reason = "ntfy_url";
    } else if (n->ntfy_url[0] && strncmp(n->ntfy_url, "http://", 7) != 0) {
        /* Only http:// — this build has no TLS (see KM_TLS in CMakeLists.txt),
         * and silently sending an https:// URL over plaintext would be the
         * worst possible way to find that out. */
        reason = "ntfy_url must start with http://";
    } else if (!plain_text(n->mqtt_host, CFG_HOST_MAX)) {
        reason = "mqtt_host";
    } else if (n->mqtt_port == 0) {
        reason = "mqtt_port";
    } else if (!plain_text(n->mqtt_topic, CFG_TOPIC_MAX) || (n->mqtt_host[0] && !n->mqtt_topic[0])) {
        reason = "mqtt_topic";
    } else if (!plain_text(n->mqtt_user, CFG_USER_MAX) || !plain_text(n->mqtt_pass, CFG_PASS_MAX)) {
        reason = "mqtt credentials";
    }
    if (reason) {
        if (why) *why = reason;
        return false;
    }
    if (memcmp(&notify, n, sizeof(notify)) != 0) {
        notify = *n;
        notify_dirty = true;
    }
    return true;
}

/* --- accessors ------------------------------------------------------------ */

int cfg_net_count(void) {
    return net_n;
}

const cfg_net_t *cfg_net_at(int i) {
    return (i >= 0 && i < net_n) ? &nets[i] : NULL;
}

const cfg_net_t *cfg_net_find(const char *ssid) {
    if (!ssid) return NULL;
    for (int i = 0; i < net_n; i++) {
        if (strcmp(nets[i].ssid, ssid) == 0) {
            return &nets[i];
        }
    }
    return NULL;
}

bool cfg_net_put(const char *ssid, const char *psk) {
    if (!valid_ssid(ssid)) return false;
    if (psk && !valid_psk(psk)) return false;

    for (int i = 0; i < net_n; i++) {
        if (strcmp(nets[i].ssid, ssid) == 0) {
            /* NULL psk = leave the stored password alone; see config.h. */
            if (psk && strcmp(nets[i].psk, psk) != 0) {
                snprintf(nets[i].psk, sizeof(nets[i].psk), "%s", psk);
                dirty = true;
            }
            return true;
        }
    }
    if (net_n >= CFG_MAX_NET) {
        return false;
    }
    snprintf(nets[net_n].ssid, sizeof(nets[net_n].ssid), "%s", ssid);
    snprintf(nets[net_n].psk, sizeof(nets[net_n].psk), "%s", psk ? psk : "");
    net_n++;
    dirty = true;
    return true;
}

bool cfg_net_forget(const char *ssid) {
    if (!ssid) return false;
    for (int i = 0; i < net_n; i++) {
        if (strcmp(nets[i].ssid, ssid) == 0) {
            for (int j = i; j + 1 < net_n; j++) {
                nets[j] = nets[j + 1];
            }
            net_n--;
            dirty = true;
            return true;
        }
    }
    return false;
}

const char *cfg_ap_ssid(void) {
    return ap_ssid;
}

const char *cfg_ap_psk(void) {
    return ap_psk;
}

bool cfg_set_ap(const char *ssid, const char *psk) {
    /* Validate both before changing either, so a bad PSK cannot leave the AP
     * renamed. Unchanged values write nothing — see cfg_set_mode(). */
    if (ssid && !valid_ssid(ssid)) return false;
    if (psk && !valid_psk(psk)) return false;
    if (ssid && strcmp(ap_ssid, ssid) != 0) {
        snprintf(ap_ssid, sizeof(ap_ssid), "%s", ssid);
        dirty = true;
    }
    if (psk && strcmp(ap_psk, psk) != 0) {
        snprintf(ap_psk, sizeof(ap_psk), "%s", psk);
        dirty = true;
    }
    return true;
}

const char *cfg_mode(void) {
    return mode;
}

bool cfg_set_mode(const char *m) {
    if (!valid_mode(m)) return false;
    if (strcmp(mode, m) == 0) {
        /* No change, so no write. Every avoided write is an avoided radio
         * outage on the guarded board, and the UI PUTs the whole object. */
        return true;
    }
    snprintf(mode, sizeof(mode), "%s", m);
    dirty = true;
    return true;
}
