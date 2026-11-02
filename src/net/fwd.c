/*
 * Forwarding rules: TCP proxy + UDP relay. See fwd.h and fwd_core.h.
 *
 * TCP splice. Each direction takes ownership of AT MOST ONE received pbuf
 * chain (`pend`) and writes it to the other side as that side's send buffer
 * allows (tcp_write COPY; tcp_recved only for what was written, so the
 * sender's window closes while the far side is slow). While a chain is
 * pending, further data from that side is refused (ERR_MEM), and lwIP holds
 * it as refused_data -- so memory stays bounded at one chain per direction
 * however fast one side sends and however slow the other drains.
 *
 * FIN is propagated as a half-close (tcp_shutdown TX) once the pending data
 * is written; both sides closed -> both pcbs closed. Any error or RST on one
 * side aborts the other. A target that refuses or does not answer within
 * FWD_CONNECT_MS marks the rule's target DOWN (fwd_core.h).
 *
 * Everything here runs in lwIP callbacks or from fwd_tick() in the main loop;
 * nothing blocks.
 */
#include "net/fwd.h"
#include "net/ping.h"

#if CONFIG_NET_FORWARD

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/ip.h"
#include "lwip/etharp.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/udp.h"
#include "lwip/priv/tcp_priv.h"   /* tcp_process_refused_data */

#include "net/fwd_core.h"
#include "api/api.h"
#include "api/ops.h"

#define FWD_CONNECT_MS   3000
#define FWD_ARP_CHECK_MS 2000

typedef struct {
    int           id;               /* 0 = free */
    int           owner;
    bool          udp;
    uint16_t      listen;
    char          iface[4];
    ip4_addr_t    to;
    uint16_t      port;
    bool          fail_fast;
    fwd_target_t  tgt;
    void         *lpcb;             /* tcp listen pcb or udp pcb */
    struct tcp_pcb *probe;
    uint32_t      probe_ms, arp_check_at;
    uint32_t      active, total, refused, errors, bytes_in, bytes_out;
} rule_t;

struct conn;
typedef struct { struct conn *c; uint8_t i; } side_t;   /* 0 = client, 1 = target */

typedef struct conn {
    bool            used;
    uint8_t         rule;
    struct tcp_pcb *pcb[2];
    side_t          side[2];
    struct pbuf    *pend[2];        /* received FROM side i, not yet written to the other */
    bool            eof[2];         /* FIN received from side i */
    bool            shut[2];        /* FIN passed on TO side i */
    bool            connected;      /* target side */
    uint32_t        start_ms;
} conn_t;

static rule_t rules[FWD_RULES];
static conn_t conns[FWD_TCP_CONNS];
static fwd_sess_t sess[FWD_UDP_SESSIONS];
static struct raw_pcb *icmp_pcb;
static int next_id = 1;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static struct netif *netif_by_name(const char *n) {
    struct netif *x;
    NETIF_FOREACH(x) {
        if (x->name[0] == n[0] && x->name[1] == n[1]) { return x; }
    }
    return NULL;
}

/* --- TCP ------------------------------------------------------------------ */

static void detach(struct tcp_pcb *p) {
    if (!p) { return; }
    tcp_arg(p, NULL);
    tcp_recv(p, NULL);
    tcp_sent(p, NULL);
    tcp_err(p, NULL);
}

static void conn_free(conn_t *c) {
    for (int i = 0; i < 2; i++) {
        if (c->pend[i]) { pbuf_free(c->pend[i]); }
    }
    if (rules[c->rule].active) { rules[c->rule].active--; }
    memset(c, 0, sizeof *c);
}

/* Abort both sides (RST). `except` is a pcb lwIP is already freeing. */
static void conn_abort(conn_t *c, struct tcp_pcb *except) {
    for (int i = 0; i < 2; i++) {
        struct tcp_pcb *p = c->pcb[i];
        c->pcb[i] = NULL;
        if (p && p != except) { detach(p); tcp_abort(p); }
    }
    conn_free(c);
}

static void conn_close(conn_t *c) {
    for (int i = 0; i < 2; i++) {
        struct tcp_pcb *p = c->pcb[i];
        c->pcb[i] = NULL;
        if (!p) { continue; }
        detach(p);
        if (tcp_close(p) != ERR_OK) { tcp_abort(p); }
    }
    conn_free(c);
}

