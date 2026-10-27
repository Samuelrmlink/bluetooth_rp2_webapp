/* HTTP + WebSocket server. Static-file-serving/fs-API structure adapted
 * from the confirmed-working reference at
 * /home/samuel/Projects/pm220-pico2w/src/net/http.c (same hand-rolled
 * raw-lwIP-TCP approach, no RTOS, no lwIP httpd app) — printer/Bluetooth/
 * Wi-Fi-specific routes dropped, replaced with a WebSocket upgrade path
 * (new — pm220-pico2w has no WebSocket) that tunnels proto_stub's ASCII
 * line protocol, i.e. the same wire format js/proto.js already parses. */
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include "lwip/tcp.h"
#include "lwip/stats.h"
#include "fs/fs.h"
#include "net/ws.h"
#include "api/api.h"
#include "autoconf.h"
#if CONFIG_SD
#include "sd/sd_api.h"
#endif
#if CONFIG_NET_WIFI
#include "net/wifi.h"
#include "config/config.h"
#endif
#include "pico/time.h"
#if CONFIG_LUA_ENABLE
#include "lua/lheap.h"
#endif

#include "net/usb_ncm.h"

#define HTTP_HDR 1024
/* A request's path. SD names arrive percent-encoded (/api/sd/files/...), so
 * an SD build takes twice the usual; others keep the 96 they always had. */
#if CONFIG_SD
#define HTTP_PATH 192
#else
#define HTTP_PATH 96
#endif
#define HTTP_CONNS 8
#define FILE_CHUNK 1460
#define HTTP_POLL_INT 2
#define HTTP_MAX_RETRIES 20
#define WS_PATH "/ws"

static char json_buf[4096];
static uint8_t file_chunk[FILE_CHUNK];
/* The tar stream's own buffer: unlike a file it cannot be rewound, so what
 * TCP did not take yet waits here for the next pump. One archive at a time
 * (fs.c), so one buffer. */
static uint8_t tar_buf[FILE_CHUNK];
static size_t tar_buf_len, tar_buf_pos;

typedef struct {
    struct tcp_pcb *pcb;
    char hdr[HTTP_HDR];
    uint16_t hdr_len;
    bool hdr_done;
    size_t content_len;
    size_t body_got;
    bool taking_fs_body;
    bool taking_post_body;
    bool sending_file;
    bool sending_tar;       /* streaming a folder as tar (fs_tar_*) */
    bool sending_remove;    /* POST /api/fs/remove, one path per main-loop turn */
    uint64_t tar_left;      /* archive bytes still to send */
    char fs_name[FS_PATH_MAX + 1];
    char method[8];
    char path[HTTP_PATH];
    /* 512, not 256: PUT /api/notify carries a URL, an MQTT host, a topic
     * and credentials in one body, and the WiFi PUTs were already close to
     * the old limit with a 63-character PSK. */
    char post_body[512];
    /* POST /api/op's body (up to OP_BODY_MAX): too big for post_body, and
     * eight connections' worth of static buffer is RAM this part does not
     * have, so it comes from the top of the Lua arena per request. */
    char *op_body;
    uint32_t op_gen;
    int file_h;
    size_t file_left;
    bool closing;
    bool pending_file;
    bool pending_gzip;
    uint8_t retries;
    bool is_ws;
    bool follow;
    ws_parser_t ws;
    /* Live lines this follower did not get because its TCP send buffer was
     * full (tcp_write ERR_MEM). Before this was counted the line went on the
     * floor silently, and a 1.6 s WiFi stall looked like a hole in the
     * meter's data. Reported on STATE as DROPPED= for the asking client. */
    uint32_t dropped;
#if CONFIG_SD
    /* an /api/sd transfer this connection is waiting on (sd_service) */
    bool sd_on;
    bool sd_hdr_sent;
    bool sd_in_ended;
    bool taking_sd_body;
    uint8_t sd_x;
    uint8_t sd_op;
    struct pbuf *sd_hold;        /* upload bytes the worker has not taken yet */
    uint16_t sd_hold_off;
    uint32_t sd_acked_ahead;     /* held bytes already tcp_recved (they came with the headers) */
    size_t sd_put_got;
#endif
} http_conn_t;

static http_conn_t conns[HTTP_CONNS];

#define OP_BODY_MAX 4096

static char *op_alloc(size_t n, uint32_t *gen) {
#if CONFIG_LUA_ENABLE
    *gen = lheap_generation();
    return (char *)lheap_alloc_high(n);
#else
    *gen = 0;
    return (char *)malloc(n);
#endif
}

static void op_free(char *p, uint32_t gen) {
    if (!p) { return; }
#if CONFIG_LUA_ENABLE
    /* An op can restart the Lua VM (script.vm_restart), which resets the
     * arena this came from: then it is already gone. */
    if (gen == lheap_generation()) { lheap_free(p); }
#else
    (void)gen;
    free(p);
#endif
}

static bool path_is(const char *path, const char *want) {
    size_t n = strlen(want);
    return strncmp(path, want, n) == 0 && (path[n] == 0 || path[n] == '?' || path[n] == '#');
}

static const char *status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 507: return "Insufficient Storage";
        default: return "OK";
    }
}

static const char *cors_methods(void) {
    return "GET, POST, PUT, DELETE, OPTIONS";
}

static err_t http_reply(struct tcp_pcb *tpcb, int code, const char *ctype, const char *body) {
    if (!body) {
        body = "";
    }
    char hdr[360];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %u\r\n"
                      "Cache-Control: no-store\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Access-Control-Allow-Methods: %s\r\n"
                      "Access-Control-Allow-Headers: Content-Type\r\n"
                      "Connection: close\r\n\r\n",
                      code, status_text(code), ctype, (unsigned)strlen(body), cors_methods());
    tcp_write(tpcb, hdr, (uint16_t)hl, TCP_WRITE_FLAG_COPY);
    if (body[0]) {
        tcp_write(tpcb, body, (uint16_t)strlen(body), TCP_WRITE_FLAG_COPY);
    }
    tcp_output(tpcb);
    tcp_close(tpcb);
    return ERR_OK;
}

static http_conn_t *conn_find(struct tcp_pcb *tpcb) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (conns[i].pcb == tpcb) {
            return &conns[i];
        }
    }
    return NULL;
}

static void conn_cleanup(http_conn_t *c) {
    op_free(c->op_body, c->op_gen);
    c->op_body = NULL;
    if (c->taking_fs_body) {
        fs_abort_write();
        if (c->fs_name[0]) {
            fs_delete(c->fs_name);
        }
    }
    if (c->sending_file) {
        fs_end_read(c->file_h);
    }
    if (c->sending_tar) {
        fs_tar_end();
    }
    if (c->sending_remove) {
        c->sending_remove = false;
        fs_remove_abort();
    }
#if CONFIG_SD
    if (c->sd_on) {
        sd_xfer_release(c->sd_x); /* the worker stops; an unfinished upload is deleted */
        c->sd_on = false;
    }
    if (c->sd_hold) {
        pbuf_free(c->sd_hold);
        c->sd_hold = NULL;
    }
#endif
}

static void conn_reset(http_conn_t *c) {
    memset(c, 0, sizeof(*c));
    c->file_h = -1;
}

static bool conn_held(const http_conn_t *c) {
    return c->sending_file || c->sending_tar || c->sending_remove || c->closing || c->pending_file || c->is_ws
#if CONFIG_SD
           || c->sd_on
#endif
        ;
}

static void conn_drop(struct tcp_pcb *tpcb) {
    http_conn_t *c = conn_find(tpcb);
    if (c) {
        conn_cleanup(c);
        conn_reset(c);
    }
}

static http_conn_t *conn_for(struct tcp_pcb *tpcb) {
    http_conn_t *c = conn_find(tpcb);
    if (c) {
        return c;
    }
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (!conns[i].pcb) {
            conn_reset(&conns[i]);
            conns[i].pcb = tpcb;
            return &conns[i];
        }
    }
    return NULL;
}

