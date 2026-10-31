#include "wifi_sta.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "cfg_wifi.h"

static const char *TAG = "wifi";
static bool sta_up, ap_up, scanning, have_ip;
static char cur_ssid[CFG_SSID_MAX + 1];
static char ip_str[16];
static char err_str[48];
static int scan_n;
typedef struct { char ssid[33]; int8_t rssi; uint8_t chan; uint8_t auth; } ap_t;
static ap_t scan_aps[16];

static bool want_sta(void) {
    const char *m = cfg_mode();
    if (!strcmp(m, "off") || !strcmp(m, "ap")) return false;
    if (!strcmp(m, "auto")) return cfg_net_count() > 0;
    return cfg_net_count() > 0;
}
static bool want_ap(void) {
    const char *m = cfg_mode();
    if (!strcmp(m, "off") || !strcmp(m, "sta")) return false;
    if (!strcmp(m, "auto")) return cfg_net_count() == 0;
    return true;
}

static const char *auth_name(uint8_t a) {
    switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    default: return "other";
    }
}

static char prefer[CFG_SSID_MAX + 1];

static void join_current(void) {
    if (!want_sta() || cfg_net_count() < 1) return;
    char ssid[CFG_SSID_MAX + 1], psk[CFG_PSK_MAX + 1];
    int pick = 0;
    if (prefer[0]) {
        for (int i = 0; i < cfg_net_count(); i++) {
            if (cfg_net_at(i, ssid, sizeof ssid, NULL, 0) && !strcmp(ssid, prefer)) { pick = i; break; }
        }
    }
    if (!cfg_net_at(pick, ssid, sizeof ssid, psk, sizeof psk)) return;
    wifi_config_t sta = { 0 };
    memcpy(sta.sta.ssid, ssid, strlen(ssid));
    memcpy(sta.sta.password, psk, strlen(psk));
    sta.sta.threshold.authmode = psk[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    snprintf(cur_ssid, sizeof cur_ssid, "%s", ssid);
    ESP_LOGI(TAG, "joining '%s'", ssid);
    esp_wifi_connect();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        join_current();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        have_ip = false;
        ip_str[0] = 0;
        wifi_event_sta_disconnected_t *ev = data;
        snprintf(err_str, sizeof err_str, "disconnect %u", ev ? (unsigned)ev->reason : 0);
        ESP_LOGW(TAG, "%s", err_str);
        if (want_sta()) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = 16;
        wifi_ap_record_t rec[16];
        if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) n = 0;
        scan_n = n;
        for (int i = 0; i < scan_n; i++) {
            snprintf(scan_aps[i].ssid, sizeof scan_aps[i].ssid, "%s", (char *)rec[i].ssid);
            scan_aps[i].rssi = rec[i].rssi;
            scan_aps[i].chan = rec[i].primary;
            scan_aps[i].auth = rec[i].authmode;
        }
        scanning = false;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        snprintf(ip_str, sizeof ip_str, IPSTR, IP2STR(&ev->ip_info.ip));
        have_ip = true;
        err_str[0] = 0;
        ESP_LOGI(TAG, "address %s", ip_str);
    }
}

static void apply_radio(void) {
    bool st = want_sta(), ap = want_ap();
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (st && ap) mode = WIFI_MODE_APSTA;
    else if (st) mode = WIFI_MODE_STA;
    else if (ap) mode = WIFI_MODE_AP;
    esp_wifi_set_mode(mode);
    if (ap) {
        wifi_config_t ac = { 0 };
        const char *s = cfg_ap_ssid(), *p = cfg_ap_psk();
        memcpy(ac.ap.ssid, s, strlen(s));
        ac.ap.ssid_len = strlen(s);
        memcpy(ac.ap.password, p, strlen(p));
        ac.ap.authmode = p[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ac.ap.max_connection = 4;
        ac.ap.channel = 6;
        esp_wifi_set_config(WIFI_IF_AP, &ac);
        ap_up = true;
        ESP_LOGI(TAG, "softap '%s'", s);
    } else {
        ap_up = false;
    }
    sta_up = st;
    if (!st) { have_ip = false; ip_str[0] = 0; cur_ssid[0] = 0; }
}

void wifi_start_from_cfg(void) {
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    apply_radio();
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "mode %s", cfg_mode());
}

