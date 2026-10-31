/* Reverse proxy so a browser on this board's Wi-Fi or USB link can manage
 * another BTGW that was found by mDNS on a different interface. /ws/peer/<name>
 * bridges websocket frames. /api/peer/<name>/<rest> forwards the HTTP request
 * to that board's /<rest>. Two at a time. */
#include "net/peer_proxy.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "net/http.h"
#include "net/mdns.h"
#include "net/ws.h"

#define SLOTS 2
#define HDR 640

typedef struct slot {
    bool used;
    bool ws;
    bool ws_up;
    struct tcp_pcb *front;
    struct tcp_pcb *back;
    struct pbuf *to_back;
    struct pbuf *to_front;
    uint16_t back_off;
    uint16_t front_off;
    uint32_t preacked;
    char hdr[HDR];
    uint16_t hdr_len;
    uint16_t hdr_off;
    char wsbuf[384];
    uint16_t wslen;
} slot_t;

static slot_t slots[SLOTS];

bool peer_proxy_wants(const char *path) {
    return path && (strncmp(path, "/ws/peer/", 9) == 0 || strncmp(path, "/api/peer/", 10) == 0);
}

static void url_decode(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '%' && r[1] && r[2]) {
            unsigned v = 0;
            if (sscanf(r + 1, "%2x", &v) == 1) { *w++ = (char)v; r += 2; continue; }
        }
        *w++ = *r;
    }
    *w = 0;
}

static bool split_name(const char *path, char *name, size_t ncap, const char **rest, bool *ws) {
    const char *p;
    if (strncmp(path, "/ws/peer/", 9) == 0) { *ws = true; p = path + 9; *rest = "/ws"; }
    else if (strncmp(path, "/api/peer/", 10) == 0) { *ws = false; p = path + 10; }
    else return false;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    const char *q = memchr(p, '?', n);
    if (q) n = (size_t)(q - p);
    if (!n || n >= ncap) return false;
    memcpy(name, p, n);
    name[n] = 0;
    url_decode(name);
    if (!*ws) *rest = slash ? slash : "/";
    return name[0] != 0;
}