static bool copy_path_token(const char *p, char *out, size_t cap) {
    size_t n = 0;
    while (p[n] && p[n] != '?' && p[n] != '#') {
        n++;
    }
    if (n == 0 || n >= cap) {
        return false;
    }
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

/* Device configuration lives under this prefix and must never be reachable
 * through the filesystem routes or as a static file. /api/fs serves whatever
 * it is asked for, unauthenticated and with CORS wide open — and once WiFi is
 * up, "whatever it is asked for" is offered to the whole LAN. The WiFi API is
 * the only thing allowed to read credentials out, which is what keeps showing
 * passwords in the UI a deliberate choice rather than an accident.
 *
 * Checked in BOTH fs_api_name() and static_req_name(): blocking only the
 * former would still leave GET /cfg/wifi.json serving the file as a static
 * asset. */
#define CFG_PREFIX "cfg/"

/* cfg/ is firmware-owned and never reachable from outside. staged/ is
 * different: it is WRITABLE through the filesystem API on purpose, because
 * that is how script source larger than a WebSocket frame gets to the device,
 * streaming straight to flash. It must still never be served as a static
 * asset, so static_req_name checks it separately. */
static bool path_is_staged(const char *name) {
    return strncmp(name, "staged/", 7) == 0;
}

/* cfg/ used to be refused outright by the filesystem API, on the reasoning
 * that a credential should not be reachable over the network. That is no
 * longer the policy: on a development bench the operator wants to read back
 * what is provisioned, and the thing actually worth protecting is the git
 * history, not the device. cfg/ is git-ignored and stays that way.
 *
 * Note what this now exposes: cfg/wifi.json holds the WiFi PSK, so anyone who
 * can reach port 80 can read it. On a USB-NCM link or a trusted LAN that is
 * the intended trade; on an untrusted network it is not.
 *
 * Still refused for STATIC serving -- see static_req_name -- because a config
 * file should not be fetchable as a web asset at its bare path. */
static bool path_reserved(const char *name) {
    (void)name;
    return false;
}

static bool fs_api_name(const char *path, char *out, size_t cap) {
    if (strncmp(path, "/api/fs/", 8) != 0) {
        return false;
    }
    if (!copy_path_token(path + 8, out, cap)) {
        return false;
    }
    size_t n = strlen(out);
    if (n && out[n - 1] == '/') {
        out[n - 1] = 0;
    }
    if (path_reserved(out)) {
        return false;
    }
    return out[0] != 0;
}

static bool json_str_field(const char *body, const char *key, char *out, size_t cap) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body ? body : "", pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < cap) {
        n++;
    }
    if (p[n] != '"') {
        return false;
    }
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static bool static_req_name(const char *path, char *out, size_t cap) {
    if (strncmp(path, "/api/", 5) == 0 || path_is(path, WS_PATH)) {
        return false;
    }
    const char *p = path;
    if (*p == '/') {
        p++;
    }
    if (*p == 0 || *p == '?' || *p == '#') {
        if (cap < 11) {
            return false;
        }
        memcpy(out, "index.html", 11);
        return true;
    }
    if (!copy_path_token(p, out, cap)) {
        return false;
    }
    /* "/anker/" and "/anker" name the same app. fs_valid_path rejects the
     * empty final segment a trailing slash leaves behind, so strip it here --
     * fs_api_name already does the same for its half of the URL space. The
     * trailing-slash form is the one the app nav links to, so without this
     * every link between apps 404s. */
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[--len] = 0;
    }
    /* staged/ is writable through the filesystem API but must not be servable:
     * uploading a script should not publish it at a URL. */
    if (path_reserved(out) || path_is_staged(out)) {
        return false;
    }
    return fs_valid_path(out);
}

static bool ends_with(const char *s, const char *suf) {
    size_t n = strlen(s);
    size_t m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

/* Try "<base>.gz" before "<base>", because the .gz is what is actually
 * shipped -- make_release.py stores only the compressed copy. */
static bool try_file(const char *base, char *name, size_t cap, bool *gzipped) {
    size_t n = strlen(base);
    if (!ends_with(base, ".gz") && n + 4 < cap) {
        memcpy(name, base, n);
        memcpy(name + n, ".gz", 4);
        if (fs_stat(name, NULL)) {
            *gzipped = true;
            return true;
        }
    }
    if (n >= cap) {
        return false;
    }
    memcpy(name, base, n + 1);
    if (fs_stat(name, NULL) && !fs_is_dir(name)) {
        *gzipped = false;
        return true;
    }
    return false;
}

static bool resolve_static(const char *req, char *name, size_t cap, bool *gzipped) {
    *gzipped = false;
    if (try_file(req, name, cap, gzipped)) {
        return true;
    }
    /* A DIRECTORY serves its index.html, so each webapp can live in its own
     * folder and be reached at /anker/ or /pinecil/ rather than every app
     * sharing one page. Both "/anker" and "/anker/" land here. */
    char idx[FS_PATH_MAX + 1];
    size_t n = strlen(req);
    if (n == 0 || n + 12 >= sizeof(idx)) {
        return false;
    }
    int w = snprintf(idx, sizeof(idx), "%s%sindex.html", req,
                     req[n - 1] == '/' ? "" : "/");
    if (w <= 0 || (size_t)w >= sizeof(idx)) {
        return false;
    }
    return try_file(idx, name, cap, gzipped);
}

static const char *mime_of(const char *name) {
    char tmp[FS_PATH_MAX + 1];
    size_t n = strlen(name);
    if (n >= 3 && strcmp(name + n - 3, ".gz") == 0) {
        n -= 3;
    }
    if (n == 0 || n >= sizeof(tmp)) {
        return "application/octet-stream";
    }
    memcpy(tmp, name, n);
    tmp[n] = 0;
    const char *ext = strrchr(tmp, '.');
    if (!ext) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".js") == 0 || strcmp(ext, ".mjs") == 0) return "text/javascript; charset=utf-8";
    if (strcmp(ext, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".txt") == 0) return "text/plain; charset=utf-8";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    return "application/octet-stream";
}

static err_t http_start_file(http_conn_t *c, struct tcp_pcb *tpcb, const char *name, bool gzipped);
static err_t http_finish_file(http_conn_t *c, struct tcp_pcb *tpcb, int rc);
static void http_kick_pending(void);

static void http_try_close(http_conn_t *c, struct tcp_pcb *tpcb) {
    if (c->sending_file) {
        fs_end_read(c->file_h);
        c->sending_file = false;
        c->file_h = -1;
    }
    if (c->sending_tar) {
        fs_tar_end();
        c->sending_tar = false;
    }
    c->closing = true;
    if (tcp_close(tpcb) == ERR_OK) {
        conn_reset(c);
    }
}

/* 1 = all bytes queued, 0 = wait for sent/poll, -1 = abort. */
static int http_pump_file(http_conn_t *c, struct tcp_pcb *tpcb) {
    while (c->file_left) {
        u16_t room = tcp_sndbuf(tpcb);
        if (room < 32) {
            break;
        }
        size_t want = c->file_left;
        if (want > sizeof(file_chunk)) {
            want = sizeof(file_chunk);
        }
        if (want > room) {
            want = room;
        }
        int n = fs_read(c->file_h, file_chunk, want);
        if (n <= 0) {
            return -1;
        }
        if (tcp_write(tpcb, file_chunk, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            if (fs_rewind(c->file_h, (size_t)n) < 0) {
                return -1;
            }
            break;
        }
        c->file_left -= (size_t)n;
        c->retries = 0;
    }
    tcp_output(tpcb);
    return c->file_left == 0 ? 1 : 0;
}

/* Same contract as http_pump_file. -1 also when the walk no longer matches
 * the length promised in Content-Length (a folder changed mid-download):
 * aborting shows the browser a failed download instead of a wrong archive. */
static int http_pump_tar(http_conn_t *c, struct tcp_pcb *tpcb) {
    while (c->tar_left) {
        if (tar_buf_pos == tar_buf_len) {
            size_t want = sizeof(tar_buf) < c->tar_left ? sizeof(tar_buf) : (size_t)c->tar_left;
            int n = fs_tar_read(tar_buf, want);
            if (n <= 0) {
                return -1;                      /* shorter than promised */
            }
            tar_buf_len = (size_t)n;
            tar_buf_pos = 0;
        }
        u16_t room = tcp_sndbuf(tpcb);
        if (room < 32) {
            break;
        }
        size_t k = tar_buf_len - tar_buf_pos;
        if (k > room) {
            k = room;
        }
        if (tcp_write(tpcb, tar_buf + tar_buf_pos, (u16_t)k, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            break;                              /* kept in tar_buf for the next pump */
        }
        tar_buf_pos += k;
        c->tar_left -= k;
        c->retries = 0;
    }
    tcp_output(tpcb);
    if (c->tar_left) {
        return 0;
    }
    uint8_t probe;
    return fs_tar_read(&probe, 1) == 0 ? 1 : -1;   /* longer than promised */
}

static uint32_t query_u32(const char *path, const char *key) {
    size_t klen = strlen(key);
    const char *q = strchr(path, '?');
    if (!q) { return 0; }
    for (const char *p = q + 1; *p; ) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            return (uint32_t)strtoul(p + klen + 1, NULL, 10);
        }
        const char *amp = strchr(p, '&');
        if (!amp) { break; }
        p = amp + 1;
    }
    return 0;
}

static void release_op_body(http_conn_t *c) {
    op_free(c->op_body, c->op_gen);
    c->op_body = NULL;
}

static err_t http_write_tar_hdr(struct tcp_pcb *tpcb, unsigned long len, const char *base) {
    char hdr[480];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/x-tar\r\n"
                      "Content-Length: %lu\r\n"
                      "Content-Disposition: attachment; filename=\"%s.tar\"\r\n"
                      "Cache-Control: no-store\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Access-Control-Expose-Headers: Content-Length, Content-Disposition\r\n"
                      "Connection: close\r\n\r\n",
                      len, base);
    if (hl <= 0 || (size_t)hl >= sizeof(hdr)) { return ERR_MEM; }
    if (tcp_write(tpcb, hdr, (uint16_t)hl, TCP_WRITE_FLAG_COPY) != ERR_OK) { return ERR_MEM; }
    return ERR_OK;
}

static err_t http_tar_arm(http_conn_t *c, struct tcp_pcb *tpcb, uint64_t len, const char *base) {
    if (http_write_tar_hdr(tpcb, (unsigned long)len, base) != ERR_OK) {
        fs_tar_end();
        return http_reply(tpcb, 503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    }
    c->sending_tar = true;
    c->tar_left = len;
    c->retries = 0;
    tar_buf_len = tar_buf_pos = 0;
    return http_finish_file(c, tpcb, http_pump_tar(c, tpcb));
}

/* GET /api/tar/<folder>[?mtime=<unix s>] -- the folder ("" = everything) as
 * a ustar archive, streamed (fs.c fs_tar_*). The board has no clock, so the
 * browser passes the time to stamp the entries with. id= names the fs.job
 * notes; a firmware that does not know it ignores the query. */
static err_t http_start_tar(http_conn_t *c, struct tcp_pcb *tpcb, const char *path) {
    char folder[FS_PATH_MAX + 1] = "";
    const char *p = path + 8;                   /* after "/api/tar" */
    while (*p == '/') { p++; }
    size_t n = 0;
    while (p[n] && p[n] != '?' && p[n] != '#' && n < FS_PATH_MAX) { folder[n] = p[n]; n++; }
    folder[n] = 0;
    while (n && folder[n - 1] == '/') { folder[--n] = 0; }
    if (folder[0] && !fs_valid_path(folder)) {
        return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad name\"}");
    }
    uint32_t mtime = query_u32(path, "mtime");
    uint64_t len = 0;
    int rc = fs_tar_begin(folder, mtime, &len);
    if (rc == -2) {
        return http_reply(tpcb, 503, "application/json", "{\"ok\":false,\"error\":\"an archive is already streaming\"}");
    }
    if (rc == -3) {
        return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"no such folder\"}");
    }
    if (rc < 0) {
        return http_reply(tpcb, 500, "application/json", "{\"ok\":false,\"error\":\"could not read the folder\"}");
    }
    fs_tar_set_job(query_u32(path, "id"));
    const char *base = strrchr(folder, '/');
    base = base ? base + 1 : (folder[0] ? folder : "btgw");
    return http_tar_arm(c, tpcb, len, base);   /* newlib-nano: no %llu; an archive is < 2 MB */
}

