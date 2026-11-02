/* One outgoing websocket per peer mDNS name.
 *
 * The browser keeps the socket it opened to this board. A text frame whose
 * first key is `node` is for another board: `node` is removed, the id is
 * replaced with one this board owns, and the frame is written on a client
 * websocket to that peer. The peer runs it locally because the frame has
 * no `node`. Replies come back on that same socket; this board puts `node`
 * back and restores the browser's id. Events are broadcast the same way.
 *
 * A dial that is not up in 300 ms is aborted. The pcb is stored before SYN
 * is sent, and received bytes are copied out of the pbuf before it is
 * freed. Two peers, eight calls in flight. This is not the /ws/peer byte
 * bridge, and it does not use that bridge's slots.
 */
#include "net/peer_fwd.h"

#include "net/peer_fwd_msg.h"
#include "net/http.h"
#include "net/mdns.h"
#include "net/ws.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/time.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"

#define NFWD 2
#define NMAP 8
#define DIAL_MS 300
#define RX 1280
#define PEND 1024
#define HDR 240

enum { ST_SYN = 1, ST_HS = 2, ST_UP = 3 };

typedef struct {
    bool used;
    uint8_t state;
    uint8_t bad;
    char name[64];
    char ip[16];
    uint16_t port;
    struct tcp_pcb *pcb;
    uint32_t t0;
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
    bool pend_on;
    void *pend_conn;
    uint32_t pend_epoch;
    long pend_browser;
    uint16_t pend_peer;
    char pend[PEND];
} slot_t;

typedef struct {
    bool used;
    uint8_t slot;
    void *conn;
    uint32_t epoch;
    long browser_id;
    uint16_t peer_id;
} map_t;

static slot_t slots[NFWD];
static map_t maps[NMAP];
static uint16_t next_id = 1;

static uint32_t mono_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void reply_code(void *conn, uint32_t epoch, const char *node, long id,
                       const char *code, const char *msg) {
    static char line[320];
    if (!http_conn_live(conn, epoch)) return;
    snprintf(line, sizeof line,
             "{\"node\":\"%s\",\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
             node ? node : "", id, code, msg);
    http_send_line(conn, line);
}

static int id_in_use(uint16_t id) {
    int i;
    for (i = 0; i < NMAP; i++) {
        if (maps[i].used && maps[i].peer_id == id) return 1;
    }
    return 0;
}

static map_t *map_add(int si, void *conn, uint32_t epoch, long browser_id) {
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
    m->conn = conn;
    m->epoch = epoch;
    m->browser_id = browser_id;
    m->peer_id = id;
    return m;
}

static void map_clear_slot(int si) {
    int i;
    for (i = 0; i < NMAP; i++) {
        if (maps[i].used && maps[i].slot == si) maps[i].used = false;
    }
}

static map_t *map_find(int si, uint16_t peer_id) {
    int i;
    for (i = 0; i < NMAP; i++) {
        if (maps[i].used && maps[i].slot == si && maps[i].peer_id == peer_id) return &maps[i];
    }
    return NULL;
}

static void fail_waiters(int si, const char *name, const char *why) {
    int i;
    for (i = 0; i < NMAP; i++) {
        map_t *m = &maps[i];
        if (!m->used || m->slot != si) continue;
        reply_code(m->conn, m->epoch, name, m->browser_id, "peer_connect", why);
        m->used = false;
    }
}

static void pcb_quiet(struct tcp_pcb *pcb) {
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    tcp_sent(pcb, NULL);
}

static err_t close_out(slot_t *s, struct tcp_pcb *caller, const char *why) {
    struct tcp_pcb *pcb;
    char name[64];
    int si;
    err_t ret = ERR_OK;
    if (!s || !s->used) return ERR_OK;
    si = (int)(s - slots);
    snprintf(name, sizeof name, "%s", s->name);
    pcb = s->pcb;
    s->pcb = NULL;
    s->used = false;
    fail_waiters(si, name, why);
    memset(s, 0, sizeof *s);
    printf("peer fwd %s: %s\n", name[0] ? name : "?", why);
    if (!pcb) return ERR_OK;
    pcb_quiet(pcb);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        if (pcb == caller) ret = ERR_ABRT;
    }
    return ret;
}

