#include "cfg_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "fs_lfs.h"

static const char *TAG = "cfg";
#define CFG_PATH "cfg/wifi.json"
#define CFG_BUF 1536

typedef struct { char ssid[CFG_SSID_MAX + 1]; char psk[CFG_PSK_MAX + 1]; } net_t;
static net_t nets[CFG_MAX_NET];
static int net_n;
static char ap_ssid[CFG_SSID_MAX + 1] = "BTGW-Setup";
static char ap_psk[CFG_PSK_MAX + 1] = "btgw-setup";
static char mode[8] = "auto";
static bool dirty;

static const char *json_str_from(const char *start, const char *key, char *out, size_t cap) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = start ? strstr(start, pat) : NULL;
    if (!p) return NULL;
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

static bool valid_mode(const char *m) {
    return m && (!strcmp(m, "auto") || !strcmp(m, "sta") || !strcmp(m, "ap") ||
                 !strcmp(m, "both") || !strcmp(m, "off"));
}
static bool valid_ssid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    return n > 0 && n <= CFG_SSID_MAX && !strchr(s, '"') && !strchr(s, '\\');
}
static bool valid_psk(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n > CFG_PSK_MAX || (n > 0 && n < 8)) return false;
    return !strchr(s, '"') && !strchr(s, '\\');
}

static bool persist(void) {
    char buf[CFG_BUF];
    size_t n = (size_t)snprintf(buf, sizeof buf,
        "{\"mode\":\"%s\",\"ap_ssid\":\"%s\",\"ap_psk\":\"%s\",\"nets\":[", mode, ap_ssid, ap_psk);
    for (int i = 0; i < net_n; i++) {
        int w = snprintf(buf + n, sizeof buf - n, "%s{\"ssid\":\"%s\",\"psk\":\"%s\"}",
                         i ? "," : "", nets[i].ssid, nets[i].psk);
        if (w < 0 || n + (size_t)w >= sizeof buf - 4) return false;
        n += (size_t)w;
    }
    int w = snprintf(buf + n, sizeof buf - n, "]}");
    if (w < 0 || n + (size_t)w >= sizeof buf) return false;
    n += (size_t)w;
    return fs_write_file(CFG_PATH, buf, n) == 0;
}

void cfg_load(void) {
    size_t got = 0;
    char buf[CFG_BUF];
    if (fs_read_file(CFG_PATH, buf, sizeof buf - 1, &got) != 0 || got == 0) {
        ESP_LOGI(TAG, "no cfg/wifi.json, mode=%s, no station", mode);
        return;
    }
    buf[got] = 0;
    char tmp[CFG_PSK_MAX + 1];
    if (json_str_from(buf, "mode", tmp, sizeof tmp) && valid_mode(tmp))
        snprintf(mode, sizeof mode, "%.*s", (int)sizeof mode - 1, tmp);
    if (json_str_from(buf, "ap_ssid", tmp, sizeof tmp) && valid_ssid(tmp))
        snprintf(ap_ssid, sizeof ap_ssid, "%.*s", (int)sizeof ap_ssid - 1, tmp);
    if (json_str_from(buf, "ap_psk", tmp, sizeof tmp) && valid_psk(tmp)) snprintf(ap_psk, sizeof ap_psk, "%s", tmp);
    const char *p = strstr(buf, "\"nets\"");
    net_n = 0;
    while (p && net_n < CFG_MAX_NET) {
        char ssid[CFG_SSID_MAX + 1], psk[CFG_PSK_MAX + 1] = "";
        const char *after = json_str_from(p, "ssid", ssid, sizeof ssid);
        if (!after) break;
        const char *after_psk = json_str_from(after, "psk", psk, sizeof psk);
        if (valid_ssid(ssid) && valid_psk(psk)) {
            snprintf(nets[net_n].ssid, sizeof nets[net_n].ssid, "%s", ssid);
            snprintf(nets[net_n].psk, sizeof nets[net_n].psk, "%s", psk);
            net_n++;
        }
        p = after_psk ? after_psk : after;
    }
    dirty = false;
    ESP_LOGI(TAG, "%d network(s), mode=%s, ap=%s", net_n, mode, ap_ssid);
}

void cfg_tick(void) {
    if (!dirty) return;
    if (persist()) dirty = false;
    else ESP_LOGW(TAG, "cfg/wifi.json write failed");
}

const char *cfg_mode(void) { return mode; }
const char *cfg_ap_ssid(void) { return ap_ssid; }
const char *cfg_ap_psk(void) { return ap_psk; }
int cfg_net_count(void) { return net_n; }

bool cfg_net_at(int i, char *ssid, size_t ssid_cap, char *psk, size_t psk_cap) {
    if (i < 0 || i >= net_n) return false;
    if (ssid && ssid_cap) snprintf(ssid, ssid_cap, "%s", nets[i].ssid);
    if (psk && psk_cap) snprintf(psk, psk_cap, "%s", nets[i].psk);
    return true;
}

bool cfg_set_mode(const char *m) {
    if (!valid_mode(m)) return false;
    if (!strcmp(mode, m)) return true;
    snprintf(mode, sizeof mode, "%s", m);
    dirty = true;
    return true;
}

bool cfg_set_ap(const char *ssid, const char *psk) {
    if (!valid_ssid(ssid)) return false;
    if (psk && !valid_psk(psk)) return false;
    snprintf(ap_ssid, sizeof ap_ssid, "%s", ssid);
    if (psk) snprintf(ap_psk, sizeof ap_psk, "%s", psk);
    dirty = true;
    return true;
}

bool cfg_net_upsert(const char *ssid, const char *psk) {
    if (!valid_ssid(ssid) || !valid_psk(psk ? psk : "")) return false;
    for (int i = 0; i < net_n; i++) {
        if (!strcmp(nets[i].ssid, ssid)) {
            snprintf(nets[i].psk, sizeof nets[i].psk, "%s", psk ? psk : "");
            dirty = true;
            return true;
        }
    }
    if (net_n >= CFG_MAX_NET) return false;
    snprintf(nets[net_n].ssid, sizeof nets[net_n].ssid, "%s", ssid);
    snprintf(nets[net_n].psk, sizeof nets[net_n].psk, "%s", psk ? psk : "");
    net_n++;
    dirty = true;
    return true;
}

bool cfg_net_forget(const char *ssid) {
    for (int i = 0; i < net_n; i++) {
        if (strcmp(nets[i].ssid, ssid)) continue;
        memmove(&nets[i], &nets[i + 1], (size_t)(net_n - i - 1) * sizeof nets[i]);
        net_n--;
        dirty = true;
        return true;
    }
    return false;
}

int cfg_networks_json(char *buf, size_t cap, const char *current_ssid) {
    size_t n = (size_t)snprintf(buf, cap, "{\"ok\":true,\"networks\":[");
    for (int i = 0; i < net_n && n + 160 < cap; i++) {
        n += (size_t)snprintf(buf + n, cap - n, "%s{\"ssid\":\"%s\",\"psk\":\"%s\",\"current\":%s}",
                              i ? "," : "", nets[i].ssid, nets[i].psk,
                              current_ssid && !strcmp(current_ssid, nets[i].ssid) ? "true" : "false");
    }
    n += (size_t)snprintf(buf + n, cap - n, "]}");
    return (int)n;
}