static err_t http_start_tar_post(http_conn_t *c, struct tcp_pcb *tpcb, const char *body, size_t body_len) {
    uint64_t len = 0;
    int rc = fs_tar_begin_json(body ? body : "", body_len, &len);
    release_op_body(c);
    if (rc == -3) {
        return http_reply(tpcb, 503, "application/json", "{\"ok\":false,\"error\":\"an archive is already streaming\"}");
    }
    if (rc == -2) {
        return http_reply(tpcb, 413, "application/json", "{\"ok\":false,\"error\":\"too many paths\"}");
    }
    if (rc == -4) {
        return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
    }
    if (rc < 0) {
        return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad request\"}");
    }
    return http_tar_arm(c, tpcb, len, "files");
}

static err_t http_start_remove(http_conn_t *c, struct tcp_pcb *tpcb, const char *body, size_t body_len) {
    int rc = fs_remove_begin_json(body ? body : "", body_len);
    release_op_body(c);
    if (rc == -3) {
        return http_reply(tpcb, 503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    }
    if (rc == -2) {
        return http_reply(tpcb, 413, "application/json", "{\"ok\":false,\"error\":\"too many paths\"}");
    }
    if (rc < 0) {
        return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad request\"}");
    }
    c->sending_remove = true;
    c->retries = 0;
    return ERR_OK;
}

void http_jobs_pump(void) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        http_conn_t *c = &conns[i];
        if (!c->pcb || !c->sending_remove) { continue; }
        int removed = 0, total = 0;
        char err[96];
        err[0] = 0;
        int st = fs_remove_result(&removed, &total, err, sizeof err);
        if (st == 0) { continue; }
        c->sending_remove = false;
        if (st == -2) {
            conn_cleanup(c);
            conn_reset(c);
            continue;
        }
        char body[220];
        int code;
        if (st > 0) {
            snprintf(body, sizeof body, "{\"ok\":true,\"removed\":%d,\"total\":%d}", removed, total);
            code = 200;
        } else {
            char esc[140];
            api_json_escape(esc, sizeof esc, err[0] ? err : "delete failed");
            snprintf(body, sizeof body,
                     "{\"ok\":false,\"error\":\"%s\",\"removed\":%d,\"total\":%d}",
                     esc, removed, total);
            code = 400;
        }
        struct tcp_pcb *pcb = c->pcb;
        http_reply(pcb, code, "application/json", body);
        conn_cleanup(c);
        conn_reset(c);
    }
}

static err_t http_finish_file(http_conn_t *c, struct tcp_pcb *tpcb, int rc) {
    if (rc < 0) {
        conn_cleanup(c);
        conn_reset(c);
        tcp_abort(tpcb);
        http_kick_pending();
        return ERR_ABRT;
    }
    if (rc > 0) {
        http_try_close(c, tpcb);
        http_kick_pending();
    }
    return ERR_OK;
}

static void http_park_file(http_conn_t *c, const char *name, bool gzipped) {
    snprintf(c->fs_name, sizeof(c->fs_name), "%s", name);
    c->pending_file = true;
    c->pending_gzip = gzipped;
    c->sending_file = false;
    c->file_h = -1;
}

static err_t http_start_file(http_conn_t *c, struct tcp_pcb *tpcb, const char *name, bool gzipped) {
    size_t sz = 0;
    int h = fs_begin_read(name, &sz);
    if (h < 0) {
        http_park_file(c, name, gzipped);
        return ERR_OK;
    }
    char hdr[400];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %u\r\n"
                      "%s"
                      "Cache-Control: no-cache\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Access-Control-Allow-Methods: %s\r\n"
                      "Access-Control-Allow-Headers: Content-Type\r\n"
                      "Connection: close\r\n\r\n",
                      mime_of(name), (unsigned)sz,
                      gzipped ? "Content-Encoding: gzip\r\n" : "",
                      cors_methods());
    if (tcp_write(tpcb, hdr, (uint16_t)hl, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        fs_end_read(h);
        http_park_file(c, name, gzipped);
        return ERR_OK;
    }
    c->pending_file = false;
    c->sending_file = true;
    c->file_h = h;
    c->file_left = sz;
    c->retries = 0;
    return http_finish_file(c, tpcb, http_pump_file(c, tpcb));
}

static void http_kick_pending(void) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        http_conn_t *c = &conns[i];
        if (c->pcb && c->pending_file) {
            http_start_file(c, c->pcb, c->fs_name, c->pending_gzip);
            if (!c->pending_file) {
                break;
            }
        }
    }
}

/* --- WebSocket upgrade + proto_stub tunnel ---------------------------- */

/* Case-insensitive search for "\r\nName:" (or start-of-headers "Name:")
 * and returns a pointer to its trimmed value plus length; NULL if absent.
 * `hdr` is the raw request-line-and-headers block (NUL-terminated). */
static const char *find_header(const char *hdr, const char *name, size_t *len) {
    size_t name_len = strlen(name);
    const char *p = hdr;
    while (*p) {
        bool at_start = (p == hdr) || (p[-1] == '\n');
        if (at_start && strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            p += name_len + 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            const char *v = p;
            while (*p && *p != '\r' && *p != '\n') {
                p++;
            }
            *len = (size_t)(p - v);
            return v;
        }
        p++;
    }
    return NULL;
}

static bool header_has_token_ci(const char *value, size_t len, const char *token) {
    /* "Connection: keep-alive, Upgrade" — comma-separated tokens. */
    size_t tok_len = strlen(token);
    const char *p = value;
    const char *end = value + len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',')) {
            p++;
        }
        const char *start = p;
        while (p < end && *p != ',') {
            p++;
        }
        size_t seg_len = (size_t)(p - start);
        while (seg_len > 0 && start[seg_len - 1] == ' ') {
            seg_len--;
        }
        if (seg_len == tok_len && strncasecmp(start, token, tok_len) == 0) {
            return true;
        }
    }
    return false;
}

/* ctx is always one of conns[]'s (permanently allocated) slots, never a
 * raw tcp_pcb* — see http_send_line/http_broadcast_line below for why:
 * a reply from core1 can arrive well after the WS connection it was for
 * has closed, so the target needs to still be safely dereferenceable
 * (and checkable for "is this still that same live connection") at that
 * later point. */
static void ws_reply_line(const char *line, void *ctx) {
    http_conn_t *c = (http_conn_t *)ctx;
    if (!c->pcb || !c->is_ws) {
        return;
    }
    /* static: WS_TX_MAX is far too large for core0's stack, and this runs from
     * an lwIP callback which is already the deepest path we have. Safe because
     * core0 is single-threaded and the frame is handed to tcp_write here. */
    static uint8_t frame[WS_TX_MAX];
    size_t n = ws_encode_text_frame((const uint8_t *)line, strlen(line), frame, sizeof(frame));
    if (n == 0) {
        c->dropped++;   /* over WS_TX_MAX: counted, never silently discarded */
        return;
    }
    /* No tcp_output() here, deliberately. Pushing every line out as its own
     * segment meant a follower could have at most TCP_SND_QUEUELEN (16) lines
     * in flight, and the 17th during a WiFi stall was silently dropped — 1.6 s
     * of data at 10 SPS, seen live as gaps in the graph. Left alone, lwIP
     * packs consecutive writes into one MSS-sized segment (TCP_OVERSIZE) and
     * Nagle sends it as soon as the previous one is ACKed: ~one RTT of
     * latency, and the buffer becomes TCP_SND_BUF's 5.8 KB (~45 lines) rather
     * than 16 segments. http_flush_ws() at the end of each drain pass sends
     * what is pending; a stall no longer turns into loss until that buffer is
     * genuinely full, and then it is counted. No new memory: the copies come
     * from the same lwIP heap they always did. */
    if (tcp_write(c->pcb, frame, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        c->dropped++;
    }
}

/* One tcp_output() per WS follower per drain pass — see ws_reply_line(). */
void http_flush_ws(void) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (conns[i].pcb && conns[i].is_ws) {
            tcp_output(conns[i].pcb);
        }
    }
}

uint32_t http_conn_dropped(void *ctx) {
    http_conn_t *c = (http_conn_t *)ctx;
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (&conns[i] == c) return c->dropped;
    }
    return 0;
}

static void ws_on_text(const uint8_t *payload, size_t len, void *user) {
    http_conn_t *c = (http_conn_t *)user;
    /* static: a kilobyte, on the deepest path core0 has — an lwIP recv
     * callback into the WebSocket parser into here into the verb
     * handler — and core0's 4KB stack has core1's directly below it,
     * not a guard page. core0 is single-threaded and the line is
     * consumed before this returns. */
    static char line[1024 + 1];
    if (len > sizeof(line) - 1) {
        len = sizeof(line) - 1;
    }
    memcpy(line, payload, len);
    line[len] = 0;
    api_handle_text(line, ws_reply_line, c);
}

