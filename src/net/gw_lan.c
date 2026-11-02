/* Gateway LAN: cfg, hostname, heard peers, advertisements, and gw.forward.
 * The link-local netif itself lives in eth_w5500.c. See gw_lan.h. */
#include "net/gw_lan.h"

#if CONFIG_NET_GW_LAN

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"

#include "api/api.h"
#include "config/config.h"
#include "net/eth_w5500.h"
#include "net/gw_core.h"
#include "net/mdns.h"
#include "net/uplink.h"

#if CONFIG_NET_MDNS_GW
#include "lwip/apps/mdns.h"
#include "lwip/prot/dns.h"
#endif

#define GW_CFG_PATH  "cfg/gw.json"
#define GW_NAME_PATH "cfg/hostname.json"
#define GW_RULES     4
#define GW_SPLICES   4
#define GW_UDP_SESS  8
#define GW_UDP_IDLE  60000u
#define GW_ADS       4

static char lan_iface[GW_IFACE_LEN] = "none";
static char active_name[64];
static bool name_from_file;
static gw_table_t peers;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void lower_copy(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (!dst || cap == 0) { return; }
    if (!src) { dst[0] = 0; return; }
    for (; src[n] && n + 1 < cap; n++) {
        char c = src[n];
        if (c >= 'A' && c <= 'Z') { c = (char)(c - 'A' + 'a'); }
        dst[n] = c;
    }
    dst[n] = 0;
}

bool gw_lan_hold_ll(void) { return strcmp(lan_iface, "e0") == 0; }
const char *gw_lan_iface(void) { return lan_iface; }
bool gw_is_lan_iface(const char *name) {
    return name && strcmp(lan_iface, "none") != 0 && strcmp(name, lan_iface) == 0;
}
const char *gw_active_name(void) { return active_name[0] ? active_name : ""; }
const char *gw_name_source(void) { return name_from_file ? "file" : "serial"; }

bool gw_best_peer(char *name, size_t name_cap, char *iface, size_t iface_cap,
                  int32_t *metric, char *ll, size_t ll_cap) {
    return gw_peer_best(&peers, now_ms(), name, name_cap, iface, iface_cap,
                        metric, ll, ll_cap);
}

/* --- staged work (ops run inside an lwIP callback) ------------------------ */

enum { ST_HOST = 1, ST_FWD = 2, ST_UNFWD = 3 };

typedef struct {
    uint8_t  kind;
    bool     tcp;
    uint16_t listen, port;
    char     name[GW_NAME_LEN];
    char     ll[GW_LL_LEN];
} stage_t;

static stage_t queue[4];
static int queue_n;

static bool enqueue(const stage_t *s, const char **why) {
    if (queue_n >= (int)(sizeof queue / sizeof queue[0])) {
        if (why) { *why = "busy"; }
        return false;
    }
    queue[queue_n++] = *s;
    return true;
}

void gw_hostname_written(const char *name) {
    const char *why = NULL;
    gw_hostname_stage(name, &why);
}

bool gw_hostname_stage(const char *name, const char **why) {
    if (!gw_hostname_ok(name)) {
        if (why) { *why = "name must be a DNS label"; }
        return false;
    }
    stage_t s;
    memset(&s, 0, sizeof s);
    s.kind = ST_HOST;
    snprintf(s.name, sizeof s.name, "%s", name);
    return enqueue(&s, why);
}

bool gw_forward_stage(const char *proto, long listen_port, const char *to,
                      long dest_port, const char **why) {
    bool tcp;
    if (proto && strcmp(proto, "tcp") == 0) { tcp = true; }
    else if (proto && strcmp(proto, "udp") == 0) { tcp = false; }
    else { if (why) { *why = "proto is tcp or udp"; } return false; }
    if (listen_port < 1 || listen_port > 65535 || dest_port < 1 || dest_port > 65535) {
        if (why) { *why = "listen and port must be 1..65535"; }
        return false;
    }
    char name[GW_NAME_LEN];
    lower_copy(name, sizeof name, to);
    char ll[GW_LL_LEN];
    if (!gw_peer_ll(&peers, name, now_ms(), ll, sizeof ll)) {
        if (why) { *why = "to is not a name gw.peers currently returns"; }
        return false;
    }
    stage_t s;
    memset(&s, 0, sizeof s);
    s.kind = ST_FWD;
    s.tcp = tcp;
    s.listen = (uint16_t)listen_port;
    s.port = (uint16_t)dest_port;
    snprintf(s.name, sizeof s.name, "%s", name);
    snprintf(s.ll, sizeof s.ll, "%s", ll);
    return enqueue(&s, why);
}