static int write_masked(slot_t *s, const char *line) {
    static uint8_t frame[PEND + 8];
    size_t n;
    size_t len = strlen(line);
    if (!s->pcb || len > PEND) return 0;
    n = ws_encode_client_text((const uint8_t *)line, len, frame, sizeof frame,
                              mono_ms() ^ 0xA5C3u);
    if (!n) return 0;
    if (tcp_write(s->pcb, frame, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) return 0;
    tcp_output(s->pcb);
    return 1;
}

static void deliver(slot_t *s) {
    static char tagged[RX + 96];
    long peer_id = 0;
    int kind;
    map_t *m;
    s->pay[s->pay_got] = 0;
    /* A leading node on the way back would be a second hop. Drop it. */
    {
        char ignore[64];
        if (peer_msg_node(s->pay, ignore, sizeof ignore) == 1) return;
    }
    kind = peer_msg_kind(s->pay, &peer_id);
    if (kind == 2) {
        if (peer_msg_tag(s->pay, s->name, 0, 0, tagged, sizeof tagged) < 0) return;
        http_broadcast_line(tagged);
        return;
    }
    if (kind != 1) return;
    m = map_find((int)(s - slots), (uint16_t)peer_id);
    if (!m) return;
    if (peer_msg_tag(s->pay, s->name, m->browser_id, 1, tagged, sizeof tagged) > 0 &&
        http_conn_live(m->conn, m->epoch)) {
        http_send_line(m->conn, tagged);
    }
    m->used = false;
}

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

static void send_pending(slot_t *s) {
    if (!s->pend_on) return;
    if (!write_masked(s, s->pend)) {
        map_t *m = map_find((int)(s - slots), s->pend_peer);
        reply_code(s->pend_conn, s->pend_epoch, s->name, s->pend_browser,
                   "busy", "telemetry has the link");
        if (m) m->used = false;
    }
    s->pend_on = false;
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

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    slot_t *s = arg;
    struct pbuf *q;
    (void)err;
    if (!s || !s->used) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (!p) return close_out(s, tpcb, "connection failed");
    for (q = p; q && !s->bad; q = q->next) {
        take_bytes(s, (const uint8_t *)q->payload, q->len);
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    if (s->used && s->bad) return close_out(s, tpcb, "connection failed");
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    slot_t *s = arg;
    (void)err;
    if (!s || !s->used) return;
    s->pcb = NULL;
    close_out(s, NULL, "connection failed");
}

static err_t on_poll(void *arg, struct tcp_pcb *tpcb) {
    slot_t *s = arg;
    if (!s || !s->used) return ERR_OK;
    if (s->state != ST_UP && (int32_t)(mono_ms() - s->t0) >= DIAL_MS) {
        return close_out(s, tpcb, "connection failed");
    }
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    slot_t *s = arg;
    static char req[220];
    int n;
    if (!s || !s->used) return ERR_OK;
    if (err != ERR_OK) {
        s->pcb = NULL;
        close_out(s, NULL, "connection failed");
        return ERR_OK;
    }
    s->state = ST_HS;
    tcp_recv(tpcb, on_recv);
    n = snprintf(req, sizeof req,
                 "GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
                 s->ip);
    if (n < 0 || n >= (int)sizeof req ||
        tcp_write(tpcb, req, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        return close_out(s, tpcb, "connection failed");
    }
    tcp_output(tpcb);
    return ERR_OK;
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
    for (i = 0; i < NFWD; i++) {
        if (!slots[i].used) return &slots[i];
    }
    return NULL;
}

static int dial(slot_t *s) {
    ip4_addr_t addr;
    struct tcp_pcb *pcb;
    if (!ip4addr_aton(s->ip, &addr)) return 0;
    pcb = tcp_new();
    if (!pcb) return 0;
    tcp_arg(pcb, s);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 1);
    s->pcb = pcb;
    s->t0 = mono_ms();
    s->state = ST_SYN;
    if (tcp_connect(pcb, &addr, s->port, on_connected) != ERR_OK) {
        s->pcb = NULL;
        pcb_quiet(pcb);
        tcp_abort(pcb);
        return 0;
    }
    return 1;
}

static void queue_or_send(slot_t *s, map_t *m, const char *fwd) {
    if (s->state == ST_UP) {
        if (!write_masked(s, fwd)) {
            reply_code(m->conn, m->epoch, s->name, m->browser_id, "busy", "telemetry has the link");
            m->used = false;
        }
        return;
    }
    if (s->pend_on) {
        reply_code(m->conn, m->epoch, s->name, m->browser_id, "busy", "peer is connecting");
        m->used = false;
        return;
    }
    snprintf(s->pend, sizeof s->pend, "%s", fwd);
    s->pend_on = true;
    s->pend_conn = m->conn;
    s->pend_epoch = m->epoch;
    s->pend_browser = m->browser_id;
    s->pend_peer = m->peer_id;
}

bool peer_fwd_handle(const char *text, void *conn) {
    char name[64];
    char ip[16];
    static char fwd[PEND];
    long browser_id = 0;
    uint32_t epoch;
    uint16_t port = 80;
    int nr;
    int si;
    slot_t *s;
    map_t *m;

    nr = peer_msg_node(text, name, sizeof name);
    if (nr == 0) return false;
    epoch = http_conn_epoch(conn);
    if (nr < 0) {
        reply_code(conn, epoch, "", 0, "bad_request", "bad node");
        return true;
    }
    if (strcasecmp(name, mdns_hostname()) == 0) return false;
    if (peer_msg_request_id(text, &browser_id) != 1) {
        reply_code(conn, epoch, name, 0, "bad_request", "missing \"id\"");
        return true;
    }
    s = find_name(name);
    if (!s) {
        if (!mdns_peer_endpoint(name, ip, sizeof ip, &port)) {
            mdns_browse_kick();
            reply_code(conn, epoch, name, browser_id, "peer_connect", "peer is not on this network");
            return true;
        }
        s = alloc_slot();
        if (!s) {
            reply_code(conn, epoch, name, browser_id, "peer_connect", "no free peer socket");
            return true;
        }
        memset(s, 0, sizeof *s);
        s->used = true;
        snprintf(s->name, sizeof s->name, "%s", name);
        snprintf(s->ip, sizeof s->ip, "%s", ip);
        s->port = port ? port : 80;
        si = (int)(s - slots);
        m = map_add(si, conn, epoch, browser_id);
        if (!m || peer_msg_forward(text, m ? m->peer_id : 0, fwd, sizeof fwd) < 0) {
            if (m) m->used = false;
            s->used = false;
            reply_code(conn, epoch, name, browser_id, "peer_connect", "no free peer socket");
            return true;
        }
        queue_or_send(s, m, fwd);
        if (!dial(s)) close_out(s, NULL, "connection failed");
        return true;
    }
    si = (int)(s - slots);
    m = map_add(si, conn, epoch, browser_id);
    if (!m || peer_msg_forward(text, m->peer_id, fwd, sizeof fwd) < 0) {
        if (m) m->used = false;
        reply_code(conn, epoch, name, browser_id, "peer_connect", "no free peer socket");
        return true;
    }
    queue_or_send(s, m, fwd);
    return true;
}

void peer_fwd_poll(void) {
    int i;
    uint32_t now = mono_ms();
    for (i = 0; i < NFWD; i++) {
        slot_t *s = &slots[i];
        if (!s->used || s->state == ST_UP) continue;
        if ((int32_t)(now - s->t0) >= DIAL_MS) close_out(s, NULL, "connection failed");
    }
}