void wifi_apply_cfg(void) {
    apply_radio();
    if (want_sta()) join_current();
}

bool wifi_has_addr(void) { return have_ip; }
int wifi_ip(char *dst, size_t cap) { return snprintf(dst, cap, "%s", have_ip ? ip_str : ""); }
const char *wifi_current_ssid(void) { return cur_ssid; }
bool wifi_scanning(void) { return scanning; }

bool wifi_join(const char *ssid, const char *psk_or_null) {
    if (!ssid || !ssid[0]) return false;
    char psk[CFG_PSK_MAX + 1] = "";
    if (psk_or_null) {
        snprintf(psk, sizeof psk, "%s", psk_or_null);
    } else {
        bool found = false;
        for (int i = 0; i < cfg_net_count(); i++) {
            char s[CFG_SSID_MAX + 1];
            if (cfg_net_at(i, s, sizeof s, psk, sizeof psk) && !strcmp(s, ssid)) { found = true; break; }
        }
        if (!found) return false;
    }
    if (!cfg_net_upsert(ssid, psk)) return false;
    snprintf(prefer, sizeof prefer, "%s", ssid);
    const char *m = cfg_mode();
    if (!strcmp(m, "off") || !strcmp(m, "ap")) cfg_set_mode("sta");
    apply_radio();
    wifi_config_t sta = { 0 };
    memcpy(sta.sta.ssid, ssid, strlen(ssid));
    memcpy(sta.sta.password, psk, strlen(psk));
    sta.sta.threshold.authmode = psk[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    snprintf(cur_ssid, sizeof cur_ssid, "%s", ssid);
    ESP_LOGI(TAG, "joining '%s'", ssid);
    return esp_wifi_connect() == ESP_OK;
}

bool wifi_scan_start(void) {
    if (scanning) return false;
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) return false;
    scanning = true;
    return true;
}

int wifi_status_json(char *buf, size_t cap) {
    return snprintf(buf, cap,
        "{\"ok\":true,\"driver\":true,\"mode\":\"%s\",\"want\":\"%s\","
        "\"ssid\":\"%s\",\"ip\":\"%s\",\"ap\":%s,\"ap_ip\":\"%s\","
        "\"ap_ssid\":\"%s\",\"ap_psk\":\"%s\",\"ap_clients\":0,"
        "\"scanning\":%s,\"error\":\"%s\","
        "\"radio_join\":\"0x0\",\"link\":%s,\"dhcp\":%d,"
        "\"join_deferred\":false,\"fw_rejoins\":0}",
        sta_up && ap_up ? "apsta" : sta_up ? "sta" : ap_up ? "ap" : "off",
        cfg_mode(), cur_ssid, ip_str,
        ap_up ? "true" : "false", ap_up ? "192.168.4.1" : "",
        cfg_ap_ssid(), cfg_ap_psk(),
        scanning ? "true" : "false", err_str,
        have_ip ? "true" : "false", have_ip ? 10 : 0);
}

int wifi_scan_json(char *buf, size_t cap) {
    size_t n = (size_t)snprintf(buf, cap, "{\"ok\":true,\"scanning\":%s,\"aps\":[", scanning ? "true" : "false");
    for (int i = 0; i < scan_n && n + 120 < cap; i++) {
        bool known = false;
        for (int k = 0; k < cfg_net_count(); k++) {
            char ssid[CFG_SSID_MAX + 1];
            if (cfg_net_at(k, ssid, sizeof ssid, NULL, 0) && !strcmp(ssid, scan_aps[i].ssid)) known = true;
        }
        n += (size_t)snprintf(buf + n, cap - n,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"chan\":%u,\"auth\":\"%s\",\"known\":%s}",
            i ? "," : "", scan_aps[i].ssid, scan_aps[i].rssi, scan_aps[i].chan,
            auth_name(scan_aps[i].auth), known ? "true" : "false");
    }
    n += (size_t)snprintf(buf + n, cap - n, "]}");
    return (int)n;
}