bool gw_unforward_stage(const char *proto, long listen_port, const char **why) {
    bool tcp;
    if (proto && strcmp(proto, "tcp") == 0) { tcp = true; }
    else if (proto && strcmp(proto, "udp") == 0) { tcp = false; }
    else { if (why) { *why = "proto is tcp or udp"; } return false; }
    if (listen_port < 1 || listen_port > 65535) {
        if (why) { *why = "listen must be 1..65535"; }
        return false;
    }
    stage_t s;
    memset(&s, 0, sizeof s);
    s.kind = ST_UNFWD;
    s.tcp = tcp;
    s.listen = (uint16_t)listen_port;
    return enqueue(&s, why);
}

/* --- gw.forward: a small splice, distinct from NET_FORWARD ---------------- */

typedef struct gw_rule gw_rule_t;

typedef struct {
    bool used;
    uint8_t rule;
    bool connected;
    bool eof[2];
    bool shut[2];
    struct tcp_pcb *pcb[2];
    struct pbuf *pend[2];
} gw_splice_t;

struct gw_rule {
    bool used;
    bool tcp;
    uint16_t listen, port;
    char name[GW_NAME_LEN];
    ip_addr_t to;
    void *lpcb;
};

typedef struct {
    gw_splice_t *sp;
    uint8_t i;
} gw_side_t;

static gw_rule_t rules[GW_RULES];
static gw_splice_t splices[GW_SPLICES];
static gw_side_t sides[GW_SPLICES][2];

typedef struct {
    bool used;
    uint8_t rule;
    uint32_t client_ip;
    uint16_t client_port;
    uint32_t last_ms;
    struct udp_pcb *pcb;
} gw_udp_t;

static gw_udp_t udp_sess[GW_UDP_SESS];

static void side_detach(struct tcp_pcb *p) {
    if (!p) { return; }
    tcp_arg(p, NULL);
    tcp_recv(p, NULL);
    tcp_sent(p, NULL);
    tcp_err(p, NULL);
    tcp_accept(p, NULL);
}

static void splice_free(gw_splice_t *c) {
    for (int i = 0; i < 2; i++) {
        if (c->pend[i]) { pbuf_free(c->pend[i]); }
    }
    memset(c, 0, sizeof *c);
}

static void splice_abort(gw_splice_t *c, struct tcp_pcb *except) {
    for (int i = 0; i < 2; i++) {
        struct tcp_pcb *p = c->pcb[i];
        c->pcb[i] = NULL;
        if (p && p != except) { side_detach(p); tcp_abort(p); }
    }
    splice_free(c);
}

static void splice_pump(gw_splice_t *c, int from) {
    int to = 1 - from;
    struct tcp_pcb *src = c->pcb[from];
    struct tcp_pcb *dst = c->pcb[to];
    if (!dst || (to == 1 && !c->connected)) { return; }
    bool wrote = false;
    while (c->pend[from]) {
        struct pbuf *q = c->pend[from];
        u16_t n = q->len;
        if (n > tcp_sndbuf(dst)) { n = tcp_sndbuf(dst); }
        if (!n) { break; }
        u8_t flags = TCP_WRITE_FLAG_COPY;
        if (q->next || n < q->len) { flags = (u8_t)(flags | TCP_WRITE_FLAG_MORE); }
        if (tcp_write(dst, q->payload, n, flags) != ERR_OK) { break; }
        wrote = true;
        if (src) { tcp_recved(src, n); }
        c->pend[from] = pbuf_free_header(q, n);
    }
    if (wrote) { tcp_output(dst); }
    if (c->eof[from] && !c->pend[from] && dst && !c->shut[to]) {
        c->shut[to] = true;
        tcp_shutdown(dst, 0, 1);
    }
}

static bool splice_done(const gw_splice_t *c) {
    return c->eof[0] && c->eof[1] && !c->pend[0] && !c->pend[1];
}

static void splice_close(gw_splice_t *c) {
    for (int i = 0; i < 2; i++) {
        struct tcp_pcb *p = c->pcb[i];
        c->pcb[i] = NULL;
        if (!p) { continue; }
        side_detach(p);
        if (tcp_close(p) != ERR_OK) { tcp_abort(p); }
    }
    splice_free(c);
}