void http_send_line(void *ctx, const char *line) {
    http_conn_t *c = (http_conn_t *)ctx;
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (&conns[i] != c) {
            continue;
        }
        if (c->is_ws) {
            ws_reply_line(line, c);
        }
        return;
    }
}

/* Every WebSocket client gets every event. The `follow` flag this used to gate
 * on belonged to the old ASCII protocol's FOLLOW ON verb, which nothing in the
 * JSON API sets -- so gating on it sent events precisely nowhere. Per-topic
 * subscription (sub.add) replaces it properly in phase 6. */
void http_broadcast_line(const char *line) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (conns[i].pcb && conns[i].is_ws) {
            ws_reply_line(line, &conns[i]);
        }
    }
}

int http_client_count(void) {
    int n = 0;
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (conns[i].pcb && conns[i].is_ws) {
            n++;
        }
    }
    return n;
}

/* Attempts the WS upgrade for a GET /ws request; returns true (and has
 * already written the 101 response, or a failure response) if this
 * request was a WS upgrade attempt at all — the caller must not fall
 * through to normal dispatch() either way. */
static bool try_ws_upgrade(http_conn_t *c, struct tcp_pcb *tpcb, const char *method,
                          const char *path) {
    if (strcmp(method, "GET") != 0 || !path_is(path, WS_PATH)) {
        return false;
    }
    size_t upg_len = 0, key_len = 0, conn_len = 0;
    const char *upg = find_header(c->hdr, "Upgrade", &upg_len);
    const char *key = find_header(c->hdr, "Sec-WebSocket-Key", &key_len);
    const char *conn_hdr = find_header(c->hdr, "Connection", &conn_len);
    bool ok = upg && upg_len == 9 && strncasecmp(upg, "websocket", 9) == 0 && key && key_len > 0 &&
             conn_hdr && header_has_token_ci(conn_hdr, conn_len, "Upgrade");
    if (!ok) {
        http_reply(tpcb, 400, "text/plain", "expected a WebSocket upgrade");
        return true;
    }
    char accept[32];
    ws_accept_key(key, key_len, accept, sizeof(accept));
    char resp[256];
    int rl = snprintf(resp, sizeof(resp),
                      "HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n",
                      accept);
    tcp_write(tpcb, resp, (uint16_t)rl, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
    c->is_ws = true;
    /* Reclaim the slot if the peer vanishes without a close frame -- otherwise
     * eight abandoned clients exhaust HTTP_CONNS and the server stops
     * answering, which presents as "connection closed" on a healthy device. */
    ip_set_option(tpcb, SOF_KEEPALIVE);
    tpcb->keep_idle  = 20000;   /* ms of silence before the first probe */
    tpcb->keep_intvl = 5000;
    tpcb->keep_cnt   = 3;
    c->dropped = 0;
    ws_parser_reset(&c->ws);
    return true;
}

/* --- REST-ish API + static file dispatch ------------------------------- */

/* Same positional contract as json_str_field(), for a bare integer and a
 * bare true/false. Neither existed until the notify API needed numbers;
 * every earlier route took only strings. */
static bool json_int_field(const char *body, const char *key, long *out) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body ? body : "", pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

static bool json_bool_field(const char *body, const char *key, bool *out) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body ? body : "", pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

/* ---- /api/meters -------------------------------------------------------
 *
 * What is plugged in, and which one this session is driving. Two meters
 * on one hub is the ordinary bench case; they are addressed by serial
 * number because that is what is printed on the meter, and because slot
 * indices renumber the moment something is unplugged.
 *
 * Every attached meter is driven at once. "Selecting" one names the
 * default: where an unqualified command goes when it is the only meter
 * attached, and what the legacy single trigger record binds to. It does
 * not gate the live stream or downloads.
 */
/* `wanted`, when non-empty, is a selection that has just been made and
 * that core1 has not yet acted on — a reply built from the roster alone
 * would still name the previous meter and look like the request had
 * failed. */
static bool query_value(const char *path, const char *key, char *dst, size_t cap) {
    const char *q = strchr(path, '?');
    if (q == NULL) {
        return false;
    }
    size_t klen = strlen(key);
    for (const char *p = q + 1; p && *p;) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t n = strcspn(v, "&");
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(dst, v, n);
            dst[n] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

#if CONFIG_NET_WIFI
/* ---------------------------------------------------------------------------
 * Wi-Fi API -- GET/PUT /api/wifi, scan, known networks, connect.
 *
 * Kept on plain HTTP rather than moved to the WebSocket with everything else,
 * because this is how the device is provisioned: needing the JSON API up in
 * order to configure the network you reach the JSON API over is a loop worth
 * avoiding. Phase 7 additionally exposes wifi.* through the module registry.
 */
static int wifi_routes(struct tcp_pcb *tpcb, const char *method, const char *path,
                       const char *body) {
    char ssid[CFG_SSID_MAX + 1];
    char psk[CFG_PSK_MAX + 1];

    if (path_is(path, "/api/wifi")) {
        if (strcmp(method, "GET") == 0) {
            wifi_status_json(json_buf, sizeof(json_buf));
            return http_reply(tpcb, 200, "application/json", json_buf);
        }
        if (strcmp(method, "PUT") == 0) {
            bool ok = true;
            char mode[8];
            if (json_str_field(body, "mode", mode, sizeof(mode))) {
                ok = cfg_set_mode(mode);
            }
            /* An absent key means "leave it alone"; an empty string is a real
             * value (an open AP), which is why these are separate calls. */
            bool have_ssid = json_str_field(body, "ap_ssid", ssid, sizeof(ssid));
            bool have_psk = json_str_field(body, "ap_psk", psk, sizeof(psk));
            if (ok && (have_ssid || have_psk)) {
                ok = cfg_set_ap(have_ssid ? ssid : NULL, have_psk ? psk : NULL);
            }
            if (!ok) {
                return http_reply(tpcb, 400, "application/json",
                                  "{\"ok\":false,\"error\":\"invalid value\"}");
            }
            wifi_apply_mode();
            wifi_status_json(json_buf, sizeof(json_buf));
            return http_reply(tpcb, 200, "application/json", json_buf);
        }
        return http_reply(tpcb, 400, "application/json",
                          "{\"ok\":false,\"error\":\"bad method\"}");
    }

    if (path_is(path, "/api/wifi/scan")) {
        if (strcmp(method, "POST") == 0) {
            if (!wifi_scan_start()) {
                return http_reply(tpcb, 409, "application/json",
                                  "{\"ok\":false,\"error\":\"scan busy or radio down\"}");
            }
            return http_reply(tpcb, 200, "application/json",
                              "{\"ok\":true,\"scanning\":true}");
        }
        wifi_scan_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }

    if (path_is(path, "/api/wifi/networks")) {
        if (strcmp(method, "PUT") == 0) {
            if (!json_str_field(body, "ssid", ssid, sizeof(ssid))) {
                return http_reply(tpcb, 400, "application/json",
                                  "{\"ok\":false,\"error\":\"ssid required\"}");
            }
            bool have_psk = json_str_field(body, "psk", psk, sizeof(psk));
            if (!cfg_net_put(ssid, have_psk ? psk : NULL)) {
                return http_reply(tpcb, 409, "application/json",
                                  "{\"ok\":false,\"error\":\"invalid or store full\"}");
            }
            return http_reply(tpcb, 200, "application/json", "{\"ok\":true}");
        }
        wifi_networks_json(json_buf, sizeof(json_buf));
        return http_reply(tpcb, 200, "application/json", json_buf);
    }

    if (path_is(path, "/api/wifi/forget") && strcmp(method, "POST") == 0) {
        if (!json_str_field(body, "ssid", ssid, sizeof(ssid))) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"ssid required\"}");
        }
        if (!cfg_net_forget(ssid)) {
            return http_reply(tpcb, 404, "application/json",
                              "{\"ok\":false,\"error\":\"not found\"}");
        }
        return http_reply(tpcb, 200, "application/json", "{\"ok\":true}");
    }

    if (path_is(path, "/api/wifi/connect") && strcmp(method, "POST") == 0) {
        if (!json_str_field(body, "ssid", ssid, sizeof(ssid))) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"ssid required\"}");
        }
        bool have_psk = json_str_field(body, "psk", psk, sizeof(psk));
        if (!wifi_connect(ssid, have_psk ? psk : NULL)) {
            return http_reply(tpcb, 400, "application/json",
                              "{\"ok\":false,\"error\":\"invalid ssid or password\"}");
        }
        return http_reply(tpcb, 200, "application/json",
                          "{\"ok\":true,\"connecting\":true}");
    }

    return -1;
}
#endif /* CONFIG_NET_WIFI */

#if CONFIG_SD
/* ---- the SD card: /api/sd (src/sd/sd_api.h) --------------------------------
 * Ported from km003c_rp2_webapp's http.c with the same routes and answers:
 *   GET    /api/sd[?reset=1]          status JSON
 *   POST   /api/sd/format {fs,cluster}
 *   POST   /api/sd/bench {mb}
 *   GET    /api/sd/files[/<path>]     a file, or a folder's listing
 *   GET    /sd[/<path>]               a page: .gz preferred, a folder's index.html
 *   PUT    /api/sd/files/<path>       upload (parents made); <path>/ = mkdir
 *   DELETE /api/sd/files/<path>
 *   POST   /api/sd/rename {from,to}
 * A transfer holds its connection while the storage worker does the card
 * I/O; http_sd_pump() (main loop) moves it along. An upload is held as
 * pbufs and acknowledged only as the worker takes the bytes -- real TCP
 * back-pressure, unlike /api/fs's PUT, so a slow card slows the sender
 * instead of stalling the loop. */

