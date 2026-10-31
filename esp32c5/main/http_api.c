/* Websocket and filesystem HTTP for a board that does not serve web assets.
 * The envelope matches the RP2350 API: {"id","op","params"} answers
 * {"id","ok","result"} or {"id","ok":false,"error":{"code","message"}}.
 * Files go over /api/fs, the same routes the editor already uses. */
#include "http_api.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cfg_wifi.h"
#include "fs_lfs.h"
#include "mdns_adv.h"
#include "pex_host.h"
#include "wifi_sta.h"

static const char *TAG = "http";
static httpd_handle_t srv;
static SemaphoreHandle_t mu;
static char req_buf[8192];
static char out_buf[8192];

int pex_json_str(const char *json, const char *path, char *out, size_t cap);
int pex_json_len(const char *json, const char *path);
int pex_json_int(const char *json, const char *path, int64_t *out);

static void cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, PUT, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t send_status(httpd_req_t *req, const char *status, const char *type, const char *body) {
    cors(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, body ? body : "", body ? HTTPD_RESP_USE_STRLEN : 0);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body) {
    return send_status(req, status, "application/json", body);
}

static void url_decode(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

/* /api/fs/<path> -> path without a leading slash. *dir is set when the URL
 * names a directory (it ends in '/'). cfg/ is refused by the caller. */
static bool fs_rel(const httpd_req_t *req, char *dst, size_t cap, bool *dir) {
    const char *uri = req->uri;
    if (strncmp(uri, "/api/fs", 7) != 0) return false;
    const char *p = uri + 7;
    if (*p == '/') p++;
    snprintf(dst, cap, "%s", p);
    char *q = strchr(dst, '?');
    if (q) *q = 0;
    size_t n = strlen(dst);
    *dir = n > 0 && dst[n - 1] == '/';
    if (*dir) dst[n - 1] = 0;
    url_decode(dst);
    if (strstr(dst, "..") || strchr(dst, '\\')) return false;
    return true;
}

static bool cfg_hidden(const char *path) {
    return strcmp(path, "cfg") == 0 || strncmp(path, "cfg/", 4) == 0;
}

static uint32_t fnv1a_feed(uint32_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static int file_hash(const char *path, size_t size, uint32_t *out) {
    fs_rd_t *f = fs_open_read(path, NULL);
    if (!f) return -1;
    uint32_t h = 2166136261u;
    uint8_t buf[256];
    uint32_t off = 0;
    while (off < size) {
        uint32_t n = size - off > sizeof buf ? sizeof buf : (uint32_t)(size - off);
        if (fs_pread(f, off, buf, n) != (int)n) { fs_close_read(f); return -1; }
        h = fnv1a_feed(h, buf, n);
        off += n;
    }
    fs_close_read(f);
    *out = h;
    return 0;
}

static esp_err_t fs_get(httpd_req_t *req) {
    char path[160];
    bool as_dir = false;
    if (!fs_rel(req, path, sizeof path, &as_dir)) return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"bad name\"}");
    if (cfg_hidden(path)) return send_json(req, "403 Forbidden", "{\"ok\":false,\"error\":\"cfg is not served\"}");
    if (!path[0] || as_dir || fs_is_dir(path)) {
        xSemaphoreTakeRecursive(mu, portMAX_DELAY);
        int n = fs_list_json(path, out_buf, sizeof out_buf);
        esp_err_t e = n < 0
            ? send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"fs unavailable\"}")
            : send_json(req, "200 OK", out_buf);
        xSemaphoreGiveRecursive(mu);
        return e;
    }
    size_t sz = 0;
    if (!fs_stat(path, &sz)) return send_json(req, "404 Not Found", "{\"ok\":false,\"error\":\"not found\"}");
    fs_rd_t *f = fs_open_read(path, NULL);
    if (!f) return send_json(req, "404 Not Found", "{\"ok\":false,\"error\":\"not found\"}");
    cors(req);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/octet-stream");
    uint8_t chunk[1024];
    uint32_t off = 0;
    esp_err_t er = ESP_OK;
    while (off < sz) {
        uint32_t n = sz - off > sizeof chunk ? sizeof chunk : (uint32_t)(sz - off);
        if (fs_pread(f, off, chunk, n) != (int)n) { er = ESP_FAIL; break; }
        if (httpd_resp_send_chunk(req, (const char *)chunk, n) != ESP_OK) { er = ESP_FAIL; break; }
        off += n;
    }
    fs_close_read(f);
    if (er == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return er;
}

static esp_err_t fs_put(httpd_req_t *req) {
    char path[160];
    bool as_dir = false;
    if (!fs_rel(req, path, sizeof path, &as_dir) || !path[0])
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"bad name\"}");
    if (cfg_hidden(path)) return send_json(req, "403 Forbidden", "{\"ok\":false,\"error\":\"cfg is not served\"}");
    if (as_dir) {
        if (fs_mkdir_p(path)) return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"could not make the folder\"}");
        return send_json(req, "200 OK", "{\"ok\":true,\"dir\":true}");
    }
    if (req->content_len > 1024 * 1024)
        return send_json(req, "413 Payload Too Large", "{\"ok\":false,\"error\":\"file too large\"}");
    fs_wr_t *w = fs_open_write(path);
    if (!w) return send_json(req, "409 Conflict", "{\"ok\":false,\"error\":\"fs write busy or failed\"}");
    int left = req->content_len;
    char chunk[1024];
    bool bad = false;
    while (left > 0) {
        int n = httpd_req_recv(req, chunk, left > (int)sizeof chunk ? (int)sizeof chunk : left);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || fs_pwrite(w, chunk, (uint32_t)n)) { bad = true; break; }
        left -= n;
    }
    if (fs_close_write(w, bad) || bad)
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"write failed\"}");
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t fs_del(httpd_req_t *req) {
    char path[160];
    bool as_dir = false;
    if (!fs_rel(req, path, sizeof path, &as_dir) || !path[0])
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"bad name\"}");
    if (cfg_hidden(path)) return send_json(req, "403 Forbidden", "{\"ok\":false,\"error\":\"cfg is not served\"}");
    if (!fs_stat(path, NULL) && !fs_is_dir(path))
        return send_json(req, "404 Not Found", "{\"ok\":false,\"error\":\"not found\"}");
    if (fs_delete(path)) return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"delete failed\"}");
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t fs_rename_op(httpd_req_t *req) {
    int n = req->content_len;
    if (n <= 0 || n >= (int)sizeof req_buf) return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"bad name\"}");
    int got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, req_buf + got, n - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"bad name\"}");
        got += r;
    }
    req_buf[got] = 0;
    char from[140], to[140];
    if (!pex_json_str(req_buf, "from", from, sizeof from) || !pex_json_str(req_buf, "to", to, sizeof to))
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"bad name\"}");
    if (cfg_hidden(from) || cfg_hidden(to))
        return send_json(req, "403 Forbidden", "{\"ok\":false,\"error\":\"cfg is not served\"}");
    int err = fs_rename(from, to);
    if (err == 1) return send_json(req, "409 Conflict", "{\"ok\":false,\"error\":\"exists\"}");
    if (err < 0) {
        if (!fs_stat(from, NULL) && !fs_is_dir(from))
            return send_json(req, "404 Not Found", "{\"ok\":false,\"error\":\"not found\"}");
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"rename failed\"}");
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t opt_any(httpd_req_t *req) {
    cors(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t no_assets(httpd_req_t *req) {
    return send_json(req, "404 Not Found",
                     "{\"ok\":false,\"error\":\"this board has no web assets\",\"assets\":0,\"ws\":\"/ws\",\"api\":\"/api\"}");
}

static long json_id(const char *s) {
    const char *p = strstr(s ? s : "", "\"id\"");
    if (!p) return 0;
    p += 4;
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    while (*p == ' ') p++;
    return strtol(p, NULL, 10);
}

static void reply_ok(char *dst, size_t cap, long id, const char *result) {
    snprintf(dst, cap, "{\"id\":%ld,\"ok\":true,\"result\":%s}", id, result && *result ? result : "null");
}

static void reply_err(char *dst, size_t cap, long id, const char *code, const char *msg) {
    snprintf(dst, cap, "{\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
             id, code ? code : "error", msg ? msg : "");
}

static void reboot_timer(void *arg) { (void)arg; esp_restart(); }

static int op_sys(const char *op, const char *frame, char *dst, size_t cap, const char **err) {
    if (!strcmp(op, "sys.info")) {
        snprintf(dst, cap,
                 "{\"firmware\":\"btgw-esp32c5\",\"board\":\"esp32c5\",\"uptime_ms\":%llu,"
                 "\"assets\":0,\"hostname\":\"%s\",\"transports\":[\"wifi\"],"
                 "\"modules\":[\"fs\",\"wifi\",\"pex\",\"ota\",\"launch\"],"
                 "\"fs\":{\"mounted\":true,\"block\":4096}}",
                 (unsigned long long)(esp_timer_get_time() / 1000), mdns_adv_hostname());
        return 0;
    }
    if (!strcmp(op, "sys.reboot")) {
        const esp_timer_create_args_t args = { .callback = reboot_timer, .name = "reboot" };
        esp_timer_handle_t t = NULL;
        if (esp_timer_create(&args, &t) != ESP_OK || esp_timer_start_once(t, 150 * 1000) != ESP_OK) {
            *err = "could not schedule the reboot";
            return -1;
        }
        snprintf(dst, cap, "{\"rebooting\":true}");
        return 0;
    }
    (void)frame;
    *err = "not supported on this image";
    return -1;
}

static int op_wifi(const char *op, const char *frame, char *dst, size_t cap, const char **err) {
    if (!strcmp(op, "wifi.status")) { wifi_status_json(dst, cap); return 0; }
    if (!strcmp(op, "wifi.scan")) {
        bool started = wifi_scan_start();
        snprintf(dst, cap, "{\"started\":%s,\"scanning\":%s}", started ? "true" : "false", wifi_scanning() ? "true" : "false");
        return 0;
    }
    if (!strcmp(op, "wifi.scan_results")) { wifi_scan_json(dst, cap); return 0; }
    if (!strcmp(op, "wifi.networks")) { cfg_networks_json(dst, cap, wifi_current_ssid()); return 0; }
    if (!strcmp(op, "wifi.connect")) {
        char ssid[33], psk[64];
        if (!pex_json_str(frame, "params.ssid", ssid, sizeof ssid) || !ssid[0]) { *err = "need \"ssid\""; return -1; }
        bool have = pex_json_str(frame, "params.psk", psk, sizeof psk);
        if (!wifi_join(ssid, have ? psk : NULL)) { *err = "the credential store refused that ssid/psk"; return -1; }
        snprintf(dst, cap, "{\"joining\":true,\"ssid\":\"%s\"}", ssid);
        return 0;
    }
    if (!strcmp(op, "wifi.forget")) {
        char ssid[33];
        if (!pex_json_str(frame, "params.ssid", ssid, sizeof ssid) || !ssid[0]) { *err = "need \"ssid\""; return -1; }
        bool had = cfg_net_forget(ssid);
        snprintf(dst, cap, "{\"forgotten\":%s}", had ? "true" : "false");
        return 0;
    }
    if (!strcmp(op, "wifi.mode")) {
        char mode[16];
        if (pex_json_str(frame, "params.mode", mode, sizeof mode)) {
            if (!cfg_set_mode(mode)) { *err = "mode must be one of auto, sta, ap, both, off"; return -1; }
            wifi_apply_cfg();
        }
        snprintf(dst, cap, "{\"mode\":\"%s\"}", cfg_mode());
        return 0;
    }
    if (!strcmp(op, "wifi.ap")) {
        char ssid[33], psk[64];
        if (pex_json_str(frame, "params.ssid", ssid, sizeof ssid)) {
            bool have = pex_json_str(frame, "params.psk", psk, sizeof psk);
            if (!cfg_set_ap(ssid, have ? psk : NULL)) { *err = "the store refused that ap config"; return -1; }
            wifi_apply_cfg();
        }
        snprintf(dst, cap, "{\"ssid\":\"%s\"}", cfg_ap_ssid());
        return 0;
    }
    *err = "not supported on this image";
    return -1;
}

static int op_fs_stat(const char *frame, char *dst, size_t cap, const char **err) {
    int n = pex_json_len(frame, "params.paths");
    if (n < 0) { *err = "need \"paths\""; return -1; }
    if (n > 32) { *err = "at most 32 paths"; return -1; }
    size_t o = (size_t)snprintf(dst, cap, "{\"files\":[");
    for (int i = 0; i < n; i++) {
        char key[32], path[140];
        snprintf(key, sizeof key, "params.paths.%d", i);
        if (!pex_json_str(frame, key, path, sizeof path)) { *err = "need \"paths\""; return -1; }
        size_t sz = 0;
        bool ok = !cfg_hidden(path) && fs_stat(path, &sz);
        uint32_t h = 0;
        bool hashed = ok && file_hash(path, sz, &h) == 0;
        int w;
        if (!ok) w = snprintf(dst + o, cap - o, "%s{\"path\":\"%s\",\"missing\":true}", i ? "," : "", path);
        else if (hashed) w = snprintf(dst + o, cap - o, "%s{\"path\":\"%s\",\"size\":%u,\"hash\":\"%08lx\"}",
                                      i ? "," : "", path, (unsigned)sz, (unsigned long)h);
        else w = snprintf(dst + o, cap - o, "%s{\"path\":\"%s\",\"size\":%u}", i ? "," : "", path, (unsigned)sz);
        if (w < 0 || o + (size_t)w >= cap - 4) { *err = "stat"; return -1; }
        o += (size_t)w;
    }
    snprintf(dst + o, cap - o, "]}");
    return 0;
}

static char result_buf[7000];

static int dispatch_op(const char *frame, char *dst, size_t cap) {
    long id = json_id(frame);
    char op[48];
    if (!pex_json_str(frame, "op", op, sizeof op)) {
        reply_err(dst, cap, id, "bad_request", "missing \"op\"");
        return (int)strlen(dst);
    }
    char *result = result_buf;
    const char *err = NULL;
    int rc;
    if (!strncmp(op, "pex.", 4)) rc = pex_host_op(op, frame, result, sizeof result_buf, &err);
    else if (!strcmp(op, "launch.status")) { launch_status_json(result, sizeof result_buf); rc = 0; }
    else if (!strcmp(op, "launch.set")) rc = launch_set_json(frame, result, sizeof result_buf, &err);
    else if (!strcmp(op, "launch.now")) {
        launch_now();
        pex_host_poll();
        snprintf(result, sizeof result_buf, "{\"started\":true}");
        rc = 0;
    } else if (!strncmp(op, "sys.", 4)) rc = op_sys(op, frame, result, sizeof result_buf, &err);
    else if (!strncmp(op, "wifi.", 5)) rc = op_wifi(op, frame, result, sizeof result_buf, &err);
    else if (!strcmp(op, "fs.stat")) rc = op_fs_stat(frame, result, sizeof result_buf, &err);
    else { err = "not supported on this image"; rc = -1; }
    if (rc) reply_err(dst, cap, id, "error", err ? err : "failed");
    else reply_ok(dst, cap, id, result);
    return (int)strlen(dst);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) return ESP_OK;
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) return ESP_FAIL;
    if (frame.len == 0 || frame.len >= sizeof req_buf) return ESP_OK;
    frame.payload = (uint8_t *)req_buf;
    if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) return ESP_FAIL;
    req_buf[frame.len] = 0;
    xSemaphoreTakeRecursive(mu, portMAX_DELAY);
    int n = dispatch_op(req_buf, out_buf, sizeof out_buf);
    httpd_ws_frame_t out = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)out_buf,
        .len = (size_t)n,
    };
    esp_err_t e = httpd_ws_send_frame(req, &out);
    xSemaphoreGiveRecursive(mu);
    return e;
}

void http_api_start(void) {
    mu = xSemaphoreCreateRecursiveMutex();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 6;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd did not start");
        return;
    }
    const httpd_uri_t routes[] = {
        { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true },
        { .uri = "/", .method = HTTP_GET, .handler = no_assets },
        { .uri = "/index.html", .method = HTTP_GET, .handler = no_assets },
        { .uri = "/api/fs", .method = HTTP_GET, .handler = fs_get },
        { .uri = "/api/fs/*", .method = HTTP_GET, .handler = fs_get },
        { .uri = "/api/fs/*", .method = HTTP_PUT, .handler = fs_put },
        { .uri = "/api/fs/*", .method = HTTP_DELETE, .handler = fs_del },
        { .uri = "/api/fs/rename", .method = HTTP_POST, .handler = fs_rename_op },
        { .uri = "/*", .method = HTTP_OPTIONS, .handler = opt_any },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        if (httpd_register_uri_handler(srv, &routes[i]) != ESP_OK)
            ESP_LOGW(TAG, "did not register %s", routes[i].uri);
    }
    ESP_LOGI(TAG, "websocket /ws and /api/fs on port 80; no static assets");
}