/* Move what side `from` sent to the other side, as far as it will take it. */
static void pump(conn_t *c, int from) {
    int to = 1 - from;
    struct tcp_pcb *src = c->pcb[from], *dst = c->pcb[to];
    if (!dst || (to == 1 && !c->connected)) { return; }
    rule_t *r = &rules[c->rule];
    bool wrote = false;
    while (c->pend[from]) {
        struct pbuf *q = c->pend[from];
        u16_t n = q->len;
        if (n > tcp_sndbuf(dst)) { n = tcp_sndbuf(dst); }
        if (!n || !fwd_can_pass(n, tcp_sndbuf(dst), tcp_sndqueuelen(dst), TCP_SND_QUEUELEN)) { break; }
        if (tcp_write(dst, q->payload, n, TCP_WRITE_FLAG_COPY | (q->next || n < q->len ? TCP_WRITE_FLAG_MORE : 0)) != ERR_OK) {
            break;
        }
        wrote = true;
        if (src) { tcp_recved(src, n); }
        if (from == 0) { r->bytes_in += n; } else { r->bytes_out += n; }
        c->pend[from] = pbuf_free_header(q, n);
    }
    if (wrote) { tcp_output(dst); }
    /* The sender closed and everything it sent has been passed on: close
     * our side towards the other one (half-close). */
    if (c->eof[from] && !c->pend[from] && !c->shut[to]) {
        c->shut[to] = true;
        tcp_shutdown(dst, 0, 1);
    }
}

static bool conn_done(conn_t *c) {
    return c->eof[0] && c->eof[1] && !c->pend[0] && !c->pend[1];
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    side_t *s = (side_t *)arg;
    if (!s) { if (p) { pbuf_free(p); } return ERR_OK; }
    conn_t *c = s->c;
    int i = s->i;
    if (err != ERR_OK && p) { pbuf_free(p); return ERR_OK; }
    if (!p) {
        c->eof[i] = true;
        if (i == 0 && !c->connected) {         /* gave up before we got through */
            conn_abort(c, NULL);
            return ERR_ABRT;
        }
        pump(c, i);
        if (conn_done(c)) { conn_close(c); }
        return ERR_OK;
    }
    if (c->pend[i] || (i == 0 && !c->connected)) {
        return ERR_MEM;      /* lwIP keeps it as refused_data; offered again later */
    }
    c->pend[i] = p;
    pump(c, i);
    (void)tpcb;
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    side_t *s = (side_t *)arg;
    (void)tpcb; (void)len;
    if (!s) { return ERR_OK; }
    conn_t *c = s->c;
    pump(c, 1 - s->i);       /* room on side i: move what the other side sent */
    if (conn_done(c)) { conn_close(c); return ERR_OK; }
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    side_t *s = (side_t *)arg;
    if (!s) { return; }
    conn_t *c = s->c;
    rule_t *r = &rules[c->rule];
    if (s->i == 1 && !c->connected) {
        fwd_target_fail(&r->tgt, now_ms(), err == ERR_RST ? "refused" : "connect failed");
    } else {
        r->errors++;
    }
    struct tcp_pcb *gone = c->pcb[s->i];
    c->pcb[s->i] = NULL;           /* lwIP has freed it */
    conn_abort(c, gone);
}

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    side_t *s = (side_t *)arg;
    (void)tpcb;
    if (!s || err != ERR_OK) { return ERR_OK; }
    conn_t *c = s->c;
    c->connected = true;
    fwd_target_ok(&rules[c->rule].tgt);
    pump(c, 0);              /* anything the client sent while we waited */
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    rule_t *r = (rule_t *)arg;
    if (err != ERR_OK || !newpcb || !r) { return ERR_VAL; }
    uint32_t now = now_ms();
    if (!fwd_target_admit(&r->tgt, r->fail_fast, now)) {
        r->refused++;
        tcp_abort(newpcb);       /* RST: the client learns now, not after a timeout */
        return ERR_ABRT;
    }
    conn_t *c = NULL;
    for (int i = 0; i < FWD_TCP_CONNS; i++) {
        if (!conns[i].used) { c = &conns[i]; break; }
    }
    struct tcp_pcb *t = c ? tcp_new() : NULL;
    if (!t) {
        r->errors++;
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    memset(c, 0, sizeof *c);
    c->used = true;
    c->rule = (uint8_t)(r - rules);
    c->pcb[0] = newpcb;
    c->pcb[1] = t;
    c->start_ms = now;
    for (int i = 0; i < 2; i++) {
        c->side[i].c = c;
        c->side[i].i = (uint8_t)i;
        tcp_arg(c->pcb[i], &c->side[i]);
        tcp_recv(c->pcb[i], on_recv);
        tcp_sent(c->pcb[i], on_sent);
        tcp_err(c->pcb[i], on_err);
    }
    r->active++;
    r->total++;
    if (tcp_connect(t, &r->to, r->port, on_connected) != ERR_OK) {
        fwd_target_fail(&r->tgt, now, "no route");
        conn_abort(c, NULL);
        return ERR_ABRT;
    }
    return ERR_OK;
}