static err_t sd_http_err(struct tcp_pcb *tpcb, const struct sd_reply *r) {
    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\",\"card\":\"%s\"}", r->error,
             r->card ? r->card : sd_card_state());
    char hdr[400];
    char ra[32] = "";
    if (r->retry_after > 0) {
        snprintf(ra, sizeof(ra), "Retry-After: %d\r\n", r->retry_after);
    }
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n%s"
                      "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
                      "Access-Control-Allow-Methods: %s\r\nConnection: close\r\n\r\n",
                      r->status, status_text(r->status), (unsigned)strlen(body), ra, cors_methods());
    tcp_write(tpcb, hdr, (uint16_t)hl, TCP_WRITE_FLAG_COPY);
    tcp_write(tpcb, body, (uint16_t)strlen(body), TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
    tcp_close(tpcb);
    return ERR_OK;
}

static uint32_t json_u32_field(const char *body, const char *key) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body ? body : "", pat);
    if (!p) {
        return 0;
    }
    p = strchr(p + strlen(pat), ':');
    return p ? (uint32_t)strtoul(p + 1, NULL, 10) : 0;
}

/* Request paths, percent-encoded as they arrive (sd_path_clean decodes);
 * one request is parsed at a time. */
static char s_sdp1[192], s_sdp2[192];

/* Hold a connection on transfer x. */
static err_t sd_hold_conn(http_conn_t *c, int x, enum sd_op op) {
    c->sd_on = true;
    c->sd_x = (uint8_t)x;
    c->sd_op = (uint8_t)op;
    c->retries = 0;
    return ERR_OK;      /* held: http_sd_pump answers */
}

/* /sd, /sd/, /sd?..., /sd/<path>. Not /sdk or any longer sibling. */
static bool sd_public_prefix(const char *path) {
    return strncmp(path, "/sd", 3) == 0 &&
           (path[3] == 0 || path[3] == '/' || path[3] == '?' || path[3] == '#');
}

/* GET /sd and GET /sd/<path> serve a page the way LittleFS does. name.gz is
 * preferred and sent with Content-Encoding: gzip. A directory, including
 * the card root, serves index.html. Writes and folder listings stay on
 * /api/sd/files. */
static int sd_public_get(http_conn_t *c, struct tcp_pcb *tpcb, const char *method, const char *path) {
    if (strcmp(method, "GET") != 0) {
        return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"GET\"}");
    }
    static struct sd_reply r;
    memset(&r, 0, sizeof(r));
    if (!sd_api_gate(&r)) {
        return sd_http_err(tpcb, &r);
    }
    char *p1 = s_sdp1;
    p1[0] = 0;
    /* "/sd" and "/sd/" name the card root. Anything after the slash is the path. */
    if (path[3] == '/') {
        const char *rest = path + 4;
        if (*rest && *rest != '?' && *rest != '#') {
            if (!copy_path_token(rest, p1, sizeof(s_sdp1))) {
                return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad path\"}");
            }
        }
    }
    if (!sd_path_clean(p1, sizeof(s_sdp1))) {
        return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad path\"}");
    }
    int x = sd_xfer_start(SD_OP_GET, p1, NULL, 0, &r);
    if (x < 0) {
        return sd_http_err(tpcb, &r);
    }
    sd_xfer_as_site(x);
    return sd_hold_conn(c, x, SD_OP_GET);
}

/* /api/sd, /api/sd/format, /api/sd/bench, /api/sd/files[/...] (GET,
 * DELETE), /api/sd/rename. PUT is taken in http_recv, as its body streams.
 * -1 = not ours. */
static int sd_routes(http_conn_t *c, struct tcp_pcb *tpcb, const char *method, const char *path,
                     const char *body) {
    static struct sd_reply r;
    memset(&r, 0, sizeof(r));
    if (path_is(path, "/api/sd")) {
        if (sd_api_status_json(json_buf, sizeof(json_buf)) < 0) {
            return http_reply(tpcb, 500, "application/json", "{\"ok\":false,\"error\":\"status too long\"}");
        }
        if (strstr(path, "reset=1")) {
            sd_api_stats_reset();
        }
        return http_reply(tpcb, 200, "application/json", json_buf);
    }
    if (path_is(path, "/api/sd/format")) {
        if (strcmp(method, "POST") != 0) {
            return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"POST\"}");
        }
        char fs[12] = "";
        json_str_field(body, "fs", fs, sizeof(fs));
        if (!sd_format_start(fs, json_u32_field(body, "cluster"), &r)) {
            return sd_http_err(tpcb, &r);
        }
        return http_reply(tpcb, 202, "application/json", "{\"ok\":true}");
    }
    if (path_is(path, "/api/sd/bench")) {
        if (strcmp(method, "POST") != 0) {
            return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"POST\"}");
        }
        if (!sd_bench_start(json_u32_field(body, "mb"), &r)) {
            return sd_http_err(tpcb, &r);
        }
        return http_reply(tpcb, 202, "application/json", "{\"ok\":true}");
    }
    bool rename = path_is(path, "/api/sd/rename");
    bool files = strncmp(path, "/api/sd/files", 13) == 0 &&
                 (path[13] == 0 || path[13] == '/' || path[13] == '?' || path[13] == '#');
    if (!rename && !files) {
        return -1;
    }
    if (!sd_api_gate(&r)) {
        return sd_http_err(tpcb, &r);
    }
    char *p1 = s_sdp1, *p2 = s_sdp2;
    p1[0] = p2[0] = 0;
    enum sd_op op;
    if (rename) {
        if (strcmp(method, "POST") != 0 || !json_str_field(body, "from", p1, sizeof(s_sdp1)) ||
            !json_str_field(body, "to", p2, sizeof(s_sdp2)) || !sd_path_clean(p1, sizeof(s_sdp1)) ||
            !sd_path_clean(p2, sizeof(s_sdp2)) || !p1[0] || !p2[0]) {
            return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad path\"}");
        }
        op = SD_OP_RENAME;
    } else {
        if (path[13] == '/' && !copy_path_token(path + 14, p1, sizeof(s_sdp1))) {
            p1[0] = 0; /* "/api/sd/files/" = the root */
        }
        if (!sd_path_clean(p1, sizeof(s_sdp1))) {
            return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad path\"}");
        }
        if (strcmp(method, "GET") == 0) {
            op = SD_OP_GET;
        } else if (strcmp(method, "DELETE") == 0) {
            op = SD_OP_DELETE;
        } else {
            return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"GET, PUT or DELETE\"}");
        }
    }
    int x = sd_xfer_start(op, p1, p2, 0, &r);
    if (x < 0) {
        return sd_http_err(tpcb, &r);
    }
    return sd_hold_conn(c, x, op);
}

/* PUT /api/sd/files/<path>: from http_recv once the headers are in. A
 * trailing '/' makes a folder. Returns an err_t the caller passes on; the
 * pbuf is the caller's unless *took_p. */
static err_t sd_put_begin(http_conn_t *c, struct tcp_pcb *tpcb, const char *path, struct pbuf *p, size_t offset,
                          bool *took_p) {
    static struct sd_reply sr;
    char *sp = s_sdp1;
    memset(&sr, 0, sizeof(sr));
    *took_p = false;
    if (!sd_api_gate(&sr)) {
        return sd_http_err(tpcb, &sr);
    }
    size_t tok = strcspn(path, "?#");
    bool mkdir = tok > 14 && path[tok - 1] == '/';
    if (!copy_path_token(path + 14, sp, sizeof(s_sdp1)) || !sd_path_clean(sp, sizeof(s_sdp1)) || !sp[0]) {
        return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad path\"}");
    }
    enum sd_op op = mkdir ? SD_OP_MKDIR : SD_OP_PUT;
    int x = sd_xfer_start(op, sp, NULL, mkdir ? 0 : (uint64_t)c->content_len, &sr);
    if (x < 0) {
        return sd_http_err(tpcb, &sr);
    }
    sd_hold_conn(c, x, op);
    if (mkdir) {
        return ERR_OK;
    }
    c->taking_sd_body = true;
    c->sd_put_got = 0;
    if (offset < p->tot_len) {
        /* The body's first bytes came with the headers, and this pbuf was
         * acknowledged already. */
        c->sd_hold = p;
        c->sd_hold_off = (uint16_t)offset;
        c->sd_acked_ahead = (uint32_t)(p->tot_len - offset);
        *took_p = true;
    }
    return ERR_OK;
}

/* Hands held upload bytes to the worker and acknowledges what it took. The
 * hold is a pbuf chain; c->sd_hold_off is how far into it the worker is. */
static void sd_feed(http_conn_t *c) {
    if (!c->sd_hold) {
        return;
    }
    size_t took = 0; /* bytes of the chain consumed, from sd_hold_off */
    struct pbuf *q = c->sd_hold;
    size_t off = c->sd_hold_off;
    while (q && off >= q->len) {
        off -= q->len;
        q = q->next;
    }
    for (; q; q = q->next, off = 0) {
        size_t len = q->len - off;
        size_t left = c->content_len - c->sd_put_got;
        size_t want = len < left ? len : left; /* past Content-Length: consumed, ignored */
        size_t k = want ? sd_xfer_put(c->sd_x, (const uint8_t *)q->payload + off, want) : 0;
        c->sd_put_got += k;
        if (k < want) {
            took += k;
            break; /* the worker's buffer is full */
        }
        took += len;
    }
    if (!took) {
        return;
    }
    /* Acknowledge it, less what the header pbuf already acknowledged. */
    size_t pre = took < c->sd_acked_ahead ? took : c->sd_acked_ahead;
    c->sd_acked_ahead -= (uint32_t)pre;
    for (size_t ack = took - pre; ack;) {
        uint16_t n = ack > 0xFFFF ? 0xFFFF : (uint16_t)ack;
        tcp_recved(c->pcb, n);
        ack -= n;
    }
    /* Free the pbufs at the front that are wholly consumed. */
    size_t pos = c->sd_hold_off + took;
    while (c->sd_hold && pos >= c->sd_hold->len) {
        struct pbuf *head = c->sd_hold;
        struct pbuf *next = head->next;
        pos -= head->len;
        if (next) {
            pbuf_ref(next); /* survives the dechain below */
            pbuf_dechain(head);
        }
        pbuf_free(head);
        c->sd_hold = next;
    }
    c->sd_hold_off = (uint16_t)pos;
}