static err_t gw_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    gw_side_t *s = arg;
    (void)tpcb;
    if (!s || !s->sp || !s->sp->used) { if (p) { pbuf_free(p); } return ERR_OK; }
    gw_splice_t *c = s->sp;
    int i = s->i;
    if (err != ERR_OK && p) { pbuf_free(p); return ERR_OK; }
    if (!p) {
        c->eof[i] = true;
        if (i == 0 && !c->connected) { splice_abort(c, NULL); return ERR_ABRT; }
        splice_pump(c, i);
        if (splice_done(c)) { splice_close(c); }
        return ERR_OK;
    }
    if (c->pend[i] || (i == 0 && !c->connected)) { return ERR_MEM; }
    c->pend[i] = p;
    splice_pump(c, i);
    return ERR_OK;
}

static err_t gw_tcp_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    gw_side_t *s = arg;
    (void)tpcb; (void)len;
    if (!s || !s->sp || !s->sp->used) { return ERR_OK; }
    splice_pump(s->sp, 1 - s->i);
    if (splice_done(s->sp)) { splice_close(s->sp); }
    return ERR_OK;
}

static void gw_tcp_err(void *arg, err_t err) {
    gw_side_t *s = arg;
    (void)err;
    if (!s || !s->sp || !s->sp->used) { return; }
    gw_splice_t *c = s->sp;
    struct tcp_pcb *gone = c->pcb[s->i];
    c->pcb[s->i] = NULL;
    splice_abort(c, gone);
}

static err_t gw_tcp_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    gw_side_t *s = arg;
    (void)tpcb;
    if (!s || !s->sp || err != ERR_OK) { return ERR_OK; }
    s->sp->connected = true;
    splice_pump(s->sp, 0);
    return ERR_OK;
}

