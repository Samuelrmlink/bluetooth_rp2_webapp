/* Reverse proxy so a browser on this board's Wi-Fi or USB link can manage
 * another BTGW that was found by mDNS on a different interface. /ws/peer/<name>
 * bridges websocket frames. /api/peer/<name>/<rest> forwards the HTTP request
 * to that board's /<rest>. Two at a time.
 *
 * Received bytes are copied into a fixed RAM queue and the pool pbuf is freed
 * before the recv callback returns. Holding the pool pbuf (one buffer per
 * small TCP segment) used to fill all 16 PBUF_POOL entries while the window
 * was still open, and every interface then dropped new frames. A segment that
 * does not fit is refused: lwIP keeps that one pbuf and retries it. The
 * connecting pcb is stored before SYN is sent, so a backend that never
 * answers can be aborted. A stall of about 8 s closes the slot.
 */
#include "net/peer_proxy.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/time.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "net/http.h"
#include "net/mdns.h"
#include "net/net_log.h"
#include "net/ws.h"

#define SLOTS 2
#define HDR 640
#define QMAX 1460
#define STALL_MS 8000
#define POLL_TICKS 4

typedef struct {
    uint8_t data[QMAX];
    uint16_t len;
    uint16_t off;
} q_t;

typedef struct slot {
    bool used;
    bool ws;
    bool ws_up;
    bool back_up;
    bool holding;
    struct tcp_pcb *front;
    struct tcp_pcb *back;
    q_t back_q;
    q_t front_q;
    uint32_t preacked;
    uint32_t stall_ms;
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

static uint32_t mono_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void q_compact(q_t *q) {
    if (!q->off) return;
    uint16_t n = (uint16_t)(q->len - q->off);
    if (n) memmove(q->data, q->data + q->off, n);
    q->len = n;
    q->off = 0;
}

static bool q_put(q_t *q, const struct pbuf *p, uint16_t off, uint16_t n) {
    if (!n) return true;
    q_compact(q);
    if ((uint32_t)q->len + n > QMAX) return false;
    if (pbuf_copy_partial(p, q->data + q->len, n, off) != n) return false;
    q->len = (uint16_t)(q->len + n);
    return true;
}

static void pcb_clear(struct tcp_pcb *pcb) {
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
}

/* Close, or abort when close cannot. ERR_ABRT only when `caller` was aborted,
 * which is the pcb whose callback is running. */
static err_t pcb_shut(struct tcp_pcb *pcb, struct tcp_pcb *caller) {
    pcb_clear(pcb);
    if (tcp_close(pcb) == ERR_OK) return ERR_OK;
    tcp_abort(pcb);
    return pcb == caller ? ERR_ABRT : ERR_OK;
}

static err_t kill_slot(slot_t *s, struct tcp_pcb *caller) {
    if (!s || !s->used) return ERR_OK;
    struct tcp_pcb *front = s->front;
    struct tcp_pcb *back = s->back;
    memset(s, 0, sizeof *s);
    err_t ret = ERR_OK;
    if (front) {
        http_conn_release(front);
        if (pcb_shut(front, caller) == ERR_ABRT) ret = ERR_ABRT;
    }
    if (back && pcb_shut(back, caller) == ERR_ABRT) ret = ERR_ABRT;
    return ret;
}

static void pump_out(struct tcp_pcb *dst, q_t *q, struct tcp_pcb *ack, uint32_t *preacked) {
    if (!dst) return;
    while (q->off < q->len && tcp_sndbuf(dst) > 16) {
        uint16_t avail = (uint16_t)(q->len - q->off);
        uint16_t room = tcp_sndbuf(dst);
        uint16_t n = avail > room ? room : avail;
        if (!n || tcp_write(dst, q->data + q->off, n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        if (preacked && *preacked) {
            if (*preacked >= n) *preacked -= n;
            else {
                if (ack) tcp_recved(ack, (uint16_t)(n - *preacked));
                *preacked = 0;
            }
        } else if (ack) {
            tcp_recved(ack, n);
        }
        q->off = (uint16_t)(q->off + n);
    }
    q_compact(q);
    tcp_output(dst);
}

static void pump(slot_t *s) {
    if (s->back && s->hdr_off < s->hdr_len) {
        while (s->hdr_off < s->hdr_len && tcp_sndbuf(s->back) > 16) {
            uint16_t n = (uint16_t)(s->hdr_len - s->hdr_off);
            if (n > tcp_sndbuf(s->back)) n = tcp_sndbuf(s->back);
            if (tcp_write(s->back, s->hdr + s->hdr_off, n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
            s->hdr_off = (uint16_t)(s->hdr_off + n);
        }
        tcp_output(s->back);
    }
    if (s->back && s->hdr_off >= s->hdr_len)
        pump_out(s->back, &s->back_q, s->front, &s->preacked);
    if (s->front && (!s->ws || s->ws_up))
        pump_out(s->front, &s->front_q, s->back, NULL);
}

static bool pending(const slot_t *s) {
    if (s->back && !s->back_up) return true;
    if (s->hdr_off < s->hdr_len) return true;
    if (s->back_q.len) return true;
    if (s->front_q.len) return true;
    if (s->holding) return true;
    if (s->back && s->back->nrtx > 0) return true;
    return false;
}

static err_t on_poll(void *arg, struct tcp_pcb *tpcb) {
    slot_t *s = arg;
    if (!s || !s->used) return ERR_OK;
    uint16_t bl = s->back_q.len, fl = s->front_q.len, ho = s->hdr_off;
    bool was_up = s->back_up;
    pump(s);
    /* back_up becoming true is the SYN completing. A connection that stays up
     * with nothing moving is not progress, or a retransmit would never expire. */
    bool progress = s->back_q.len != bl || s->front_q.len != fl || s->hdr_off != ho
                    || s->back_up != was_up;
    if (!pending(s) || progress) {
        s->stall_ms = 0;
        return ERR_OK;
    }
    uint32_t now = mono_ms();
    if (!s->stall_ms) {
        s->stall_ms = now ? now : 1;
        return ERR_OK;
    }
    if ((uint32_t)(now - s->stall_ms) < STALL_MS) return ERR_OK;
    net_log("proxy stall slot %d syn %d nrtx %u hdr %u/%u bq %u fq %u hold %d",
            (int)(s - slots), s->back && !s->back_up,
            s->back ? (unsigned)s->back->nrtx : 0,
            (unsigned)s->hdr_off, (unsigned)s->hdr_len,
            (unsigned)s->back_q.len, (unsigned)s->front_q.len, (int)s->holding);
    return kill_slot(s, tpcb);
}

enum { WS_OK = 0, WS_FAIL = 1, WS_MEM = 2 };

static int ws_take(slot_t *s, struct pbuf *p) {
    /* Hold the peer's HTTP response until its blank line, then bridge.
     * The 101 and the first websocket frame often arrive in one segment.
     * Only the header has to fit in wsbuf. The frame is copied into the RAM
     * queue. A segment that does not fit stays with lwIP (one refused pbuf). */
    if (s->ws_up) {
        if (!q_put(&s->front_q, p, 0, p->tot_len)) return WS_MEM;
        pbuf_free(p);
        return WS_OK;
    }
    uint16_t total = p->tot_len;
    uint16_t room = (uint16_t)((sizeof s->wsbuf - 1) - s->wslen);
    uint16_t take = total > room ? room : total;
    uint16_t wslen_before = s->wslen;
    if (take) pbuf_copy_partial(p, s->wsbuf + s->wslen, take, 0);
    s->wslen = (uint16_t)(s->wslen + take);
    s->wsbuf[s->wslen] = 0;
    char *end = strstr(s->wsbuf, "\r\n\r\n");
    if (!end) {
        if (s->back) tcp_recved(s->back, take);
        pbuf_free(p);
        return take == total ? WS_OK : WS_FAIL;
    }
    if (strncmp(s->wsbuf, "HTTP/1.1 101", 12) != 0) {
        if (s->back) tcp_recved(s->back, total);
        pbuf_free(p);
        return WS_FAIL;
    }
    uint16_t hdr_bytes = (uint16_t)((end + 4) - s->wsbuf);
    uint16_t hdr_in_p = hdr_bytes > wslen_before ? (uint16_t)(hdr_bytes - wslen_before) : 0;
    uint16_t frame_len = total > hdr_in_p ? (uint16_t)(total - hdr_in_p) : 0;
    if (frame_len && !q_put(&s->front_q, p, hdr_in_p, frame_len)) {
        s->wslen = wslen_before;
        return WS_MEM;
    }
    s->ws_up = true;
    /* Ack the header only. The frame is acked as pump writes it to the browser,
     * so the window stays closed for bytes we have not delivered. */
    if (s->back && hdr_in_p) tcp_recved(s->back, hdr_in_p);
    pbuf_free(p);
    s->wslen = 0;
    return WS_OK;
}

static void note_hold(slot_t *s, const char *dir, uint16_t len) {
    if (s->holding) return;
    s->holding = true;
    net_log("proxy hold slot %d %s q %u seg %u",
            (int)(s - slots), dir, (unsigned)s->back_q.len + (unsigned)s->front_q.len,
            (unsigned)len);
}

static err_t front_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)err;
    slot_t *s = arg;
    if (!s || !s->used) { if (p) pbuf_free(p); return ERR_OK; }
    if (!p) return kill_slot(s, tpcb);
    pump(s);
    if (!q_put(&s->back_q, p, 0, p->tot_len)) {
        note_hold(s, "back", p->tot_len);
        return ERR_MEM;
    }
    s->holding = false;
    pbuf_free(p);
    pump(s);
    return ERR_OK;
}

static err_t back_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)err;
    slot_t *s = arg;
    if (!s || !s->used) { if (p) pbuf_free(p); return ERR_OK; }
    if (!p) return kill_slot(s, tpcb);
    if (s->ws) {
        int r = ws_take(s, p);
        if (r == WS_MEM) {
            note_hold(s, "front", p->tot_len);
            return ERR_MEM;
        }
        if (r != WS_OK) return kill_slot(s, tpcb);
        s->holding = false;
        pump(s);
        return ERR_OK;
    }
    pump(s);
    if (!q_put(&s->front_q, p, 0, p->tot_len)) {
        note_hold(s, "front", p->tot_len);
        return ERR_MEM;
    }
    s->holding = false;
    pbuf_free(p);
    pump(s);
    return ERR_OK;
}

static void front_err(void *arg, err_t err) {
    slot_t *s = arg;
    (void)err;
    if (!s || !s->used) return;
    s->front = NULL;
    kill_slot(s, NULL);
}

static void back_err(void *arg, err_t err) {
    slot_t *s = arg;
    (void)err;
    if (!s || !s->used) return;
    s->back = NULL;
    kill_slot(s, NULL);
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
    if (err != ERR_OK) return kill_slot(s, tpcb);
    s->back = tpcb;
    s->back_up = true;
    s->stall_ms = 0;
    tcp_recv(tpcb, back_recv);
    tcp_sent(tpcb, on_sent);
    tcp_err(tpcb, back_err);
    tcp_poll(tpcb, on_poll, POLL_TICKS);
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
            uint32_t bl = (uint32_t)(first->tot_len - body_off);
            if (bl > QMAX) { s->used = false; fail(front, 500, "request too large"); return false; }
            pbuf_copy_partial(first, s->back_q.data, (uint16_t)bl, (uint16_t)body_off);
            s->back_q.len = (uint16_t)bl;
            s->preacked = bl;
        }
    }
    struct tcp_pcb *back = tcp_new();
    if (!back) { s->used = false; fail(front, 500, "no socket"); return false; }
    tcp_arg(back, s);
    tcp_recv(back, back_recv);
    tcp_sent(back, on_sent);
    tcp_err(back, back_err);
    tcp_poll(back, on_poll, POLL_TICKS);
    /* Owned before SYN is sent. On SYN_SENT the slot used to leave back NULL,
     * so the connecting pcb and any queued browser bytes could not be aborted. */
    s->back = back;
    if (tcp_connect(back, &addr, port, on_connected) != ERR_OK) {
        /* tcp_abort would call back_err, which closes the browser side. */
        s->back = NULL;
        pcb_clear(back);
        tcp_abort(back);
        s->used = false;
        fail(front, 502, "could not connect to the peer");
        return false;
    }
    tcp_arg(front, s);
    tcp_recv(front, front_recv);
    tcp_sent(front, on_sent);
    tcp_err(front, front_err);
    tcp_poll(front, on_poll, POLL_TICKS);
    return true;
}