/* Moves one connection's transfer along. Sends nothing it cannot fit. */
static void sd_service(http_conn_t *c) {
    struct tcp_pcb *tpcb = c->pcb;
    int x = c->sd_x;
    enum sd_xstate st = sd_xfer_state(x);
    c->retries = 0; /* the worker is on it; lwIP notices a vanished client */
    if (c->sd_op == SD_OP_PUT && st == SDX_STREAM) {
        sd_feed(c);
        if (!c->sd_hold && c->sd_put_got >= c->content_len && !c->sd_in_ended) {
            sd_xfer_end_input(x);
            c->sd_in_ended = true;
        }
        return;
    }
    if (c->sd_op == SD_OP_GET && (st == SDX_STREAM || st == SDX_DONE || (st == SDX_FAIL && c->sd_hdr_sent))) {
        if (!c->sd_hdr_sent) {
            char hdr[480], cl[40] = "";
            uint64_t size = sd_xfer_size(x);
            if (size != UINT64_MAX) {
                snprintf(cl, sizeof(cl), "Content-Length: %llu\r\n", (unsigned long long)size);
            }
            const char *enc = sd_xfer_gzip(x) ? "Content-Encoding: gzip\r\n" : "";
            int hl = snprintf(hdr, sizeof(hdr),
                              "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n%s%sCache-Control: no-store\r\n"
                              "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: %s\r\n"
                              "Connection: close\r\n\r\n",
                              sd_xfer_mime(x), enc, cl, cors_methods());
            if (hl <= 0 || (size_t)hl >= sizeof(hdr) || tcp_sndbuf(tpcb) < hl ||
                tcp_write(tpcb, hdr, (uint16_t)hl, TCP_WRITE_FLAG_COPY) != ERR_OK) {
                return;
            }
            c->sd_hdr_sent = true;
        }
        const uint8_t *p;
        size_t n;
        while ((n = sd_xfer_peek(x, &p)) > 0) {
            u16_t room = tcp_sndbuf(tpcb);
            if (room < 64) {
                break;
            }
            if (n > room) {
                n = room;
            }
            if (tcp_write(tpcb, p, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
                break;
            }
            sd_xfer_consume(x, n);
        }
        tcp_output(tpcb);
        if (st == SDX_FAIL) {
            /* Headers are out (200 + Content-Length): cut the body short so
             * the client sees a short read, never wrong data. */
            conn_cleanup(c);
            conn_reset(c);
            tcp_abort(tpcb);
            return;
        }
        if (st == SDX_DONE && sd_xfer_peek(x, &p) == 0) {
            sd_xfer_release(x);
            c->sd_on = false;
            http_try_close(c, tpcb);
        }
        return;
    }
    if (st == SDX_DONE) {
        if (c->sd_op == SD_OP_PUT) {
            snprintf(json_buf, sizeof(json_buf), "{\"ok\":true,\"bytes\":%llu}",
                     (unsigned long long)sd_xfer_size(x));
            http_reply(tpcb, 200, "application/json", json_buf);
        } else if (c->sd_op == SD_OP_MKDIR) {
            http_reply(tpcb, 200, "application/json", "{\"ok\":true,\"dir\":true}");
        } else {
            http_reply(tpcb, 200, "application/json", "{\"ok\":true}");
        }
        conn_cleanup(c); /* releases the transfer */
        conn_reset(c);
        return;
    }
    if (st == SDX_FAIL) {
        static struct sd_reply r;
        sd_xfer_result(x, &r);
        sd_http_err(tpcb, &r);
        conn_cleanup(c);
        conn_reset(c);
    }
}

void http_sd_pump(void) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        if (conns[i].pcb && conns[i].sd_on) {
            sd_service(&conns[i]);
        }
    }
}
#else
void http_sd_pump(void) {
}
#endif

/* POST /api/op {"op":..,"params":{..}} -> the WebSocket's reply, as 200.
 * The same dispatcher (built-ins, then Lua-registered ops); the reply is
 * captured instead of framed. Op handlers that look at their connection
 * (http_send_line, http_conn_dropped) get NULL, which they treat as "not a
 * WebSocket" already. */
static bool s_op_replied;
static void op_capture(const char *line, void *ctx) {
    (void)ctx;
    if (s_op_replied) { return; }      /* one reply per request */
    snprintf(json_buf, sizeof(json_buf), "%s", line);
    s_op_replied = true;
}

static err_t op_bridge(struct tcp_pcb *tpcb, const char *body) {
    s_op_replied = false;
    api_handle_text(body ? body : "", op_capture, NULL);
    if (!s_op_replied) {
        return http_reply(tpcb, 500, "application/json",
                          "{\"ok\":false,\"error\":{\"code\":\"no_reply\",\"message\":\"the op did not answer\"}}");
    }
    return http_reply(tpcb, 200, "application/json", json_buf);
}

static int dispatch(struct tcp_pcb *tpcb, http_conn_t *c, const char *method, const char *path,
                    const char *body, size_t body_len) {
    (void)body_len;
    if (strcmp(method, "OPTIONS") == 0) {
        return http_reply(tpcb, 204, "text/plain", "");
    }
    if (path_is(path, "/api/op")) {
        if (strcmp(method, "POST") != 0) {
            return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"POST\"}");
        }
        return op_bridge(tpcb, body);
    }
#if CONFIG_SD
    if (strncmp(path, "/api/sd", 7) == 0 && (path[7] == 0 || path[7] == '/' || path[7] == '?')) {
        int rc = sd_routes(c, tpcb, method, path, body);
        if (rc >= 0) {
            return rc;
        }
    }
    /* Before LittleFS static lookup, so /sd/foo is the card and not flash
     * object sd/foo. /sdk is left for static. */
    if (sd_public_prefix(path)) {
        return sd_public_get(c, tpcb, method, path);
    }
#endif
    if (path_is(path, "/api/fs/rename") && strcmp(method, "POST") == 0) {
        /* The path buffers below are static for the reason given on
         * ws_on_text's line buffer: dispatch() has a branch per route
         * and the compiler coalesces their locals into one frame, which
         * made this the largest thing on core0's HTTP path. Each is
         * filled and used within its own branch, and core0 is
         * single-threaded. */
        static char from[FS_PATH_MAX + 1];
        static char to[FS_PATH_MAX + 1];
        if (!json_str_field(body, "from", from, sizeof(from)) ||
            !json_str_field(body, "to", to, sizeof(to)) ||
            !fs_valid_path(from) || !fs_valid_path(to)) {
            return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad name\"}");
        }
        int err = fs_rename(from, to);
        if (err == 1) {
            return http_reply(tpcb, 409, "application/json", "{\"ok\":false,\"error\":\"exists\"}");
        }
        if (err < 0) {
            if (!fs_stat(from, NULL)) {
                return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
            }
            return http_reply(tpcb, 500, "application/json", "{\"ok\":false,\"error\":\"rename failed\"}");
        }
        return http_reply(tpcb, 200, "application/json", "{\"ok\":true}");
    }
    if (path_is(path, "/api/tar") && strcmp(method, "POST") == 0) {
        return http_start_tar_post(c, tpcb, body, body_len);
    }
    if (path_is(path, "/api/fs/remove") && strcmp(method, "POST") == 0) {
        return http_start_remove(c, tpcb, body, body_len);
    }
    if (strncmp(path, "/api/tar", 8) == 0 && (path[8] == 0 || path[8] == '/' || path[8] == '?')
        && strcmp(method, "GET") == 0) {
        return http_start_tar(c, tpcb, path);
    }
    if (strncmp(path, "/api/fs", 7) == 0 && strcmp(method, "GET") == 0) {
        if (path_is(path, "/api/fs") || path_is(path, "/api/fs/")) {
            if (fs_list_json("", json_buf, sizeof(json_buf)) < 0) {
                return http_reply(tpcb, 500, "application/json", "{\"ok\":false,\"error\":\"fs unavailable\"}");
            }
            return http_reply(tpcb, 200, "application/json", json_buf);
        }
        static char name[FS_PATH_MAX + 1];
        if (!fs_api_name(path, name, sizeof(name))) {
            return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad name\"}");
        }
        if (fs_is_dir(name)) {
            if (fs_list_json(name, json_buf, sizeof(json_buf)) < 0) {
                return http_reply(tpcb, 500, "application/json", "{\"ok\":false,\"error\":\"fs unavailable\"}");
            }
            return http_reply(tpcb, 200, "application/json", json_buf);
        }
        if (!fs_valid_path(name) || !fs_stat(name, NULL)) {
            return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
        }
        return http_start_file(c, tpcb, name, ends_with(name, ".gz"));
    }
#if CONFIG_NET_WIFI
    if (strncmp(path, "/api/wifi", 9) == 0) {
        int rc = wifi_routes(tpcb, method, path, body);
        if (rc >= 0) {
            return rc;
        }
        /* fall through to 404 for an unknown /api/wifi/... path */
    }
