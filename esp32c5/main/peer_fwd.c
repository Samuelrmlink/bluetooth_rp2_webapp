/* One outgoing websocket per peer mDNS name.
 *
 * The browser keeps the socket it opened to this board. A text frame whose
 * first key is "node" is for another board: node is removed, the id is
 * replaced with one this board owns, and the frame is written on a raw
 * client websocket to that peer. The peer runs it locally because the
 * frame has no node. Replies come back on that socket; this board puts
 * node back and restores the browser id, and sends that only to the
 * browser connection that asked. Events are broadcast to browser sockets.
 * A frame that comes back with a leading node is dropped.
 *
 * Two names, eight calls. A call is freed when its reply arrives, when
 * its socket closes, or when about 10 s pass with no reply. One 1024-byte
 * line is held only while a handshake is in progress. The tcpip thread
 * must not write an httpd socket, so finished text waits in a short queue
 * drained on the httpd task. Dial failures abort the pcb (it does not stay
 * in SYN_SENT) and do not retry inside that attempt.
 */
#include "peer_fwd.h"

#include "mdns_adv.h"
#include "net/peer_fwd_msg.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"

static const char *TAG = "peer";

#define NFWD 2
#define NMAP 8
#define DIAL_US (300 * 1000)
#define CALL_US (10 * 1000 * 1000)
#define RX 1280
#define PEND 1024
#define HDR 240
#define ERR_N NMAP
#define ERR_LINE 256
#define BIG_N 3
#define BIG_LINE (RX + 96)

enum { ST_SYN = 1, ST_HS = 2, ST_UP = 3 };

typedef struct {
    bool used;
    uint8_t state;
    uint8_t bad;
    char name[64];
    char ip[16];
    uint16_t port;
    struct tcp_pcb *pcb;
    int64_t t0;
    char hdr[HDR];
    uint16_t hdr_len;
    uint8_t ph[8];
    uint8_t ph_got;
    uint8_t ph_need;
    uint8_t opcode;
    uint8_t in_pay;
    uint16_t pay_len;
    uint16_t pay_got;
    char pay[RX + 1];
} slot_t;

typedef struct {
    bool used;
    uint8_t slot;
    uint8_t firmware;
    int fd;
    uintptr_t epoch;
    long browser_id;
    uint16_t peer_id;
    int64_t t0;
    peer_fwd_on_reply fn;
    void *user;
} map_t;

typedef struct {
    int fd;
    uintptr_t epoch;
    peer_fwd_on_reply fn;
    void *user;
    char line[ERR_LINE];
} err_item_t;

typedef struct {
    int fd;
    uintptr_t epoch;
    peer_fwd_on_reply fn;
    void *user;
    char line[BIG_LINE];
} big_item_t;

static slot_t slots[NFWD];
static map_t maps[NMAP];
static uint16_t next_id = 1;
static httpd_handle_t srv;

/* Held only while one handshake has not yet reached " 101 ". */
static char pend[PEND];
static int pend_on;
static int pend_si;
static uint16_t pend_peer;

static err_item_t errs[ERR_N];
static int err_head, err_n;
static big_item_t bigs[BIG_N];
static int big_head, big_n;
static int out_posted;
static int draining;
static char sending[BIG_LINE];
static char scratch[PEND];
static uint8_t wire[PEND + 8];

static void core_lock(void) { LOCK_TCPIP_CORE(); }
static void core_unlock(void) { UNLOCK_TCPIP_CORE(); }

static int64_t mono(void) { return esp_timer_get_time(); }

static void epoch_noop(void *ctx) { (void)ctx; }

static uintptr_t epoch_of(int fd) {
    void *ctx;
    static uintptr_t next = 1;
    uintptr_t e;
    if (!srv || fd < 0) return 0;
    ctx = httpd_sess_get_ctx(srv, fd);
    if (ctx) return (uintptr_t)ctx;
    e = next++;
    if (next == 0) next = 1;
    httpd_sess_set_ctx(srv, fd, (void *)e, epoch_noop);
    return e;
}