/* Active re-test of a DOWN TCP target. */
static err_t probe_connected(void *arg, struct tcp_pcb *p, err_t err) {
    rule_t *r = (rule_t *)arg;
    (void)err;
    if (r) { fwd_target_ok(&r->tgt); r->probe = NULL; }
    detach(p);
    if (tcp_close(p) != ERR_OK) { tcp_abort(p); return ERR_ABRT; }
    return ERR_OK;
}

static void probe_err(void *arg, err_t err) {
    rule_t *r = (rule_t *)arg;
    if (!r) { return; }
    r->probe = NULL;
    fwd_target_fail(&r->tgt, now_ms(), err == ERR_RST ? "refused" : "connect failed");
}

static void start_probe(rule_t *r, uint32_t now) {
    struct tcp_pcb *p = tcp_new();
    if (!p) { return; }
    tcp_arg(p, r);
    tcp_err(p, probe_err);
    r->probe = p;
    r->probe_ms = now;
    if (tcp_connect(p, &r->to, r->port, probe_connected) != ERR_OK) {
        detach(p);
        tcp_abort(p);
        r->probe = NULL;
        fwd_target_fail(&r->tgt, now, "no route");
    }
}

/* --- UDP ------------------------------------------------------------------ */

static void sess_drop(fwd_sess_t *s) {
    if (s->pcb) { udp_remove((struct udp_pcb *)s->pcb); }
}

static void from_target(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    fwd_sess_t *s = (fwd_sess_t *)arg;
    (void)pcb; (void)addr; (void)port;
    if (!s || !s->used) { pbuf_free(p); return; }
    rule_t *r = &rules[s->rule];
    ip_addr_t client;
    ip_addr_set_ip4_u32(&client, s->client_ip);
    r->bytes_out += p->tot_len;
    s->last_ms = now_ms();
    fwd_target_ok(&r->tgt);           /* an answer proves it is there */
    udp_sendto((struct udp_pcb *)r->lpcb, p, &client, s->client_port);
    pbuf_free(p);
}

/* A target on a directly attached subnet (not us, not loopback): an ARP
 * failure there is a real signal. */
static struct netif *local_subnet_of(const ip4_addr_t *ip) {
    struct netif *n;
    NETIF_FOREACH(n) {
        if (!netif_is_up(n) || ip4_addr_isany_val(*netif_ip4_addr(n)) || !(n->flags & NETIF_FLAG_ETHARP)) { continue; }
        if (ip4_addr_eq(ip, netif_ip4_addr(n))) { return NULL; }
        if (ip4_addr_net_eq(ip, netif_ip4_addr(n), netif_ip4_netmask(n))) { return n; }
    }
    return NULL;
}