#endif

    if (strcmp(method, "DELETE") == 0) {
        static char name[FS_PATH_MAX + 1];
        if (!fs_api_name(path, name, sizeof(name)) || !fs_valid_path(name)) {
            return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad name\"}");
        }
        if (!fs_stat(name, NULL) && !fs_is_dir(name)) {
            return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
        }
        int del = fs_delete(name);
        if (del == FS_ERR_NOTEMPTY) {
            return http_reply(tpcb, 409, "application/json", "{\"ok\":false,\"error\":\"folder is not empty\"}");
        }
        if (del < 0) {
            return http_reply(tpcb, 500, "application/json", "{\"ok\":false,\"error\":\"delete failed\"}");
        }
        return http_reply(tpcb, 200, "application/json", "{\"ok\":true}");
    }
    if (strcmp(method, "GET") == 0) {
#if CONFIG_NET_WIFI
        /* Read-only for now — the rest of the WiFi API (scan, known
         * networks, mode) is Phase 2. This one exists early because it is
         * the only way to see whether the radio came up on a board whose
         * UART console isn't wired. */
        if (path_is(path, "/api/wifi")) {
            int n = wifi_status_json(json_buf, sizeof(json_buf));
            if (n <= 0 || (size_t)n >= sizeof(json_buf)) {
                return http_reply(tpcb, 500, "application/json",
                                  "{\"ok\":false,\"error\":\"status too long\"}");
            }
            return http_reply(tpcb, 200, "application/json", json_buf);
        }
#endif
        if (path_is(path, "/api/debug/crash")) {
            /* Reads core1_ticks/fault state directly (core0 memory reads,
             * no core1 round trip) so this keeps working even while core1
             * is hard-hung — see src/diag.h for what this reports and why
             * it exists. */
            int n = snprintf(json_buf, sizeof(json_buf), "uptime_ms=%lu\n",
                             (unsigned long)to_ms_since_boot(get_absolute_time()));
            if (n > 0 && (size_t)n < sizeof(json_buf)) {
                /* NCM counters first: when NCM is the thing that failed,
                 * this report is only reachable over WiFi, and they say
                 * whether the device stopped receiving, stopped
                 * transmitting, or was never asked. */
                int u = usb_ncm_debug_report(json_buf + n, sizeof(json_buf) - (size_t)n);
                if (u > 0 && (size_t)(n + u) < sizeof(json_buf)) {
                    n += u;
                }
            }
#if MEM_STATS && MEMP_STATS
            {
                /* lwIP's own heap and the two pools that gate the WebSocket
                 * stream. `max` is the high-water mark since boot; `err` is
                 * allocations refused. MEM_SIZE headroom is max vs avail. */
                size_t used = strlen(json_buf);
                snprintf(json_buf + used, sizeof(json_buf) - used,
                         "lwip mem: used=%lu max=%lu avail=%lu err=%lu | tcp_seg: used=%lu max=%lu avail=%lu err=%lu | "
                         "pbuf_pool: used=%lu max=%lu avail=%lu err=%lu\n",
                         (unsigned long)lwip_stats.mem.used, (unsigned long)lwip_stats.mem.max,
                         (unsigned long)lwip_stats.mem.avail, (unsigned long)lwip_stats.mem.err,
                         (unsigned long)lwip_stats.memp[MEMP_TCP_SEG]->used, (unsigned long)lwip_stats.memp[MEMP_TCP_SEG]->max,
                         (unsigned long)lwip_stats.memp[MEMP_TCP_SEG]->avail, (unsigned long)lwip_stats.memp[MEMP_TCP_SEG]->err,
                         (unsigned long)lwip_stats.memp[MEMP_PBUF_POOL]->used, (unsigned long)lwip_stats.memp[MEMP_PBUF_POOL]->max,
                         (unsigned long)lwip_stats.memp[MEMP_PBUF_POOL]->avail, (unsigned long)lwip_stats.memp[MEMP_PBUF_POOL]->err);
            }
#endif
            return http_reply(tpcb, 200, "text/plain", json_buf);
        }
        char req[FS_PATH_MAX + 1];
        static char name[FS_PATH_MAX + 1];
        bool gzipped = false;
        if (static_req_name(path, req, sizeof(req)) && resolve_static(req, name, sizeof(name), &gzipped)) {
            return http_start_file(c, tpcb, name, gzipped);
        }
        if (path_is(path, "/") || path_is(path, "/index.html")) {
            return http_reply(tpcb, 200, "text/plain",
                              "bluetooth_rp2_webapp\n"
                              "GET /api/fs  GET/PUT/DELETE /api/fs/<path>  POST /api/fs/rename\n"
                              "GET /api/tar/<folder>  POST /api/tar {paths}  POST /api/fs/remove {paths}\n"
                              "PUT /api/fs/<folder>/  (mkdir)\n"
                              "POST /api/op  {\"op\":..,\"params\":{..}} -> the WebSocket reply\n"
#if CONFIG_SD
                              "GET /sd[/<path>]  (a page: index.html, .gz preferred)\n"
                              "GET /api/sd  GET/PUT/DELETE /api/sd/files/<path>  POST /api/sd/rename|format|bench\n"
#endif
                              "WS " WS_PATH "  JSON API, e.g. {\"id\":1,\"op\":\"sys.info\"}\n");
        }
    }
    return http_reply(tpcb, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}\n");
}

static size_t header_content_length(const char *hdr) {
    const char *p = hdr;
    while (*p) {
        if ((p == hdr || p[-1] == '\n') && strncasecmp(p, "Content-Length:", 15) == 0) {
            return (size_t)atoi(p + 15);
        }
        p++;
    }
    return 0;
}

static err_t http_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg;
    (void)len;
    http_conn_t *c = conn_find(tpcb);
    if (!c) {
        return ERR_OK;
    }
    if (c->closing) {
        http_try_close(c, tpcb);
        return ERR_OK;
    }
    if (c->sending_file) {
        return http_finish_file(c, tpcb, http_pump_file(c, tpcb));
    }
    if (c->sending_tar) {
        return http_finish_file(c, tpcb, http_pump_tar(c, tpcb));
    }
    return ERR_OK;
}

static err_t http_poll(void *arg, struct tcp_pcb *tpcb) {
    (void)arg;
    http_conn_t *c = conn_find(tpcb);
    if (!c) {
        return ERR_OK;
    }
    if (!conn_held(c)) {
        return ERR_OK;
    }
    if (c->is_ws) {
        /* No retry budget: a WebSocket may be legitimately silent for minutes.
         * TCP keepalive (armed at upgrade) is what reclaims a peer that has
         * actually gone away. */
        return ERR_OK;
    }
    /* A batch delete holds the connection with no bytes in flight. The 1 Hz
     * poll would otherwise abort it after HTTP_MAX_RETRIES. */
    if (c->sending_remove) {
        c->retries = 0;
    }
    if (c->retries < 255) {
        c->retries++;
    }
    if (c->retries > HTTP_MAX_RETRIES) {
        conn_cleanup(c);
        conn_reset(c);
        tcp_abort(tpcb);
        return ERR_ABRT;
    }
    if (c->pending_file) {
        return http_start_file(c, tpcb, c->fs_name, c->pending_gzip);
    }
    if (c->sending_file) {
        return http_finish_file(c, tpcb, http_pump_file(c, tpcb));
    }
    if (c->sending_tar) {
        return http_finish_file(c, tpcb, http_pump_tar(c, tpcb));
    }
    /* Only a connection already closing gets another tcp_close() try here
     * (the first one answered ERR_MEM). This used to close every held
     * connection that reached this point — which is a recording download
     * mid-stream, or a request still waiting for core1's reply. lwIP polls
     * a connection only after it has seen no segment for the interval, so
     * under a saturated link (a 1000 SPS follower with NCM drops) a
     * download whose ACKs paused for a retransmit was cut off with a
     * truncated body, seemingly at random. TCP recovers from the pause on
     * its own; the retries budget above still ends a transfer that stays
     * silent for HTTP_MAX_RETRIES polls. */
    if (c->closing) {
        http_try_close(c, tpcb);
    }
    return ERR_OK;
}

static err_t http_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    (void)err;
    if (!p) {
#if CONFIG_SD
        http_conn_t *cx = conn_find(tpcb);
        if (cx && cx->sd_on) {
            sd_xfer_note_end(cx->sd_x, "client closed", 0);
        }
#endif
        conn_drop(tpcb);
        tcp_close(tpcb);
        return ERR_OK;
    }
#if CONFIG_SD
    {
        http_conn_t *cx = conn_find(tpcb);
        if (cx && cx->taking_sd_body) {
            /* Held, and acknowledged as the worker takes it (sd_feed). */
            if (cx->sd_hold) {
                pbuf_cat(cx->sd_hold, p);
            } else {
                cx->sd_hold = p;
                cx->sd_hold_off = 0;
            }
            return ERR_OK;
        }
    }