static void send_line(int fd, uintptr_t epoch, const char *line) {
    httpd_ws_frame_t frame;
    int fds[6];
    size_t nfd = 6;
    size_t i;
    if (!srv || !line) return;
    memset(&frame, 0, sizeof frame);
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)line;
    frame.len = strlen(line);
    if (fd >= 0) {
        if (!epoch) return;
        if (httpd_ws_get_fd_info(srv, fd) != HTTPD_WS_CLIENT_WEBSOCKET) return;
        if ((uintptr_t)httpd_sess_get_ctx(srv, fd) != epoch) return;
        httpd_ws_send_frame_async(srv, fd, &frame);
        return;
    }
    /* max_open_sockets is 6. Peer pcbs are not httpd sockets. */
    if (httpd_get_client_list(srv, &nfd, fds) != ESP_OK) return;
    for (i = 0; i < nfd; i++) {
        if (httpd_ws_get_fd_info(srv, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
            httpd_ws_send_frame_async(srv, fds[i], &frame);
    }
}

static void reply_now(int fd, uintptr_t epoch, const char *name, long id,
                      const char *code, const char *msg) {
    char line[ERR_LINE];
    snprintf(line, sizeof line,
             "{\"node\":\"%s\",\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
             name ? name : "", id, code ? code : "error", msg ? msg : "");
    send_line(fd, epoch, line);
}

static int err_push(int fd, uintptr_t epoch, peer_fwd_on_reply fn, void *user, const char *line) {
    err_item_t *e;
    size_t n;
    if (!line) return 0;
    n = strlen(line);
    if (n >= ERR_LINE) n = ERR_LINE - 1;
    if (err_n >= ERR_N) {
        err_head = (err_head + 1) % ERR_N;
        err_n--;
    }
    e = &errs[(err_head + err_n) % ERR_N];
    e->fd = fd;
    e->epoch = epoch;
    e->fn = fn;
    e->user = user;
    memcpy(e->line, line, n);
    e->line[n] = 0;
    err_n++;
    return 1;
}

static big_item_t *big_reserve(void) {
    if (big_n >= BIG_N && bigs[big_head].fd < 0 && !bigs[big_head].fn) {
        big_head = (big_head + 1) % BIG_N;
        big_n--;
    }
    if (big_n >= BIG_N) return NULL;
    return &bigs[(big_head + big_n) % BIG_N];
}

static void big_commit(big_item_t *b) {
    if (b) big_n++;
}

static void post_out(void);
static void out_drain(void);

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *after_leading_id(const char *text, long *id) {
    const char *p;
    const char *n;
    long v = 0;
    int neg = 0;
    if (!text) return NULL;
    p = skip_ws(text);
    if (*p != '{') return NULL;
    p = skip_ws(p + 1);
    if (strncmp(p, "\"id\"", 4) != 0) return NULL;
    p = skip_ws(p + 4);
    if (*p != ':') return NULL;
    p = skip_ws(p + 1);
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return NULL;
    n = p;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    if (p == n) return NULL;
    if (id) *id = neg ? -v : v;
    return p;
}

static int rewrite_id(const char *text, long id, char *dst, size_t cap) {
    const char *rest = after_leading_id(text, NULL);
    int n;
    if (!rest || !dst) return -1;
    n = snprintf(dst, cap, "{\"id\":%ld%s", id, rest);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

static int host_ok(const char *s) {
    size_t i, n;
    if (!s) return 0;
    n = strlen(s);
    if (!n || n > 63 || s[0] == '-') return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

/* Masked client text. Same layout as ws_encode_client_text. */
static size_t encode_client(const uint8_t *payload, size_t len, uint8_t *out, size_t cap, uint32_t mask) {
    size_t hdr;
    size_t i;
    uint8_t m[4];
    if (len <= 125) hdr = 6;
    else if (len <= 0xFFFF) hdr = 8;
    else return 0;
    if (cap < hdr + len) return 0;
    out[0] = 0x81;
    if (len <= 125) {
        out[1] = (uint8_t)(0x80 | len);
        m[0] = out[2] = (uint8_t)(mask >> 24);
        m[1] = out[3] = (uint8_t)(mask >> 16);
        m[2] = out[4] = (uint8_t)(mask >> 8);
        m[3] = out[5] = (uint8_t)mask;
    } else {
        out[1] = (uint8_t)(0x80 | 126);
        out[2] = (uint8_t)(len >> 8);
        out[3] = (uint8_t)len;
        m[0] = out[4] = (uint8_t)(mask >> 24);
        m[1] = out[5] = (uint8_t)(mask >> 16);
        m[2] = out[6] = (uint8_t)(mask >> 8);
        m[3] = out[7] = (uint8_t)mask;
    }
    for (i = 0; i < len; i++) out[hdr + i] = payload[i] ^ m[i & 3];
    return hdr + len;
}

static int id_in_use(uint16_t id) {
    int i;
    for (i = 0; i < NMAP; i++) {
        if (maps[i].used && maps[i].peer_id == id) return 1;
    }
    return 0;
}

static int map_room(void) {
    int i;
    for (i = 0; i < NMAP; i++) if (!maps[i].used) return 1;
    return 0;
}

static map_t *map_add(int si, int fd, uintptr_t epoch, long browser_id, int firmware,
                      peer_fwd_on_reply fn, void *user) {
    int i, guard;
    uint16_t id = 0;
    map_t *m = NULL;
    for (i = 0; i < NMAP; i++) {
        if (!maps[i].used) { m = &maps[i]; break; }
    }
    if (!m) return NULL;
    for (guard = 0; guard < NMAP + 2; guard++) {
        id = next_id++;
        if (next_id == 0) next_id = 1;
        if (!id_in_use(id)) break;
    }
    if (id_in_use(id)) return NULL;
    m->used = true;
    m->slot = (uint8_t)si;
    m->firmware = firmware ? 1 : 0;
    m->fd = fd;
    m->epoch = epoch;
    m->browser_id = browser_id;
    m->peer_id = id;
    m->t0 = mono();
    m->fn = fn;
    m->user = user;
    return m;
}

static map_t *map_find(int si, uint16_t peer_id) {
    int i;
    for (i = 0; i < NMAP; i++) {
        if (maps[i].used && maps[i].slot == (uint8_t)si && maps[i].peer_id == peer_id)
            return &maps[i];
    }
    return NULL;
}

static void push_fail(map_t *m, const char *name, const char *code, const char *msg) {
    char line[ERR_LINE];
    int n;
    if (!m) return;
    if (m->firmware) {
        if (!m->fn) return;
        n = snprintf(line, sizeof line,
                     "{\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                     m->browser_id, code, msg);
        if (n > 0 && n < ERR_LINE) err_push(-1, 0, m->fn, m->user, line);
        return;
    }
    n = snprintf(line, sizeof line,
                 "{\"node\":\"%s\",\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                 name ? name : "", m->browser_id, code, msg);
    if (n > 0 && n < ERR_LINE) err_push(m->fd, m->epoch, NULL, NULL, line);
}

static slot_t *find_name(const char *name) {
    int i;
    for (i = 0; i < NFWD; i++) {
        if (slots[i].used && strcasecmp(slots[i].name, name) == 0) return &slots[i];
    }
    return NULL;
}

static slot_t *alloc_slot(void) {
    int i;
    for (i = 0; i < NFWD; i++) if (!slots[i].used) return &slots[i];
    return NULL;
}

static err_t slot_fail(slot_t *s, const char *why) {
    struct tcp_pcb *pcb;
    char name[64];
    int si, i;
    if (!s || !s->used) return ERR_OK;
    si = (int)(s - slots);
    snprintf(name, sizeof name, "%s", s->name);
    pcb = s->pcb;
    s->pcb = NULL;
    s->used = false;
    if (pend_on && pend_si == si) pend_on = 0;
    for (i = 0; i < NMAP; i++) {
        if (!maps[i].used || maps[i].slot != (uint8_t)si) continue;
        push_fail(&maps[i], name, "peer_connect", why);
        maps[i].used = false;
    }
    memset(s, 0, sizeof *s);
    ESP_LOGW(TAG, "%s: %s", name[0] ? name : "?", why);
    if (!pcb) return ERR_OK;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    tcp_sent(pcb, NULL);
    tcp_abort(pcb);
    return ERR_ABRT;
}

static int write_masked(slot_t *s, const char *line) {
    size_t n;
    size_t len;
    if (!s->pcb || !line) return 0;
    len = strlen(line);
    if (len > PEND) return 0;
    n = encode_client((const uint8_t *)line, len, wire, sizeof wire,
                      (uint32_t)mono() ^ 0xA5C3u);
    if (!n) return 0;
    if (tcp_write(s->pcb, wire, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) return 0;
    tcp_output(s->pcb);
    return 1;
}

static void send_pending(slot_t *s) {
    map_t *m;
    int si;
    if (!pend_on || pend_si != (int)(s - slots)) return;
    si = pend_si;
    pend_on = 0;
    m = map_find(si, pend_peer);
    if (!write_masked(s, pend)) {
        if (m) {
            push_fail(m, s->name, "busy", "telemetry has the link");
            m->used = false;
        }
        return;
    }
    if (m) m->t0 = mono();
}

static void deliver(slot_t *s);

static void frame_done(slot_t *s) {
    uint8_t op = s->opcode;
    s->in_pay = 0;
    s->ph_got = 0;
    s->ph_need = 0;
    if (op == 0x8) {
        s->bad = 1;
        return;
    }
    if (op == 0x1) deliver(s);
    s->pay_got = 0;
}

/* Server frames are unmasked. A mask bit, a 64-bit length, or an oversize
 * payload kills the slot. The mask bit is not required. */
static void parse_feed(slot_t *s, const uint8_t *d, size_t n) {
    size_t i;
    for (i = 0; i < n && !s->bad; i++) {
        if (!s->in_pay) {
            if (s->ph_need == 0) s->ph_need = 2;
            s->ph[s->ph_got++] = d[i];
            if (s->ph_got < s->ph_need) continue;
            if (s->ph_need == 2) {
                uint8_t len7 = s->ph[1] & 0x7f;
                if (s->ph[1] & 0x80) { s->bad = 1; return; }
                s->opcode = s->ph[0] & 0x0f;
                if (len7 == 127) { s->bad = 1; return; }
                if (len7 == 126) { s->ph_need = 4; continue; }
                s->pay_len = len7;
            } else {
                s->pay_len = (uint16_t)((s->ph[2] << 8) | s->ph[3]);
            }
            s->ph_got = 0;
            s->ph_need = 0;
            if (s->pay_len > RX) { s->bad = 1; return; }
            s->pay_got = 0;
            if (!s->pay_len) frame_done(s);
            else s->in_pay = 1;
            continue;
        }
        s->pay[s->pay_got++] = (char)d[i];
        if (s->pay_got >= s->pay_len) frame_done(s);
    }
}

static void take_bytes(slot_t *s, const uint8_t *d, size_t n) {
    size_t i = 0;
    if (s->state != ST_UP) {
        while (i < n && s->state != ST_UP && !s->bad) {
            if (s->hdr_len + 1 >= HDR) { s->bad = 1; return; }
            s->hdr[s->hdr_len++] = (char)d[i++];
            s->hdr[s->hdr_len] = 0;
            if (s->hdr_len >= 4 && memcmp(s->hdr + s->hdr_len - 4, "\r\n\r\n", 4) == 0) {
                if (!strstr(s->hdr, " 101 ")) { s->bad = 1; return; }
                s->state = ST_UP;
                s->hdr_len = 0;
                send_pending(s);
            }
        }
    }
    if (s->state == ST_UP && i < n && !s->bad) parse_feed(s, d + i, n - i);
}

static void deliver(slot_t *s) {
    char ignore[64];
    long peer_id = 0;
    int kind;
    map_t *m;
    big_item_t *b;
    s->pay[s->pay_got] = 0;
    if (peer_msg_node(s->pay, ignore, sizeof ignore) == 1) return;
    kind = peer_msg_kind(s->pay, &peer_id);
    if (kind == 2) {
        b = big_reserve();
        if (!b) return;
        if (peer_msg_tag(s->pay, s->name, 0, 0, b->line, sizeof b->line) < 0) return;
        b->fd = -1;
        b->epoch = 0;
        b->fn = NULL;
        b->user = NULL;
        big_commit(b);
        return;
    }
    if (kind != 1) return;
    m = map_find((int)(s - slots), (uint16_t)peer_id);
    if (!m) return;
    if (m->firmware && !m->fn) {
        m->used = false;
        return;
    }
    b = big_reserve();
    if (!b) return;
    if (m->firmware) {
        if (rewrite_id(s->pay, m->browser_id, b->line, sizeof b->line) < 0) {
            size_t n = strlen(s->pay);
            if (n >= sizeof b->line) return;
            memcpy(b->line, s->pay, n + 1);
        }
        b->fd = -1;
        b->epoch = 0;
        b->fn = m->fn;
        b->user = m->user;
    } else if (peer_msg_tag(s->pay, s->name, m->browser_id, 1, b->line, sizeof b->line) < 0) {
        push_fail(m, s->name, "peer_connect", "too large");
        m->used = false;
        return;
    } else {
        b->fd = m->fd;
        b->epoch = m->epoch;
        b->fn = NULL;
        b->user = NULL;
    }
    big_commit(b);
    m->used = false;
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    slot_t *s = arg;
    struct pbuf *q;
    (void)err;
    if (!s || !s->used) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (!p) return slot_fail(s, "connection failed");
    for (q = p; q && !s->bad; q = q->next) {
        take_bytes(s, (const uint8_t *)q->payload, q->len);
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    if (s->used && s->bad) return slot_fail(s, "connection failed");
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    slot_t *s = arg;
    (void)err;
    if (!s || !s->used) return;
    s->pcb = NULL;
    slot_fail(s, "connection failed");
}

static err_t on_poll(void *arg, struct tcp_pcb *tpcb) {
    slot_t *s = arg;
    (void)tpcb;
    if (!s || !s->used || s->state == ST_UP || s->state == 0) return ERR_OK;
    if (mono() - s->t0 >= DIAL_US) return slot_fail(s, "connection failed");
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    slot_t *s = arg;
    static char req[240];
    int n;
    if (!s || !s->used) return ERR_OK;
    if (err != ERR_OK) return slot_fail(s, "connection failed");
    s->state = ST_HS;
    tcp_recv(tpcb, on_recv);
    n = snprintf(req, sizeof req,
                 "GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
                 s->ip);
    if (n < 0 || n >= (int)sizeof req ||
        tcp_write(tpcb, req, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        return slot_fail(s, "connection failed");
    }
    tcp_output(tpcb);
    return ERR_OK;
}

static int dial(slot_t *s) {
    ip_addr_t dst;
    struct tcp_pcb *pcb;
    if (!ipaddr_aton(s->ip, &dst) || !IP_IS_V4(&dst)) return 0;
    pcb = tcp_new();
    if (!pcb) return 0;
    tcp_arg(pcb, s);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 1);
    s->pcb = pcb;
    s->t0 = mono();
    s->state = ST_SYN;
    if (tcp_connect(pcb, &dst, s->port, on_connected) != ERR_OK) {
        s->pcb = NULL;
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        tcp_abort(pcb);
        return 0;
    }
    ESP_LOGI(TAG, "dial %s %s:%u", s->name, s->ip, (unsigned)s->port);
    return 1;
}

static void sync_browser(int fd, uintptr_t epoch, const char *name, long id,
                         const char *code, const char *msg) {
    reply_now(fd, epoch, name, id, code, msg);
    out_drain();
}

/* Lock is not held. 1 = browser frame consumed, or firmware fn will run. */
static int submit(const char *name, const char *text, int browser, int fd, uintptr_t epoch,
                  long caller_id, peer_fwd_on_reply fn, void *user, const char **err) {
    char ip[16];
    uint16_t port = 80;
    int created = 0;
    int si;
    slot_t *s;
    map_t *m;
    const char *code = "error";
    const char *msg = "failed";

    core_lock();
    s = find_name(name);
    if (!s) {
        core_unlock();
        if (!mdns_adv_peer(name, ip, sizeof ip, &port)) {
            mdns_adv_browse_kick();
            code = "peer_connect";
            msg = "peer is not on this network";
            goto sync;
        }
        if (!port) port = 80;
        core_lock();
        s = find_name(name);
        if (!s) {
            if (pend_on) {
                core_unlock();
                code = "busy";
                msg = "peer is connecting";
                goto sync;
            }
            if (!map_room()) {
                core_unlock();
                code = "busy";
                msg = "too many calls in flight";
                goto sync;
            }
            s = alloc_slot();
            if (!s) {
                core_unlock();
                code = "peer_connect";
                msg = "no free peer socket";
                goto sync;
            }
            memset(s, 0, sizeof *s);
            s->used = true;
            snprintf(s->name, sizeof s->name, "%s", name);
            snprintf(s->ip, sizeof s->ip, "%s", ip);
            s->port = port;
            created = 1;
        }
    }
    if (!created && s->state != ST_UP) {
        core_unlock();
        code = "busy";
        msg = "peer is connecting";
        goto sync;
    }
    if (!map_room()) {
        if (created) s->used = false;
        core_unlock();
        code = "busy";
        msg = "too many calls in flight";
        goto sync;
    }
    si = (int)(s - slots);
    m = map_add(si, fd, epoch, caller_id, browser ? 0 : 1, fn, user);
    if (!m) {
        if (created) s->used = false;
        core_unlock();
        code = "busy";
        msg = "too many calls in flight";
        goto sync;
    }
    if ((browser ? peer_msg_forward(text, m->peer_id, scratch, sizeof scratch)
                 : rewrite_id(text, (long)m->peer_id, scratch, sizeof scratch)) < 0) {
        m->used = false;
        if (created) s->used = false;
        core_unlock();
        code = "bad_request";
        msg = "too large";
        goto sync;
    }
    if (s->state == ST_UP) {
        if (!write_masked(s, scratch)) {
            push_fail(m, s->name, "busy", "telemetry has the link");
            m->used = false;
        } else {
            m->t0 = mono();
        }
        core_unlock();
        if (browser) out_drain();
        else post_out();
        return 1;
    }
    memcpy(pend, scratch, strlen(scratch) + 1);
    pend_on = 1;
    pend_si = si;
    pend_peer = m->peer_id;
    if (created && !dial(s)) slot_fail(s, "connection failed");
    core_unlock();
    if (browser) out_drain();
    else post_out();
    return 1;

sync:
    if (browser) {
        sync_browser(fd, epoch, name, caller_id, code, msg);
        return 1;
    }
    if (err) *err = msg;
    return 0;
}

static void out_drain(void) {
    if (draining) return;
    draining = 1;
    for (;;) {
        int fd = -1;
        uintptr_t epoch = 0;
        peer_fwd_on_reply fn = NULL;
        void *user = NULL;
        core_lock();
        if (err_n) {
            err_item_t *e = &errs[err_head];
            fd = e->fd;
            epoch = e->epoch;
            fn = e->fn;
            user = e->user;
            snprintf(sending, sizeof sending, "%s", e->line);
            err_head = (err_head + 1) % ERR_N;
            err_n--;
        } else if (big_n) {
            big_item_t *b = &bigs[big_head];
            fd = b->fd;
            epoch = b->epoch;
            fn = b->fn;
            user = b->user;
            snprintf(sending, sizeof sending, "%s", b->line);
            big_head = (big_head + 1) % BIG_N;
            big_n--;
        } else {
            out_posted = 0;
            core_unlock();
            break;
        }
        core_unlock();
        if (fn) fn(sending, user);
        else send_line(fd, epoch, sending);
    }
    draining = 0;
}

static void out_work(void *arg) {
    (void)arg;
    out_drain();
}

static void post_out(void) {
    int post = 0;
    core_lock();
    if ((err_n || big_n) && !out_posted && srv) {
        out_posted = 1;
        post = 1;
    }
    core_unlock();
    if (!post) return;
    if (httpd_queue_work(srv, out_work, NULL) != ESP_OK) {
        core_lock();
        out_posted = 0;
        core_unlock();
    }
}

void peer_fwd_bind(httpd_handle_t server) { srv = server; }

bool peer_fwd_handle(const char *text, int browser_fd) {
    char name[64];
    long browser_id = 0;
    uintptr_t epoch;
    int nr;
    if (!text) return false;
    nr = peer_msg_node(text, name, sizeof name);
    if (nr == 0) return false;
    epoch = epoch_of(browser_fd);
    if (nr < 0) {
        sync_browser(browser_fd, epoch, "", 0, "bad_request", "bad node");
        return true;
    }
    if (strcasecmp(name, mdns_adv_hostname()) == 0) return false;
    if (peer_msg_request_id(text, &browser_id) != 1) {
        sync_browser(browser_fd, epoch, name, 0, "bad_request", "missing \"id\"");
        return true;
    }
    submit(name, text, 1, browser_fd, epoch, browser_id, NULL, NULL, NULL);
    return true;
}

bool peer_fwd_call(const char *name, const char *local_json, peer_fwd_on_reply fn, void *user,
                   const char **why) {
    char probe[64];
    long id = 0;
    const char *err = "bad node";
    int nr;
    if (why) *why = "bad node";
    if (!name || !local_json || !host_ok(name)) return false;
    if (strcasecmp(name, mdns_adv_hostname()) == 0) {
        if (why) *why = "that name is this board";
        return false;
    }
    nr = peer_msg_node(local_json, probe, sizeof probe);
    if (nr != 0) return false;
    if (!after_leading_id(local_json, &id)) {
        if (why) *why = "missing \"id\"";
        return false;
    }
    if (!submit(name, local_json, 0, -1, 0, id, fn, user, &err)) {
        if (why) *why = err ? err : "connection failed";
        return false;
    }
    if (why) *why = NULL;
    return true;
}

void peer_fwd_poll(void) {
    int64_t now = mono();
    int i;
    core_lock();
    for (i = 0; i < NFWD; i++) {
        slot_t *s = &slots[i];
        if (!s->used || s->state == ST_UP || s->state == 0) continue;
        if (now - s->t0 >= DIAL_US) slot_fail(s, "connection failed");
    }
    for (i = 0; i < NMAP; i++) {
        map_t *m = &maps[i];
        const char *name = "";
        if (!m->used) continue;
        if (m->slot < NFWD && slots[m->slot].used && slots[m->slot].state != ST_UP) continue;
        if (now - m->t0 < CALL_US) continue;
        if (m->slot < NFWD && slots[m->slot].used) name = slots[m->slot].name;
        push_fail(m, name, "peer_connect", "no reply");
        m->used = false;
    }
    core_unlock();
    post_out();
}