static void from_client(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    rule_t *r = (rule_t *)arg;
    (void)pcb;
    if (!r || !r->id) { pbuf_free(p); return; }
    uint32_t now = now_ms();
    uint8_t ri = (uint8_t)(r - rules);
    fwd_sess_t *s = fwd_sess_find(sess, FWD_UDP_SESSIONS, ri, ip4_addr_get_u32(ip_2_ip4(addr)), port);
    if (!s) {
        if (!fwd_target_admit(&r->tgt, r->fail_fast, now)) {
            /* Port unreachable back to the sender, from the original
             * datagram (its IP and UDP headers are still in front). */
            r->refused++;
            if (pbuf_add_header_force(p, (size_t)ip_current_header_tot_len() + UDP_HLEN) == 0) {
                icmp_dest_unreach(p, ICMP_DUR_PORT);
            }
            pbuf_free(p);
            return;
        }
        s = fwd_sess_alloc(sess, FWD_UDP_SESSIONS);
        struct udp_pcb *out = s ? udp_new() : NULL;
        if (!out) {
            if (s) { memset(s, 0, sizeof *s); }
            r->errors++;
            pbuf_free(p);
            return;
        }
        s->rule = ri;
        s->client_ip = ip4_addr_get_u32(ip_2_ip4(addr));
        s->client_port = port;
        s->pcb = out;
        udp_bind(out, IP4_ADDR_ANY, 0);
        ip_addr_t to;
        ip_addr_copy_from_ip4(to, r->to);
        udp_connect(out, &to, r->port);
        udp_recv(out, from_target, s);
        r->total++;
        if (r->tgt.state != FWD_UP && local_subnet_of(&r->to) && !r->arp_check_at) {
            r->arp_check_at = now + FWD_ARP_CHECK_MS;
        }
    }
    s->last_ms = now;
    r->bytes_in += p->tot_len;
    udp_send((struct udp_pcb *)s->pcb, p);
    pbuf_free(p);
}

/* ICMP unreachable for one of our sessions: the target is down. The session
 * is dropped, so the client's next datagram meets fail-fast. */
static u8_t icmp_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    (void)arg; (void)pcb; (void)addr;
    struct ip_hdr ih;
    if (pbuf_copy_partial(p, &ih, sizeof ih, 0) != sizeof ih) { return 0; }
    u16_t hl = (u16_t)IPH_HL_BYTES(&ih);
    struct icmp_echo_hdr e;      /* type/code are the first two bytes of any ICMP */
    if (pbuf_copy_partial(p, &e, 4, hl) != 4 || ICMPH_TYPE(&e) != ICMP_DUR) { return 0; }
    struct ip_hdr in;
    if (pbuf_copy_partial(p, &in, sizeof in, (u16_t)(hl + 8)) != sizeof in || IPH_PROTO(&in) != IP_PROTO_UDP) { return 0; }
    u16_t ihl = (u16_t)IPH_HL_BYTES(&in);
    struct udp_hdr uh;
    if (pbuf_copy_partial(p, &uh, sizeof uh, (u16_t)(hl + 8 + ihl)) != sizeof uh) { return 0; }
    for (int i = 0; i < FWD_UDP_SESSIONS; i++) {
        fwd_sess_t *s = &sess[i];
        if (!s->used) { continue; }
        struct udp_pcb *u = (struct udp_pcb *)s->pcb;
        rule_t *r = &rules[s->rule];
        if (u->local_port == lwip_ntohs(uh.src) && lwip_ntohs(uh.dest) == r->port &&
            ip4_addr_eq(&in.dest, &r->to)) {
            fwd_target_fail(&r->tgt, now_ms(), "icmp unreachable");
            sess_drop(s);
            memset(s, 0, sizeof *s);
            break;
        }
    }
    return 0;                    /* lwIP may still want it */
}

/* --- rules ------------------------------------------------------------------ */

static void rule_close(rule_t *r) {
    uint8_t ri = (uint8_t)(r - rules);
    for (int i = 0; i < FWD_TCP_CONNS; i++) {
        if (conns[i].used && conns[i].rule == ri) { conn_abort(&conns[i], NULL); }
    }
    for (int i = 0; i < FWD_UDP_SESSIONS; i++) {
        if (sess[i].used && sess[i].rule == ri) { sess_drop(&sess[i]); memset(&sess[i], 0, sizeof sess[i]); }
    }
    if (r->probe) { detach(r->probe); tcp_abort(r->probe); r->probe = NULL; }
    if (r->lpcb) {
        if (r->udp) { udp_remove((struct udp_pcb *)r->lpcb); }
        else { struct tcp_pcb *l = (struct tcp_pcb *)r->lpcb; tcp_arg(l, NULL); tcp_close(l); }
    }
    memset(r, 0, sizeof *r);
}