static const char *hdr_val(const char *hdr, const char *key, size_t *len) {
    size_t k = strlen(key);
    const char *p = hdr;
    while (p && *p) {
        if ((p == hdr || p[-1] == '\n') && strncasecmp(p, key, k) == 0 && p[k] == ':') {
            p += k + 1;
            while (*p == ' ') p++;
            const char *e = strstr(p, "\r\n");
            *len = e ? (size_t)(e - p) : strlen(p);
            return p;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    *len = 0;
    return NULL;
}

static void kill_slot(slot_t *s) {
    if (!s || !s->used) return;
    if (s->to_back) pbuf_free(s->to_back);
    if (s->to_front) pbuf_free(s->to_front);
    struct tcp_pcb *front = s->front;
    struct tcp_pcb *back = s->back;
    memset(s, 0, sizeof *s);
    if (front) {
        tcp_arg(front, NULL);
        tcp_recv(front, NULL);
        tcp_sent(front, NULL);
        tcp_err(front, NULL);
        tcp_poll(front, NULL, 0);
        http_conn_release(front);
        if (tcp_close(front) != ERR_OK) tcp_abort(front);
    }
    if (back) {
        tcp_arg(back, NULL);
        tcp_recv(back, NULL);
        tcp_sent(back, NULL);
        tcp_err(back, NULL);
        if (tcp_close(back) != ERR_OK) tcp_abort(back);
    }
}

static void pump(slot_t *s) {
    if (s->back && s->hdr_off < s->hdr_len) {
        while (s->hdr_off < s->hdr_len && tcp_sndbuf(s->back) > 16) {
            uint16_t n = s->hdr_len - s->hdr_off;
            if (n > tcp_sndbuf(s->back)) n = tcp_sndbuf(s->back);
            if (tcp_write(s->back, s->hdr + s->hdr_off, n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
            s->hdr_off += n;
        }
        tcp_output(s->back);
    }
    if (s->back && s->hdr_off >= s->hdr_len && s->to_back) {
        while (s->to_back && tcp_sndbuf(s->back) > 16) {
            struct pbuf *p = s->to_back;
            uint16_t avail = (uint16_t)(p->len - s->back_off);
            uint16_t n = avail > tcp_sndbuf(s->back) ? tcp_sndbuf(s->back) : avail;
            if (!n || tcp_write(s->back, (uint8_t *)p->payload + s->back_off, n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
            if (s->preacked >= n) s->preacked -= n;
            else {
                tcp_recved(s->front, (uint16_t)(n - s->preacked));
                s->preacked = 0;
            }
            s->back_off += n;
            if (s->back_off >= p->len) {
                s->to_back = p->next;
                p->next = NULL;
                pbuf_free(p);
                s->back_off = 0;
            }
        }
        tcp_output(s->back);
    }
    if (s->front && s->to_front && (!s->ws || s->ws_up)) {
        while (s->to_front && tcp_sndbuf(s->front) > 16) {
            struct pbuf *p = s->to_front;
            uint16_t avail = (uint16_t)(p->len - s->front_off);
            uint16_t n = avail > tcp_sndbuf(s->front) ? tcp_sndbuf(s->front) : avail;
            if (!n || tcp_write(s->front, (uint8_t *)p->payload + s->front_off, n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
            if (s->back) tcp_recved(s->back, n);
            s->front_off += n;
            if (s->front_off >= p->len) {
                s->to_front = p->next;
                p->next = NULL;
                pbuf_free(p);
                s->front_off = 0;
            }
        }
        tcp_output(s->front);
    }
}

static void queue(struct pbuf **head, struct pbuf *p) {
    if (!p) return;
    if (*head) pbuf_cat(*head, p);
    else *head = p;
}

static bool ws_take(slot_t *s, struct pbuf *p) {
    /* Hold the peer's HTTP response until its blank line, then bridge. */
    if (s->ws_up) { queue(&s->to_front, p); return true; }
    uint16_t n = p->tot_len;
    if (s->wslen + n >= sizeof s->wsbuf) { pbuf_free(p); return false; }
    pbuf_copy_partial(p, s->wsbuf + s->wslen, n, 0);
    if (s->back) tcp_recved(s->back, n);
    pbuf_free(p);
    s->wslen += n;
    s->wsbuf[s->wslen] = 0;
    char *end = strstr(s->wsbuf, "\r\n\r\n");
    if (!end) return true;
    if (strncmp(s->wsbuf, "HTTP/1.1 101", 12) != 0) return false;
    s->ws_up = true;
    end += 4;
    uint16_t rest = (uint16_t)(s->wslen - (end - s->wsbuf));
    if (rest) {
        struct pbuf *q = pbuf_alloc(PBUF_RAW, rest, PBUF_RAM);
        if (!q) return false;
        memcpy(q->payload, end, rest);
        queue(&s->to_front, q);
    }
    s->wslen = 0;
    return true;
}

static void front_err(void *arg, err_t err) {
    slot_t *s = arg;
    (void)err;
    if (!s || !s->used) return;
    s->front = NULL;
    kill_slot(s);
}
static void back_err(void *arg, err_t err) {
    slot_t *s = arg;
    (void)err;
    if (!s || !s->used) return;
    s->back = NULL;
    kill_slot(s);
}

static err_t front_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)tpcb; (void)err;
    slot_t *s = arg;
    if (!s || !s->used) { if (p) pbuf_free(p); return ERR_OK; }
    if (!p) { kill_slot(s); return ERR_OK; }
    queue(&s->to_back, p);
    pump(s);
    return ERR_OK;
}

static err_t back_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)tpcb; (void)err;
    slot_t *s = arg;
    if (!s || !s->used) { if (p) pbuf_free(p); return ERR_OK; }
    if (!p) { kill_slot(s); return ERR_OK; }
    if (s->ws && !ws_take(s, p)) { kill_slot(s); return ERR_OK; }
    if (!s->ws) queue(&s->to_front, p);
    pump(s);
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)tpcb; (void)len;
    slot_t *s = arg;
    if (s && s->used) pump(s);
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    slot_t *s = arg;
    if (!s || !s->used) return ERR_OK;
    if (err != ERR_OK) { kill_slot(s); return ERR_OK; }
    s->back = tpcb;
    tcp_recv(tpcb, back_recv);
    tcp_sent(tpcb, on_sent);
    tcp_err(tpcb, back_err);
    pump(s);
    return ERR_OK;
}

static const char *reason_of(int code) {
    if (code == 400) return "Bad Request";
    if (code == 404) return "Not Found";
    if (code == 500) return "Internal Server Error";
    if (code == 503) return "Service Unavailable";
    return "Bad Gateway";
}

static void fail(struct tcp_pcb *pcb, int code, const char *msg) {
    char body[120];
    snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}", msg);
    char buf[280];
    int n = snprintf(buf, sizeof buf,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n"
                     "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n%s",
                     code, reason_of(code), (unsigned)strlen(body), body);
    if (n > 0) tcp_write(pcb, buf, (uint16_t)n, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    tcp_close(pcb);
}

static slot_t *alloc_slot(void) {
    for (int i = 0; i < SLOTS; i++) if (!slots[i].used) return &slots[i];
    return NULL;
}

bool peer_proxy_begin(struct tcp_pcb *front, const char *hdr, const char *method,
                      const char *path, struct pbuf *first, size_t body_off) {
    char name[64];
    const char *rest = "/";
    bool ws = false;
    if (!split_name(path, name, sizeof name, &rest, &ws)) {
        fail(front, 400, "bad peer path");
        return false;
    }
    char ip[16];
    uint16_t port = 80;
    if (!mdns_peer_endpoint(name, ip, sizeof ip, &port)) {
        fail(front, 404, "peer is not on this network");
        return false;
    }
    slot_t *s = alloc_slot();
    if (!s) { fail(front, 503, "proxy busy"); return false; }
    memset(s, 0, sizeof *s);
    s->used = true;
    s->ws = ws;
    s->front = front;
    ip4_addr_t addr;
    if (!ip4addr_aton(ip, &addr)) { s->used = false; fail(front, 404, "peer address is unusable"); return false; }

    if (ws) {
        size_t klen = 0;
        const char *key = hdr_val(hdr, "Sec-WebSocket-Key", &klen);
        if (!key || !klen) { s->used = false; fail(front, 400, "expected a WebSocket upgrade"); return false; }
        char accept[32];
        ws_accept_key(key, klen, accept, sizeof accept);
        char hello[220];
        int hn = snprintf(hello, sizeof hello,
                          "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
        tcp_write(front, hello, (uint16_t)hn, TCP_WRITE_FLAG_COPY);
        tcp_output(front);
        s->hdr_len = (uint16_t)snprintf(s->hdr, sizeof s->hdr,
            "GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", ip);
    } else {
        size_t clen = 0, tlen = 0;
        const char *cl = hdr_val(hdr, "Content-Length", &clen);
        const char *ct = hdr_val(hdr, "Content-Type", &tlen);
        int n = snprintf(s->hdr, sizeof s->hdr, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n",
                         method, rest, ip);
        if (cl && clen && n > 0 && n < (int)sizeof s->hdr - 8)
            n += snprintf(s->hdr + n, sizeof s->hdr - (size_t)n, "Content-Length: %.*s\r\n", (int)clen, cl);
        if (ct && tlen && n > 0 && n < (int)sizeof s->hdr - 8)
            n += snprintf(s->hdr + n, sizeof s->hdr - (size_t)n, "Content-Type: %.*s\r\n", (int)tlen, ct);
        if (n < 0 || n >= (int)sizeof s->hdr - 4) { s->used = false; fail(front, 500, "request too large"); return false; }
        n += snprintf(s->hdr + n, sizeof s->hdr - (size_t)n, "\r\n");
        s->hdr_len = (uint16_t)n;
        if (first && first->tot_len > body_off) {
            uint16_t bl = (uint16_t)(first->tot_len - body_off);
            struct pbuf *q = pbuf_alloc(PBUF_RAW, bl, PBUF_RAM);
            if (!q) { s->used = false; fail(front, 500, "no buffer"); return false; }
            pbuf_copy_partial(first, q->payload, bl, (uint16_t)body_off);
            s->to_back = q;
            s->preacked = bl;
        }
    }
    struct tcp_pcb *back = tcp_new();
    if (!back) { s->used = false; fail(front, 500, "no socket"); return false; }
    tcp_arg(back, s);
    tcp_err(back, back_err);
    if (tcp_connect(back, &addr, port, on_connected) != ERR_OK) {
        /* tcp_abort would call back_err, which closes the browser side. */
        tcp_err(back, NULL);
        tcp_arg(back, NULL);
        tcp_abort(back);
        s->used = false;
        fail(front, 502, "could not connect to the peer");
        return false;
    }
    tcp_arg(front, s);
    tcp_recv(front, front_recv);
    tcp_sent(front, on_sent);
    tcp_err(front, front_err);
    tcp_poll(front, NULL, 0);
    return true;
}