static err_t gw_tcp_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    gw_rule_t *r = arg;
    if (err != ERR_OK || !newpcb || !r || !r->used) { return ERR_VAL; }
    gw_splice_t *c = NULL;
    int idx = -1;
    for (int i = 0; i < GW_SPLICES; i++) {
        if (!splices[i].used) { c = &splices[i]; idx = i; break; }
    }
    struct tcp_pcb *out = c ? tcp_new() : NULL;
    if (!out) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    memset(c, 0, sizeof *c);
    c->used = true;
    c->rule = (uint8_t)(r - rules);
    c->pcb[0] = newpcb;
    c->pcb[1] = out;
    for (int i = 0; i < 2; i++) {
        sides[idx][i].sp = c;
        sides[idx][i].i = (uint8_t)i;
        tcp_arg(c->pcb[i], &sides[idx][i]);
        tcp_recv(c->pcb[i], gw_tcp_recv);
        tcp_sent(c->pcb[i], gw_tcp_sent);
        tcp_err(c->pcb[i], gw_tcp_err);
    }
    if (tcp_connect(out, &r->to, r->port, gw_tcp_connected) != ERR_OK) {
        splice_abort(c, NULL);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static void udp_sess_drop(gw_udp_t *s) {
    if (s->pcb) { udp_remove(s->pcb); }
    memset(s, 0, sizeof *s);
}

static void gw_udp_from_peer(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port) {
    gw_udp_t *s = arg;
    (void)pcb; (void)addr; (void)port;
    if (!s || !s->used || s->rule >= GW_RULES || !rules[s->rule].used) {
        if (p) { pbuf_free(p); }
        return;
    }
    ip_addr_t client;
    ip_addr_set_ip4_u32(&client, s->client_ip);
    s->last_ms = now_ms();
    udp_sendto((struct udp_pcb *)rules[s->rule].lpcb, p, &client, s->client_port);
    pbuf_free(p);
}

static void gw_udp_from_client(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                               const ip_addr_t *addr, u16_t port) {
    gw_rule_t *r = arg;
    (void)pcb;
    if (!r || !r->used || !p) { if (p) { pbuf_free(p); } return; }
    uint32_t cip = ip4_addr_get_u32(ip_2_ip4(addr));
    uint8_t ri = (uint8_t)(r - rules);
    gw_udp_t *s = NULL;
    for (int i = 0; i < GW_UDP_SESS; i++) {
        if (udp_sess[i].used && udp_sess[i].rule == ri &&
            udp_sess[i].client_ip == cip && udp_sess[i].client_port == port) {
            s = &udp_sess[i];
            break;
        }
    }
    if (!s) {
        for (int i = 0; i < GW_UDP_SESS; i++) {
            if (!udp_sess[i].used) { s = &udp_sess[i]; break; }
        }
        if (!s) {
            s = &udp_sess[0];
            for (int i = 1; i < GW_UDP_SESS; i++) {
                if ((int32_t)(udp_sess[i].last_ms - s->last_ms) < 0) { s = &udp_sess[i]; }
            }
            udp_sess_drop(s);
        }
        struct udp_pcb *out = udp_new();
        if (!out) { pbuf_free(p); return; }
        memset(s, 0, sizeof *s);
        s->used = true;
        s->rule = ri;
        s->client_ip = cip;
        s->client_port = port;
        s->pcb = out;
        udp_bind(out, IP4_ADDR_ANY, 0);
        udp_connect(out, &r->to, r->port);
        udp_recv(out, gw_udp_from_peer, s);
    }
    s->last_ms = now_ms();
    udp_send(s->pcb, p);
    pbuf_free(p);
}

static void rule_close(gw_rule_t *r) {
    uint8_t ri = (uint8_t)(r - rules);
    for (int i = 0; i < GW_SPLICES; i++) {
        if (splices[i].used && splices[i].rule == ri) { splice_abort(&splices[i], NULL); }
    }
    for (int i = 0; i < GW_UDP_SESS; i++) {
        if (udp_sess[i].used && udp_sess[i].rule == ri) { udp_sess_drop(&udp_sess[i]); }
    }
    if (r->lpcb) {
        if (r->tcp) {
            struct tcp_pcb *l = r->lpcb;
            side_detach(l);
            if (tcp_close(l) != ERR_OK) { tcp_abort(l); }
        } else {
            udp_remove(r->lpcb);
        }
    }
    memset(r, 0, sizeof *r);
}

static bool rule_open(bool tcp, uint16_t listen, uint16_t port, const char *name, const char *ll) {
    ip4_addr_t dst;
    if (!ip4addr_aton(ll, &dst)) { return false; }
    for (int i = 0; i < GW_RULES; i++) {
        if (rules[i].used && rules[i].tcp == tcp && rules[i].listen == listen) {
            printf("gw: %s :%u already forwarded\n", tcp ? "tcp" : "udp", listen);
            return false;
        }
    }
    gw_rule_t *r = NULL;
    for (int i = 0; i < GW_RULES; i++) {
        if (!rules[i].used) { r = &rules[i]; break; }
    }
    if (!r) { printf("gw: forward table full\n"); return false; }
    memset(r, 0, sizeof *r);
    r->tcp = tcp;
    r->listen = listen;
    r->port = port;
    snprintf(r->name, sizeof r->name, "%s", name);
    ip_addr_copy_from_ip4(r->to, dst);
    if (tcp) {
        struct tcp_pcb *t = tcp_new();
        if (!t) { return false; }
        if (tcp_bind(t, IP4_ADDR_ANY, listen) != ERR_OK) { tcp_close(t); return false; }
        struct tcp_pcb *l = tcp_listen(t);
        if (!l) { tcp_close(t); return false; }
        tcp_arg(l, r);
        tcp_accept(l, gw_tcp_accept);
        r->lpcb = l;
    } else {
        struct udp_pcb *u = udp_new();
        if (!u) { return false; }
        if (udp_bind(u, IP4_ADDR_ANY, listen) != ERR_OK) { udp_remove(u); return false; }
        udp_recv(u, gw_udp_from_client, r);
        r->lpcb = u;
    }
    r->used = true;
    printf("gw: %s :%u -> %s %s:%u\n", tcp ? "tcp" : "udp", listen, name, ll, port);
    return true;
}

static void apply_host(const char *name) {
    if (!mdns_set_hostname(name)) {
        printf("gw: hostname refused\n");
        return;
    }
    snprintf(active_name, sizeof active_name, "%s", mdns_hostname());
    char body[96];
    snprintf(body, sizeof body, "{\"name\":\"%s\"}", active_name);
    if (cfg_file_write(GW_NAME_PATH, body)) {
        name_from_file = true;
        printf("gw: hostname %s stored\n", active_name);
    }
}

static void apply_queue(void) {
    for (int i = 0; i < queue_n; i++) {
        stage_t *s = &queue[i];
        if (s->kind == ST_HOST) { apply_host(s->name); }
        else if (s->kind == ST_FWD) { rule_open(s->tcp, s->listen, s->port, s->name, s->ll); }
        else if (s->kind == ST_UNFWD) {
            for (int k = 0; k < GW_RULES; k++) {
                if (rules[k].used && rules[k].tcp == s->tcp && rules[k].listen == s->listen) {
                    printf("gw: unforward %s :%u\n", s->tcp ? "tcp" : "udp", s->listen);
                    rule_close(&rules[k]);
                }
            }
        }
    }
    queue_n = 0;
}

static void udp_age(uint32_t now) {
    for (int i = 0; i < GW_UDP_SESS; i++) {
        if (udp_sess[i].used && (uint32_t)(now - udp_sess[i].last_ms) >= GW_UDP_IDLE) {
            udp_sess_drop(&udp_sess[i]);
        }
    }
}

/* --- peers ---------------------------------------------------------------- */

size_t gw_peers_json(char *dst, size_t cap) {
    if (!dst || cap == 0) { return 0; }
    uint32_t now = now_ms();
    gw_peer_expire(&peers, now);
    size_t n = (size_t)snprintf(dst, cap, "{\"peers\":[");
    bool first_p = true;
    for (int i = 0; i < GW_PEERS && n + 48 < cap; i++) {
        if (!peers.peers[i].used) { continue; }
        char en[GW_NAME_LEN * 2];
        api_json_escape(en, sizeof en, peers.peers[i].name);
        size_t mark = n;
        n += (size_t)snprintf(dst + n, cap - n, "%s{\"name\":\"%s\",\"conns\":[",
                              first_p ? "" : ",", en);
        bool first_c = true;
        bool any = false;
        for (int c = 0; c < GW_CONNS && n + 80 < cap; c++) {
            gw_conn_t *cn = &peers.peers[i].conns[c];
            if (!cn->used) { continue; }
            uint32_t age = now - cn->seen_ms;
            if (age >= GW_DROP_MS) { continue; }
            char ei[8], el[GW_LL_LEN * 2];
            api_json_escape(ei, sizeof ei, cn->iface);
            api_json_escape(el, sizeof el, cn->ll);
            n += (size_t)snprintf(dst + n, cap - n,
                                  "%s{\"iface\":\"%s\",\"metric\":%ld,\"ll\":\"%s\",\"age_ms\":%lu}",
                                  first_c ? "" : ",", ei, (long)cn->metric, el,
                                  (unsigned long)age);
            first_c = false;
            any = true;
        }
        if (!any) { n = mark; continue; }
        n += (size_t)snprintf(dst + n, cap - n, "]}");
        first_p = false;
    }
    n += (size_t)snprintf(dst + n, cap > n ? cap - n : 0, "]}");
    return n;
}

/* --- advertisements and the search ---------------------------------------- */

#if CONFIG_NET_MDNS_GW

typedef struct {
    bool used;
    char iface[3];
    int slot;
} gw_ad_t;

static gw_ad_t ads[GW_ADS];
static char ads_host[64];
static u8_t search_id;
static bool search_on;
static uint32_t next_ad_ms;

static void search_stop(void) {
    if (!search_on) { return; }
    mdns_search_stop(search_id);
    search_on = false;
}

static bool nif_name(const struct netif *n, char out[3]) {
    if (!n) { out[0] = 0; return false; }
    out[0] = n->name[0];
    out[1] = n->name[1];
    out[2] = 0;
    return out[0] != 0;
}

static bool our_addr(const char *ip) {
    struct netif *n;
    NETIF_FOREACH(n) {
        if (!ip4_addr_isany_val(*netif_ip4_addr(n)) &&
            strcmp(ip, ip4addr_ntoa(netif_ip4_addr(n))) == 0) {
            return true;
        }
    }
    return false;
}

static bool ll_text(const char *ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail;
    if (!ip || sscanf(ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) { return false; }
    return a == 169 && b == 254;
}

static void label0(const u8_t *name, int len, char *dst, size_t cap) {
    dst[0] = 0;
    if (!name || len < 2 || cap == 0) { return; }
    unsigned n = name[0];
    if (n == 0 || n + 1 > cap || (int)n >= len) { return; }
    memcpy(dst, name + 1, n);
    dst[n] = 0;
}

static void note_instance(const char *instance, int32_t metric, const char *ll) {
    char raw_h[GW_NAME_LEN], raw_i[4], host[GW_NAME_LEN], iface[4];
    if (!gw_split_instance(instance, raw_h, sizeof raw_h, raw_i, sizeof raw_i)) { return; }
    lower_copy(host, sizeof host, raw_h);
    lower_copy(iface, sizeof iface, raw_i);
    if (active_name[0] && strcmp(host, active_name) == 0) { return; }
    if (ll && ll[0] && (our_addr(ll) || !ll_text(ll))) { return; }
    if (metric < 0 && (!ll || !ll[0])) {
        gw_peer_touch(&peers, host, iface, now_ms());
        return;
    }
    gw_peer_note(&peers, host, iface, metric, ll, now_ms());
}

static int txt_item(const char *p, int n, const char *key) {
    size_t k = strlen(key);
    if ((size_t)n <= k || memcmp(p, key, k) != 0) { return -1; }
    return (int)k;
}

static void on_answer(struct mdns_answer *ans, const char *varpart, int varlen, int flags, void *arg) {
    (void)flags; (void)arg;
    if (!ans || !varpart || varlen < 0) { return; }
    char owner[80];
    label0(ans->info.domain.name, ans->info.domain.length, owner, sizeof owner);
    if (ans->info.type == DNS_RRTYPE_PTR) {
        char inst[80];
        label0((const u8_t *)varpart, varlen, inst, sizeof inst);
        note_instance(inst, -1, NULL);
        return;
    }
    if (ans->info.type == DNS_RRTYPE_A && varlen >= 4) {
        char ip[16];
        snprintf(ip, sizeof ip, "%u.%u.%u.%u",
                 (unsigned)(uint8_t)varpart[0], (unsigned)(uint8_t)varpart[1],
                 (unsigned)(uint8_t)varpart[2], (unsigned)(uint8_t)varpart[3]);
        note_instance(owner, -1, ip);
        return;
    }
    if (ans->info.type == DNS_RRTYPE_TXT) {
        int metric = -1;
        int i = 0;
        while (i < varlen) {
            int n = (uint8_t)varpart[i++];
            if (n <= 0 || i + n > varlen) { break; }
            int k = txt_item(varpart + i, n, "metric=");
            if (k >= 0) {
                char tmp[16];
                int c = n - k;
                if (c > (int)sizeof tmp - 1) { c = (int)sizeof tmp - 1; }
                memcpy(tmp, varpart + i + k, (size_t)c);
                tmp[c] = 0;
                long v = 0;
                if (sscanf(tmp, "%ld", &v) == 1) { metric = (int)v; }
            }
            i += n;
        }
        if (metric >= 0) { note_instance(owner, metric, NULL); }
    }
}

static void gw_txt(struct mdns_service *service, void *txt_userdata) {
    int idx = (int)(uintptr_t)txt_userdata;
    if (idx < 0 || idx >= GW_ADS || !ads[idx].used) { return; }
#if CONFIG_NET_UPLINK
    int32_t metric = uplink_metric_of(ads[idx].iface);
#else
    int32_t metric = 2147483646; /* unknown, same as upl_metric before a sample */
#endif
    char a[16], b[24];
    gw_ad_txt(ads[idx].iface, metric, a, sizeof a, b, sizeof b);
    mdns_resp_add_service_txtitem(service, a, (u8_t)strlen(a));
    mdns_resp_add_service_txtitem(service, b, (u8_t)strlen(b));
}

static void ads_forget(void) {
    memset(ads, 0, sizeof ads);
    ads_host[0] = 0;
}

static void ads_clear(struct netif *ll) {
    for (int i = 0; i < GW_ADS; i++) {
        if (ads[i].used && ads[i].slot >= 0 && ll) {
            mdns_resp_del_service(ll, (u8_t)ads[i].slot);
        }
        ads[i].used = false;
    }
}

static bool has_gateway(struct netif *n) {
    return n && netif_is_up(n) && netif_is_link_up(n) &&
           !ip4_addr_isany_val(*netif_ip4_addr(n)) &&
           !ip4_addr_isany_val(*netif_ip4_gw(n));
}

static void services_sync(struct netif *ll) {
    if (strcmp(ads_host, mdns_hostname()) != 0) {
        ads_clear(ll);
        snprintf(ads_host, sizeof ads_host, "%s", mdns_hostname());
    }
    char want[GW_ADS][3];
    int nwant = 0;
    struct netif *nif;
    NETIF_FOREACH(nif) {
        char name[3];
        if (!nif_name(nif, name)) { continue; }
        if (strcmp(name, "el") == 0 || strcmp(name, "wg") == 0) { continue; }
        if (gw_is_lan_iface(name)) { continue; }
        if (!gw_advertise(has_gateway(nif), false)) { continue; }
        if (nwant >= GW_ADS) { break; }
        memcpy(want[nwant], name, 3);
        nwant++;
    }
    for (int i = 0; i < GW_ADS; i++) {
        if (!ads[i].used) { continue; }
        bool keep = false;
        for (int w = 0; w < nwant; w++) {
            if (strcmp(ads[i].iface, want[w]) == 0) { keep = true; }
        }
        if (!keep) {
            if (ads[i].slot >= 0) { mdns_resp_del_service(ll, (u8_t)ads[i].slot); }
            ads[i].used = false;
        }
    }
    for (int w = 0; w < nwant; w++) {
        bool have = false;
        for (int i = 0; i < GW_ADS; i++) {
            if (ads[i].used && strcmp(ads[i].iface, want[w]) == 0) { have = true; }
        }
        if (have) { continue; }
        int slot = -1;
        for (int i = 0; i < GW_ADS; i++) {
            if (!ads[i].used) { slot = i; break; }
        }
        if (slot < 0) { continue; }
        char inst[64];
        if (!gw_instance_name(mdns_hostname(), want[w], inst, sizeof inst)) { continue; }
        s8_t h = mdns_resp_add_service(ll, inst, "_btgw-gw", DNSSD_PROTO_UDP, 80,
                                       gw_txt, (void *)(uintptr_t)slot);
        if (h < 0) {
            printf("gw: advertise %s failed (%d)\n", inst, (int)h);
            continue;
        }
        ads[slot].used = true;
        ads[slot].slot = h;
        memcpy(ads[slot].iface, want[w], 3);
    }
}

static struct netif *carrier_up(void) {
    if (strcmp(lan_iface, "none") == 0) { return NULL; }
    struct netif *n;
    NETIF_FOREACH(n) {
        if (n->name[0] == lan_iface[0] && n->name[1] == lan_iface[1] &&
            netif_is_up(n) && netif_is_link_up(n)) {
            return n;
        }
    }
    return NULL;
}

static void advertise(uint32_t now) {
    struct netif *ll = eth_ll_netif();
    bool up = carrier_up() && ll && ip4_addr_islinklocal(netif_ip4_addr(ll));
    if (!up) {
        search_stop();
        if (ll) { ads_clear(ll); }
        else { ads_forget(); }
        return;
    }
    mdns_add_netif(ll);
    services_sync(ll);
    if (!search_on) {
        if (mdns_search_service(NULL, "_btgw-gw", DNSSD_PROTO_UDP, ll,
                                on_answer, NULL, &search_id) == ERR_OK) {
            search_on = true;
        }
    }
    if ((int32_t)(now - next_ad_ms) >= 0) {
        next_ad_ms = now + GW_AD_MS;
        mdns_resp_announce(ll);
    }
}

#else

static void advertise(uint32_t now) { (void)now; }

#endif /* CONFIG_NET_MDNS_GW */

void gw_lan_init(void) {
    static char buf[128];
    bool have = cfg_file_read(GW_CFG_PATH, buf, sizeof buf);
    gw_cfg_load(have ? buf : NULL, true, lan_iface, sizeof lan_iface);
    printf("gw: lan %s\n", lan_iface);

    have = cfg_file_read(GW_NAME_PATH, buf, sizeof buf);
    char name[64];
    const char *why = NULL;
    if (have && gw_hostname_parse(buf, name, sizeof name, &why)) {
        name_from_file = true;
    } else {
        pico_unique_board_id_t id;
        pico_get_unique_board_id(&id);
        gw_default_hostname("RP2", id.id, sizeof id.id, name, sizeof name);
        name_from_file = false;
    }
    if (mdns_set_hostname(name)) {
        snprintf(active_name, sizeof active_name, "%s", mdns_hostname());
    } else {
        lower_copy(active_name, sizeof active_name, name);
    }
    printf("gw: hostname %s (%s)\n", active_name, gw_name_source());
    gw_table_init(&peers);
}

void gw_lan_poll(void) {
    uint32_t now = now_ms();
    apply_queue();
    udp_age(now);
    gw_peer_expire(&peers, now);
    advertise(now);
}

#endif /* CONFIG_NET_GW_LAN */