int fwd_add(int owner, bool udp, uint16_t listen, const char *iface, const char *to,
            uint16_t port, bool fail_fast, const char **why) {
    rule_t *r = NULL;
    for (int i = 0; i < FWD_RULES; i++) {
        if (!rules[i].id) { r = &rules[i]; break; }
    }
    if (!r) { *why = "no free rule (max 8)"; return -1; }
    if (!listen || !port) { *why = "listen and port must be 1..65535"; return -1; }
    ip4_addr_t dst;
    if (!to || !ip4addr_aton(to, &dst)) { *why = "to must be an IPv4 address"; return -1; }
    struct netif *bind_if = NULL;
    if (iface && iface[0]) {
        bind_if = netif_by_name(iface);
        if (!bind_if) { *why = "no such interface"; return -1; }
    }
    for (int i = 0; i < FWD_RULES; i++) {
        if (rules[i].id && rules[i].udp == udp && rules[i].listen == listen) {
            *why = "that port already has a rule"; return -1;
        }
    }
    memset(r, 0, sizeof *r);
    r->owner = owner;
    r->udp = udp;
    r->listen = listen;
    snprintf(r->iface, sizeof r->iface, "%s", iface ? iface : "");
    r->to = dst;
    r->port = port;
    r->fail_fast = fail_fast;
    if (udp) {
        struct udp_pcb *u = udp_new();
        if (!u) { *why = "out of UDP pcbs"; return -1; }
        if (bind_if) { udp_bind_netif(u, bind_if); }
        if (udp_bind(u, IP4_ADDR_ANY, listen) != ERR_OK) {
            udp_remove(u);
            *why = "that UDP port is in use";
            return -1;
        }
        udp_recv(u, from_client, r);
        r->lpcb = u;
    } else {
        struct tcp_pcb *t = tcp_new();
        if (!t) { *why = "out of TCP pcbs"; return -1; }
        if (bind_if) { tcp_bind_netif(t, bind_if); }
        if (tcp_bind(t, IP4_ADDR_ANY, listen) != ERR_OK) {
            tcp_close(t);
            *why = "that TCP port is in use";
            return -1;
        }
        struct tcp_pcb *l = tcp_listen(t);
        if (!l) { tcp_close(t); *why = "out of listen pcbs"; return -1; }
        tcp_arg(l, r);
        tcp_accept(l, on_accept);
        r->lpcb = l;
    }
    r->id = next_id++;
    if (next_id <= 0) { next_id = 1; }
    printf("fwd: #%d %s :%u%s%s -> %s:%u%s\n", r->id, udp ? "udp" : "tcp", listen,
           bind_if ? " on " : "", bind_if ? r->iface : "", to, port, fail_fast ? " (fail fast)" : "");
    return r->id;
}

bool fwd_remove(int id) {
    for (int i = 0; i < FWD_RULES; i++) {
        if (rules[i].id == id && id > 0) { rule_close(&rules[i]); return true; }
    }
    return false;
}

int fwd_release_owner(int owner) {
    if (owner < 0) { return 0; }
    int n = 0;
    for (int i = 0; i < FWD_RULES; i++) {
        if (rules[i].id && rules[i].owner == owner) { rule_close(&rules[i]); n++; }
    }
    return n;
}

int fwd_release_all_owned(void) {
    int n = 0;
    for (int i = 0; i < FWD_RULES; i++) {
        if (rules[i].id && rules[i].owner >= 0) { rule_close(&rules[i]); n++; }
    }
    return n;
}

int fwd_count(void) {
    int n = 0;
    for (int i = 0; i < FWD_RULES; i++) { n += rules[i].id != 0; }
    return n;
}

bool fwd_info(int k, fwd_info_t *o) {
    for (int i = 0; i < FWD_RULES; i++) {
        if (!rules[i].id) { continue; }
        if (k-- > 0) { continue; }
        const rule_t *r = &rules[i];
        memset(o, 0, sizeof *o);
        o->id = r->id;
        o->udp = r->udp;
        o->listen = r->listen;
        snprintf(o->iface, sizeof o->iface, "%s", r->iface);
        ip4addr_ntoa_r(&r->to, o->to, sizeof o->to);
        o->port = r->port;
        o->fail_fast = r->fail_fast;
        o->state = r->tgt.state == FWD_UP ? "up" : r->tgt.state == FWD_DOWN ? "down" : "unknown";
        snprintf(o->why, sizeof o->why, "%s", r->tgt.why);
        o->active = r->active; o->total = r->total; o->refused = r->refused; o->errors = r->errors;
        o->bytes_in = r->bytes_in; o->bytes_out = r->bytes_out;
        o->owner = r->owner;
        return true;
    }
    return false;
}