#endif
    tcp_recved(tpcb, p->tot_len);
    http_conn_t *c = conn_for(tpcb);
    if (!c) {
        pbuf_free(p);
        return http_reply(tpcb, 409, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    }

    if (c->is_ws) {
        bool closed = false;
        for (struct pbuf *q = p; q; q = q->next) {
            ws_parser_feed(&c->ws, (const uint8_t *)q->payload, q->len, ws_on_text, c, &closed);
            if (closed) {
                break;
            }
        }
        pbuf_free(p);
        if (closed) {
            conn_drop(tpcb);
            tcp_close(tpcb);
        }
        return ERR_OK;
    }

    size_t offset = 0;
    if (!c->hdr_done) {
        uint16_t space = (uint16_t)(sizeof(c->hdr) - 1 - c->hdr_len);
        uint16_t n = pbuf_copy_partial(p, c->hdr + c->hdr_len, space, 0);
        c->hdr_len = (uint16_t)(c->hdr_len + n);
        c->hdr[c->hdr_len] = 0;
        char *split = strstr(c->hdr, "\r\n\r\n");
        if (!split) {
            pbuf_free(p);
            if (c->hdr_len >= sizeof(c->hdr) - 1) {
                conn_drop(tpcb);
                return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"headers too large\"}");
            }
            return ERR_OK;
        }
        c->hdr_done = true;
        size_t header_bytes = (size_t)(split + 4 - c->hdr);
        size_t hdr_before_this = c->hdr_len - n;
        offset = header_bytes > hdr_before_this ? header_bytes - hdr_before_this : 0;
        c->content_len = header_content_length(c->hdr);

        char method[8] = {0};
        char path[HTTP_PATH] = {0};
#if CONFIG_SD
        sscanf(c->hdr, "%7s %191s", method, path);
#else
        sscanf(c->hdr, "%7s %95s", method, path);
#endif

        if (try_ws_upgrade(c, tpcb, method, path)) {
            /* Any bytes past the header in this same pbuf are the start
             * of the first WS frame — feed them now that c->is_ws is set
             * (or drop the connection if the upgrade attempt failed). */
            if (c->is_ws && offset < p->tot_len) {
                bool closed = false;
                struct pbuf *q = p;
                size_t skip = offset;
                while (q && skip >= q->len) {
                    skip -= q->len;
                    q = q->next;
                }
                if (q) {
                    ws_parser_feed(&c->ws, (const uint8_t *)q->payload + skip, q->len - skip, ws_on_text,
                                  c, &closed);
                    for (q = q->next; q && !closed; q = q->next) {
                        ws_parser_feed(&c->ws, (const uint8_t *)q->payload, q->len, ws_on_text, c, &closed);
                    }
                }
                pbuf_free(p);
                if (closed) {
                    conn_drop(tpcb);
                    tcp_close(tpcb);
                }
                return ERR_OK;
            }
            pbuf_free(p);
            if (!c->is_ws) {
                conn_drop(tpcb);
            }
            return ERR_OK;
        }

#if CONFIG_SD
        if (strcmp(method, "PUT") == 0 && strncmp(path, "/api/sd/files/", 14) == 0) {
            bool took = false;
            err_t e = sd_put_begin(c, tpcb, path, p, offset, &took);
            if (!took) {
                pbuf_free(p);
            }
            if (!c->sd_on) {
                conn_drop(tpcb);
            }
            return e;
        }
#endif
        bool want_fs_put = strcmp(method, "PUT") == 0 && strncmp(path, "/api/fs/", 8) == 0;
        bool want_post_body = c->content_len > 0 && !want_fs_put &&
                              (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 ||
                               strcmp(method, "DELETE") == 0);
        bool big_post = want_post_body && strcmp(method, "POST") == 0 &&
                        (path_is(path, "/api/op") || path_is(path, "/api/tar") ||
                         path_is(path, "/api/fs/remove"));
        /* PUT /api/fs/<path>/ (a trailing slash) makes a folder: what an
         * archive restore needs for an empty one. */
        size_t tok = strcspn(path, "?#");
        if (want_fs_put && tok > 8 && path[tok - 1] == '/') {
            pbuf_free(p);
            conn_drop(tpcb);
            if (!fs_api_name(path, c->fs_name, sizeof(c->fs_name)) || fs_mkdir(c->fs_name) < 0) {
                return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"could not make the folder\"}");
            }
            return http_reply(tpcb, 200, "application/json", "{\"ok\":true,\"dir\":true}");
        }
        if (want_fs_put) {
            if (!fs_api_name(path, c->fs_name, sizeof(c->fs_name)) || !fs_valid_path(c->fs_name)) {
                pbuf_free(p);
                conn_drop(tpcb);
                return http_reply(tpcb, 400, "application/json", "{\"ok\":false,\"error\":\"bad name\"}");
            }
            if (c->content_len > FS_FILE_MAX) {
                pbuf_free(p);
                conn_drop(tpcb);
                return http_reply(tpcb, 413, "application/json", "{\"ok\":false,\"error\":\"file too large\"}");
            }
            if (fs_begin_write(c->fs_name) < 0) {
                pbuf_free(p);
                conn_drop(tpcb);
                return http_reply(tpcb, 409, "application/json", "{\"ok\":false,\"error\":\"fs write busy or failed\"}");
            }
            c->taking_fs_body = true;
            c->body_got = 0;
        } else if (big_post) {
            if (c->content_len > OP_BODY_MAX) {
                pbuf_free(p);
                conn_drop(tpcb);
                if (path_is(path, "/api/op")) {
                    return http_reply(tpcb, 413, "application/json",
                                      "{\"ok\":false,\"error\":{\"code\":\"too_large\",\"message\":\"body over 4096 bytes\"}}");
                }
                return http_reply(tpcb, 413, "application/json", "{\"ok\":false,\"error\":\"body too large\"}");
            }
            c->op_body = op_alloc(c->content_len + 1, &c->op_gen);
            if (!c->op_body) {
                pbuf_free(p);
                conn_drop(tpcb);
                return http_reply(tpcb, 503, "application/json",
                                  "{\"ok\":false,\"error\":{\"code\":\"busy\",\"message\":\"no memory for the body\"}}");
            }
            snprintf(c->method, sizeof(c->method), "%s", method);
            snprintf(c->path, sizeof(c->path), "%s", path);
            c->taking_post_body = true;
            c->body_got = 0;
        } else if (want_post_body) {
            if (c->content_len >= sizeof(c->post_body)) {
                pbuf_free(p);
                conn_drop(tpcb);
                return http_reply(tpcb, 413, "application/json", "{\"ok\":false,\"error\":\"body too large\"}");
            }
            snprintf(c->method, sizeof(c->method), "%s", method);
            snprintf(c->path, sizeof(c->path), "%s", path);
            c->taking_post_body = true;
            c->body_got = 0;
        } else {
            const char *body = split + 4;
            err_t derr = dispatch(tpcb, c, method, path, body, strlen(body));
            pbuf_free(p);
            if (derr == ERR_ABRT) {
                return ERR_ABRT;
            }
            if (!conn_held(c)) {
                conn_drop(tpcb);
            }
            return derr;
        }
    }

    if (c->taking_fs_body) {
        while (offset < p->tot_len && c->body_got < c->content_len) {
            size_t room = c->content_len - c->body_got;
            if (room > sizeof(file_chunk)) {
                room = sizeof(file_chunk);
            }
            uint16_t chunk = pbuf_copy_partial(p, file_chunk, (uint16_t)room, (uint16_t)offset);
            if (!chunk) {
                break;
            }
            if (fs_write(file_chunk, chunk) < 0) {
                pbuf_free(p);
                fs_abort_write();
                if (c->fs_name[0]) {
                    fs_delete(c->fs_name);
                }
                c->taking_fs_body = false;
                conn_drop(tpcb);
                return http_reply(tpcb, 500, "application/json", "{\"ok\":false,\"error\":\"write failed\"}");
            }
            c->body_got += chunk;
            offset += chunk;
        }
        pbuf_free(p);
        if (c->body_got >= c->content_len) {
            static char name[FS_PATH_MAX + 1];
            memcpy(name, c->fs_name, sizeof(name));
            size_t len = c->content_len;
            c->taking_fs_body = false;
            int err2 = fs_end_write();
            conn_drop(tpcb);
            if (err2 < 0) {
                fs_delete(name);
                return http_reply(tpcb, 500, "application/json", "{\"ok\":false,\"error\":\"close failed\"}");
            }
            snprintf(json_buf, sizeof(json_buf), "{\"ok\":true,\"name\":\"%s\",\"bytes\":%u}", name, (unsigned)len);
            return http_reply(tpcb, 200, "application/json", json_buf);
        }
        return ERR_OK;
    }

    if (c->taking_post_body) {
        char *dst = c->op_body ? c->op_body : c->post_body;
        while (offset < p->tot_len && c->body_got < c->content_len) {
            size_t room = c->content_len - c->body_got;
            uint16_t chunk = pbuf_copy_partial(p, dst + c->body_got, (uint16_t)room, (uint16_t)offset);
            if (!chunk) {
                break;
            }
            c->body_got += chunk;
            offset += chunk;
        }
        pbuf_free(p);
        if (c->body_got >= c->content_len) {
            dst[c->content_len] = 0;
            c->taking_post_body = false;
            err_t derr = dispatch(tpcb, c, c->method, c->path, dst, c->content_len);
            if (derr == ERR_ABRT) {
                return ERR_ABRT;
            }
            if (!conn_held(c)) {
                conn_drop(tpcb);
            }
        }
        return ERR_OK;
    }

    pbuf_free(p);
    return ERR_OK;
}

static void http_err(void *arg, err_t err) {
    (void)err;
    /* lwIP has already freed the pcb by the time this runs, so there is
     * nothing to close -- just release our slot. */
    conn_drop((struct tcp_pcb *)arg);
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    (void)err;
    tcp_arg(newpcb, newpcb);
    tcp_nagle_disable(newpcb);
    tcp_recv(newpcb, http_recv);
    tcp_sent(newpcb, http_sent);
    tcp_poll(newpcb, http_poll, HTTP_POLL_INT);
    tcp_err(newpcb, http_err);
    return ERR_OK;
}

void http_server_start(void) {
    for (int i = 0; i < HTTP_CONNS; i++) {
        conn_reset(&conns[i]);
    }
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    tcp_bind(pcb, IP_ANY_TYPE, 80);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, http_accept);
}