size_t fwd_status_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "{\"rules\":[");
    fwd_info_t f;
    for (int i = 0; fwd_info(i, &f) && n + 260 < cap; i++) {
        n += (size_t)snprintf(dst + n, cap - n,
            "%s{\"id\":%d,\"proto\":\"%s\",\"listen\":%u,\"iface\":\"%s\",\"to\":\"%s\",\"port\":%u,"
            "\"fail_fast\":%s,\"target\":\"%s\",\"why\":\"%s\",\"active\":%lu,\"total\":%lu,"
            "\"refused\":%lu,\"errors\":%lu,\"bytes_in\":%lu,\"bytes_out\":%lu,\"owner\":%d}",
            i ? "," : "", f.id, f.udp ? "udp" : "tcp", f.listen, f.iface, f.to, f.port,
            f.fail_fast ? "true" : "false", f.state, f.why,
            (unsigned long)f.active, (unsigned long)f.total, (unsigned long)f.refused,
            (unsigned long)f.errors, (unsigned long)f.bytes_in, (unsigned long)f.bytes_out, f.owner);
    }
    int ns = 0;
    for (int i = 0; i < FWD_UDP_SESSIONS; i++) { ns += sess[i].used; }
    return n + (size_t)snprintf(dst + n, cap - n, "],\"udp_sessions\":%d}", ns);
}

/* --- loop ------------------------------------------------------------------- */

void fwd_init(void) {
    icmp_pcb = raw_new(IP_PROTO_ICMP);
    if (icmp_pcb) {
        raw_recv(icmp_pcb, icmp_recv, NULL);
        raw_bind(icmp_pcb, IP4_ADDR_ANY);
    }
}

void fwd_tick(void) {
    /* Loopback (127.0.0.1 and our own addresses) is delivered here: in a
     * NO_SYS build nothing else polls it. */
    netif_poll_all();
    if (!fwd_count()) { return; }
    uint32_t now = now_ms();

    for (int i = 0; i < FWD_TCP_CONNS; i++) {
        conn_t *c = &conns[i];
        if (!c->used) { continue; }
        if (!c->connected && (uint32_t)(now - c->start_ms) >= FWD_CONNECT_MS) {
            fwd_target_fail(&rules[c->rule].tgt, now, "timeout");
            conn_abort(c, NULL);       /* RST to the client */
            continue;
        }
        /* Data lwIP is holding for us (refused while a chain was pending):
         * offer it again now rather than at the next 250 ms timer. */
        for (int k = 0; k < 2; k++) {
            if (c->used && c->pcb[k] && c->pcb[k]->refused_data && !c->pend[k] &&
                (k == 1 || c->connected)) {
                tcp_process_refused_data(c->pcb[k]);
            }
        }
    }

    fwd_sess_expire(sess, FWD_UDP_SESSIONS, now, sess_drop);

    for (int i = 0; i < FWD_RULES; i++) {
        rule_t *r = &rules[i];
        if (!r->id) { continue; }
        if (!r->udp) {
            if (r->probe && (uint32_t)(now - r->probe_ms) >= FWD_CONNECT_MS) {
                detach(r->probe);
                tcp_abort(r->probe);
                r->probe = NULL;
                fwd_target_fail(&r->tgt, now, "timeout");
            } else if (!r->probe && r->tgt.state == FWD_DOWN && (int32_t)(now - r->tgt.next_try) >= 0) {
                r->tgt.next_try = now + FWD_RETEST_MS;
                start_probe(r, now);
            }
        } else if (r->arp_check_at && (int32_t)(now - r->arp_check_at) >= 0) {
            r->arp_check_at = 0;
            struct netif *n = local_subnet_of(&r->to);
            struct eth_addr *ea;
            const ip4_addr_t *ia;
            if (n && etharp_find_addr(n, &r->to, &ea, &ia) < 0) {
                fwd_target_fail(&r->tgt, now, "no ARP reply");
            }
        }
    }
}

/* --- ops: net.forwards (read-only) ----------------------------------------- */

static void op_forwards(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    fwd_status_json(api_scratch(), api_scratch_size());
    api_reply_ok(reply, ctx, id, api_scratch());
}

static const op_def_t net_ops[] = {
    { "net.forwards", op_forwards },
    { "net.ping", ping_op },
    { NULL, NULL }
};

static const char *const net_topics[] = { "net.ping", NULL };

const op_module_t op_module_net = {
    .ns = "net", .ops = net_ops, .topics = net_topics, .init = NULL
};

#endif /* CONFIG_NET_FORWARD */
