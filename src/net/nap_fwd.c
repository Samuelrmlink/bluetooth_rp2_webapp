/* Masquerade the phone's 192.168.44.0/24 traffic out one uplink.
 * See nap_fwd.h. IPv4 TCP, UDP, and ICMP echo only. One table, no heap
 * beyond the packet copy used to send. */
#include "net/nap_fwd.h"

#include <string.h>

#include "bt/bt.h"
#include "net/wg.h"
#include "lwip/dns.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/udp.h"
#include "lwip/udp.h"
#include "pico/stdlib.h"

#define NAP_FWD_N     128
#define NAP_FWD_PORT0 61000
#define NAP_FWD_IDLE  120000u
#define NAP_NET       0xc0a82c00u /* 192.168.44.0 */

struct flow {
    uint8_t  used;
    uint8_t  proto;
    uint16_t phone_port;
    uint16_t peer_port;
    uint16_t map_port;
    uint32_t phone;
    uint32_t peer;
    uint32_t last_ms;
};

static struct flow s_tab[NAP_FWD_N];
static struct netif *s_lan;
static char s_iface[4] = "w0";
static int s_on;
static int s_logged;
static uint32_t s_tx, s_rx, s_drop;

enum {
    DR_ICMP, DR_PROTO, DR_WAN, DR_FULL, DR_MEM,
    DR_TTL, DR_MTU, DR_SUM, DR_SEND, DR_BACK, DR_N
};
static uint8_t s_drop_seen[DR_N];
static const char *const s_drop_name[DR_N] = {
    "icmp", "proto", "wan", "full", "mem", "ttl", "mtu", "sum", "send", "back"
};

static void drop(int why) {
    s_drop++;
    if (why < 0 || why >= DR_N || s_drop_seen[why]) { return; }
    s_drop_seen[why] = 1;
    bt_log("info", "BNEP forward drop %s", s_drop_name[why]);
}

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static int port_busy(uint16_t port) {
    for (struct tcp_pcb *t = tcp_active_pcbs; t; t = t->next) {
        if (t->local_port == port) { return 1; }
    }
    for (struct tcp_pcb *t = tcp_tw_pcbs; t; t = t->next) {
        if (t->local_port == port) { return 1; }
    }
    for (struct tcp_pcb *t = tcp_bound_pcbs; t; t = t->next) {
        if (t->local_port == port) { return 1; }
    }
    for (struct tcp_pcb_listen *t = tcp_listen_pcbs.listen_pcbs; t; t = t->next) {
        if (t->local_port == port) { return 1; }
    }
    for (struct udp_pcb *u = udp_pcbs; u; u = u->next) {
        if (u->local_port == port) { return 1; }
    }
    return 0;
}

static int map_taken(uint16_t port) {
    for (int i = 0; i < NAP_FWD_N; i++) {
        if (s_tab[i].used && s_tab[i].map_port == port) { return 1; }
    }
    return 0;
}

static int iface_is_wg(void) {
    return s_iface[0] == 'w' && s_iface[1] == 'g' && !s_iface[2];
}

static struct netif *wan(void) {
    if (!s_iface[0]) { return NULL; }
    for (struct netif *n = netif_list; n; n = n->next) {
        if (n->name[0] == s_iface[0] && n->name[1] == s_iface[1]) { return n; }
    }
    return NULL;
}

static int wan_ok(struct netif *n) {
    return n && netif_is_up(n) && netif_is_link_up(n) &&
           !ip4_addr_isany(netif_ip4_addr(n)) &&
           !ip4_addr_isany(netif_ip4_gw(n));
}

void nap_fwd_set(int on, const char *iface, struct netif *lan) {
    if (lan) { s_lan = lan; }
    if (iface && iface[0] && iface[1] && !iface[2] &&
        !(iface[0] == 'b' && iface[1] == '0')) {
        if (s_iface[0] != iface[0] || s_iface[1] != iface[1]) {
            memset(s_tab, 0, sizeof s_tab);
        }
        s_iface[0] = iface[0];
        s_iface[1] = iface[1];
        s_iface[2] = 0;
    }
    if (!s_iface[0]) {
        s_iface[0] = 'w';
        s_iface[1] = '0';
        s_iface[2] = 0;
    }
    if (!on) { memset(s_tab, 0, sizeof s_tab); }
    s_on = on ? 1 : 0;
    s_logged = 0;
}

int nap_fwd_on(void) { return s_on; }
const char *nap_fwd_iface(void) { return s_iface; }

const char *nap_fwd_why(void) {
    struct netif *n;
    if (!s_on) { return ""; }
    n = wan();
    if (!n || !netif_is_up(n) || !netif_is_link_up(n) ||
        ip4_addr_isany(netif_ip4_addr(n))) {
        return "down";
    }
    if (ip4_addr_isany(netif_ip4_gw(n))) { return "no gateway"; }
    return "";
}

void nap_fwd_counts(uint32_t *tx, uint32_t *rx, uint32_t *drop) {
    if (tx) { *tx = s_tx; }
    if (rx) { *rx = s_rx; }
    if (drop) { *drop = s_drop; }
}

static int private_v4(uint32_t h) {
    uint8_t a = (uint8_t)(h >> 24);
    uint8_t b = (uint8_t)(h >> 16);
    if (a == 10 || a == 127) { return 1; }
    if (a == 192 && b == 168) { return 1; }
    if (a == 172 && b >= 16 && b <= 31) { return 1; }
    if (a == 169 && b == 254) { return 1; }
    return 0;
}

int nap_fwd_resolver(ip4_addr_t *out) {
    const ip_addr_t *d;
    if (!s_on || !out) { return 0; }
    d = dns_getserver(0);
    if (d && !ip4_addr_isany(ip_2_ip4(d))) {
        uint32_t h = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(d)));
        if ((h & 0xffffff00u) != NAP_NET && !(iface_is_wg() && private_v4(h))) {
            ip4_addr_copy(*out, *ip_2_ip4(d));
            return 1;
        }
    }
    IP4_ADDR(out, 1, 1, 1, 1);
    return 1;
}

void nap_fwd_tick(void) {
    uint32_t now;
    if (!s_on) { return; }
    now = now_ms();
    for (int i = 0; i < NAP_FWD_N; i++) {
        if (s_tab[i].used && (uint32_t)(now - s_tab[i].last_ms) > NAP_FWD_IDLE) {
            s_tab[i].used = 0;
        }
    }
}

static struct flow *lookup_out(uint8_t proto, uint32_t phone, uint16_t php,
                                uint32_t peer, uint16_t pep) {
    uint32_t now = now_ms();
    for (int i = 0; i < NAP_FWD_N; i++) {
        struct flow *f = &s_tab[i];
        if (!f->used) { continue; }
        if ((uint32_t)(now - f->last_ms) > NAP_FWD_IDLE) {
            f->used = 0;
            continue;
        }
        if (f->proto == proto && f->phone == phone && f->phone_port == php &&
            f->peer == peer && f->peer_port == pep) {
            f->last_ms = now;
            return f;
        }
    }
    return NULL;
}

static struct flow *lookup_in(uint8_t proto, uint16_t map) {
    uint32_t now = now_ms();
    for (int i = 0; i < NAP_FWD_N; i++) {
        struct flow *f = &s_tab[i];
        if (!f->used) { continue; }
        if ((uint32_t)(now - f->last_ms) > NAP_FWD_IDLE) {
            f->used = 0;
            continue;
        }
        if (f->proto == proto && f->map_port == map) {
            f->last_ms = now;
            return f;
        }
    }
    return NULL;
}

static struct flow *alloc_flow(uint8_t proto, uint32_t phone, uint16_t php,
                                uint32_t peer, uint16_t pep) {
    struct flow *slot = NULL;
    uint16_t port = 0;
    uint32_t now = now_ms();
    for (int i = 0; i < NAP_FWD_N; i++) {
        if (!s_tab[i].used || (uint32_t)(now - s_tab[i].last_ms) > NAP_FWD_IDLE) {
            slot = &s_tab[i];
            slot->used = 0;
            break;
        }
    }
    if (!slot) {
        /* A phone opens more flows than the table holds. The oldest one
         * is replaced so a new DNS query or ping is not stuck for the
         * whole idle time. */
        slot = &s_tab[0];
        for (int i = 1; i < NAP_FWD_N; i++) {
            if ((uint32_t)(now - s_tab[i].last_ms) >
                (uint32_t)(now - slot->last_ms)) {
                slot = &s_tab[i];
            }
        }
        slot->used = 0;
    }
    for (int i = 0; i < NAP_FWD_N; i++) {
        uint16_t cand = (uint16_t)(NAP_FWD_PORT0 + i);
        if (map_taken(cand) || port_busy(cand)) { continue; }
        port = cand;
        break;
    }
    if (!port) { return NULL; }
    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    slot->proto = proto;
    slot->phone = phone;
    slot->phone_port = php;
    slot->peer = peer;
    slot->peer_port = pep;
    slot->map_port = port;
    slot->last_ms = now;
    return slot;
}

static void ip_checksum(struct ip_hdr *ip) {
    IPH_CHKSUM_SET(ip, 0);
    IPH_CHKSUM_SET(ip, inet_chksum(ip, IPH_HL_BYTES(ip)));
}

/* p starts at the IP header and its tot_len is the IP length. */
static int write_l4_checksum(struct pbuf *p, const ip4_addr_t *src, const ip4_addr_t *dst) {
    struct ip_hdr *ip = (struct ip_hdr *)p->payload;
    u16_t ihl = IPH_HL_BYTES(ip);
    u16_t l4len = (u16_t)(lwip_ntohs(IPH_LEN(ip)) - ihl);
    u8_t proto = IPH_PROTO(ip);
    if (pbuf_remove_header(p, ihl) != 0) { return 0; }
    if (p->tot_len != l4len) { pbuf_add_header(p, ihl); return 0; }
    if (proto == IP_PROTO_ICMP) {
        struct icmp_echo_hdr *ie = (struct icmp_echo_hdr *)p->payload;
        if (p->len < 8) { pbuf_add_header(p, ihl); return 0; }
        ie->chksum = 0;
        ie->chksum = inet_chksum_pbuf(p);
    } else if (proto == IP_PROTO_TCP) {
        struct tcp_hdr *th = (struct tcp_hdr *)p->payload;
        if (p->len < 20) { pbuf_add_header(p, ihl); return 0; }
        th->chksum = 0;
        th->chksum = inet_chksum_pseudo(p, IP_PROTO_TCP, l4len, src, dst);
    } else {
        struct udp_hdr *uh = (struct udp_hdr *)p->payload;
        if (p->len < 8) { pbuf_add_header(p, ihl); return 0; }
        uh->chksum = 0;
        uh->chksum = inet_chksum_pseudo(p, IP_PROTO_UDP, l4len, src, dst);
        if (uh->chksum == 0) { uh->chksum = 0xffff; }
    }
    pbuf_add_header(p, ihl);
    return 1;
}

/* A SYN's MSS has to fit the exit. The tunnel's MTU is 1420, and the
 * phone would otherwise advertise whatever BNEP gave it. */
static void clamp_mss(struct pbuf *p, u16_t mtu) {
    struct ip_hdr *ip;
    struct tcp_hdr *th;
    u8_t *opt;
    u16_t ihl, hlen, left, mss;
    if (!p || mtu <= 40) { return; }
    ip = (struct ip_hdr *)p->payload;
    if (IPH_PROTO(ip) != IP_PROTO_TCP) { return; }
    ihl = IPH_HL_BYTES(ip);
    if (p->len < (u16_t)(ihl + 20)) { return; }
    th = (struct tcp_hdr *)((u8_t *)ip + ihl);
    if ((TCPH_FLAGS(th) & TCP_SYN) == 0) { return; }
    hlen = TCPH_HDRLEN_BYTES(th);
    if (hlen <= 20 || p->len < (u16_t)(ihl + hlen)) { return; }
    mss = (u16_t)(mtu - 40);
    opt = (u8_t *)th + 20;
    left = (u16_t)(hlen - 20);
    while (left > 0) {
        u8_t kind = opt[0];
        u8_t len;
        if (kind == 0) { return; }
        if (kind == 1) { opt++; left--; continue; }
        if (left < 2) { return; }
        len = opt[1];
        if (len < 2 || len > left) { return; }
        if (kind == 2 && len == 4) {
            u16_t cur = (u16_t)((opt[2] << 8) | opt[3]);
            if (cur > mss) {
                opt[2] = (u8_t)(mss >> 8);
                opt[3] = (u8_t)mss;
            }
            return;
        }
        opt += len;
        left = (u16_t)(left - len);
    }
}

static int send_via(struct netif *out, struct pbuf *q, const ip4_addr_t *dest) {
    ip4_addr_t nh;
    if (out->name[0] == 'w' && out->name[1] == 'g') {
        return wg_output(q, dest);
    }
    if (ip4_addr_netcmp(dest, netif_ip4_addr(out), netif_ip4_netmask(out))) {
        ip4_addr_copy(nh, *dest);
    } else {
        ip4_addr_copy(nh, *netif_ip4_gw(out));
    }
    return out->output(out, q, &nh) == ERR_OK;
}

static uint16_t *flow_port(struct ip_hdr *ip, int src) {
    u8_t *l4 = (u8_t *)ip + IPH_HL_BYTES(ip);
    if (IPH_PROTO(ip) == IP_PROTO_ICMP) {
        return src ? &((struct icmp_echo_hdr *)l4)->id : NULL;
    }
    return src ? &((struct udp_hdr *)l4)->src : &((struct udp_hdr *)l4)->dest;
}

int nap_fwd_out(struct pbuf *p) {
    struct ip_hdr *ip;
    struct netif *out;
    struct flow *f;
    struct pbuf *q;
    uint32_t src_h, dst_h, src_n, dst_n;
    uint16_t sport, dport, ihl, iplen;
    u8_t proto, ttl;
    u8_t *frame;
    int dns_gw = 0;
    if (!s_on || !p || p->len < 14 + 20) { return 0; }
    frame = (u8_t *)p->payload;
    if (((frame[12] << 8) | frame[13]) != 0x0800) { return 0; }
    ip = (struct ip_hdr *)(frame + 14);
    if (IPH_V(ip) != 4) { return 0; }
    ihl = IPH_HL_BYTES(ip);
    iplen = lwip_ntohs(IPH_LEN(ip));
    if (ihl < 20 || (uint16_t)(14 + iplen) > p->tot_len || p->len < (uint16_t)(14 + ihl + 8)) {
        return 0;
    }
    if (lwip_ntohs(IPH_OFFSET(ip)) & (IP_OFFMASK | IP_MF)) { return 0; }
    src_n = ip4_addr_get_u32(&ip->src);
    dst_n = ip4_addr_get_u32(&ip->dest);
    src_h = lwip_ntohl(src_n);
    dst_h = lwip_ntohl(dst_n);
    if ((src_h & 0xffffff00u) != NAP_NET) { return 0; }
    if ((u8_t)src_h == 0 || (u8_t)src_h == 1 || (u8_t)src_h == 255) { return 0; }
    proto = IPH_PROTO(ip);
    /* The phone often uses the board as its DNS server. Nothing on the
     * board answers port 53, so those queries are sent to the upstream
     * resolver and the reply is rewritten to look like it came from the
     * board. */
    if (proto == IP_PROTO_UDP && dst_h == (NAP_NET | 1u)) {
        struct udp_hdr *uh = (struct udp_hdr *)((u8_t *)ip + ihl);
        if (lwip_ntohs(uh->dest) == 53) { dns_gw = 1; }
    }
    if (!dns_gw && ((dst_h & 0xffffff00u) == NAP_NET || (dst_h >> 28) == 0xe ||
                    dst_h == 0xffffffffu || dst_h == 0)) {
        return 0;
    }
    if (proto == IP_PROTO_ICMP) {
        struct icmp_echo_hdr *ie = (struct icmp_echo_hdr *)((u8_t *)ip + ihl);
        if (ie->type != ICMP_ECHO) { drop(DR_ICMP); return 1; }
        sport = lwip_ntohs(ie->id);
        dport = 0;
    } else if (proto == IP_PROTO_TCP || proto == IP_PROTO_UDP) {
        struct udp_hdr *uh = (struct udp_hdr *)((u8_t *)ip + ihl);
        sport = lwip_ntohs(uh->src);
        dport = lwip_ntohs(uh->dest);
    } else {
        drop(DR_PROTO);
        return 1;
    }
    out = wan();
    if (!wan_ok(out)) { drop(DR_WAN); return 1; }
    f = lookup_out(proto, src_n, sport, dst_n, dport);
    if (!f) { f = alloc_flow(proto, src_n, sport, dst_n, dport); }
    if (!f) { drop(DR_FULL); return 1; }

    q = pbuf_alloc(PBUF_LINK, iplen, PBUF_RAM);
    if (!q) { drop(DR_MEM); return 1; }
    pbuf_copy_partial(p, q->payload, iplen, 14);
    ip = (struct ip_hdr *)q->payload;
    ttl = IPH_TTL(ip);
    if (ttl <= 1) {
        pbuf_free(q);
        drop(DR_TTL);
        return 1;
    }
    IPH_TTL_SET(ip, ttl - 1);
    ip4_addr_copy(ip->src, *netif_ip4_addr(out));
    {
        uint16_t *field = flow_port(ip, 1);
        if (!field) { pbuf_free(q); drop(DR_SUM); return 1; }
        *field = lwip_htons(f->map_port);
    }
    if (q->tot_len != iplen) { pbuf_realloc(q, iplen); }
    if (out->mtu && q->tot_len > out->mtu) {
        pbuf_free(q);
        drop(DR_MTU);
        return 1;
    }
    clamp_mss(q, out->mtu);
    ip4_addr_t dst;
    memcpy(&dst.addr, (uint8_t *)ip + 16, 4);
    if (dns_gw) {
        if (!nap_fwd_resolver(&dst)) {
            pbuf_free(q);
            drop(DR_WAN);
            return 1;
        }
        memcpy((uint8_t *)ip + 16, &dst.addr, 4);
    }
    if (!write_l4_checksum(q, netif_ip4_addr(out), &dst)) {
        pbuf_free(q);
        drop(DR_SUM);
        return 1;
    }
    ip = (struct ip_hdr *)q->payload;
    ip_checksum(ip);
    if (!s_logged) {
        s_logged = 1;
        bt_log("info", "BNEP forward via %s", s_iface);
    }
    if (!send_via(out, q, &dst)) { drop(DR_SEND); }
    else { s_tx++; }
    pbuf_free(q);
    return 1;
}

int nap_fwd_ip4_input(struct pbuf *p, struct netif *inp) {
    struct ip_hdr *ip;
    struct netif *out;
    struct flow *f;
    ip4_addr_t phone, peer;
    uint16_t dport, ihl, iplen;
    u8_t proto;
    if (!s_on || !s_lan || !p) { return 0; }
    out = wan();
    if (inp != out || !wan_ok(out)) { return 0; }
    if (p->len < 20) { return 0; }
    ip = (struct ip_hdr *)p->payload;
    if (IPH_V(ip) != 4) { return 0; }
    if (!ip4_addr_cmp(&ip->dest, netif_ip4_addr(out))) { return 0; }
    ihl = IPH_HL_BYTES(ip);
    iplen = lwip_ntohs(IPH_LEN(ip));
    if (ihl < 20 || iplen > p->tot_len || p->len < (uint16_t)(ihl + 8)) { return 0; }
    if (lwip_ntohs(IPH_OFFSET(ip)) & (IP_OFFMASK | IP_MF)) { return 0; }
    proto = IPH_PROTO(ip);
    if (proto == IP_PROTO_ICMP) {
        struct icmp_echo_hdr *ie = (struct icmp_echo_hdr *)((u8_t *)ip + ihl);
        if (ie->type != ICMP_ER) { return 0; }
        dport = lwip_ntohs(ie->id);
    } else if (proto == IP_PROTO_TCP || proto == IP_PROTO_UDP) {
        dport = lwip_ntohs(((struct udp_hdr *)((u8_t *)ip + ihl))->dest);
    } else {
        return 0;
    }
    f = lookup_in(proto, dport);
    if (!f) { return 0; }
    if (p->tot_len != iplen) { pbuf_realloc(p, iplen); }
    ip = (struct ip_hdr *)p->payload;
    ip4_addr_set_u32(&phone, f->phone);
    ip4_addr_copy(ip->dest, phone);
    {
        uint16_t *field = flow_port(ip, 0);
        if (proto == IP_PROTO_ICMP) { field = flow_port(ip, 1); }
        if (!field) { drop(DR_SUM); pbuf_free(p); return 1; }
        *field = lwip_htons(f->phone_port);
    }
    ip4_addr_copy(peer, ip->src);
    /* A DNS query that was aimed at the board has to come back from the
     * board, or the phone will not match it to the question. */
    if ((lwip_ntohl(f->peer) & 0xffffff00u) == NAP_NET) {
        memcpy((uint8_t *)ip + 12, &f->peer, 4);
        memcpy(&peer.addr, &f->peer, 4);
    }
    if (out->mtu && p->tot_len > out->mtu) {
        drop(DR_MTU);
        pbuf_free(p);
        return 1;
    }
    clamp_mss(p, out->mtu);
    if (!write_l4_checksum(p, &peer, &phone)) {
        drop(DR_SUM);
        pbuf_free(p);
        return 1;
    }
    ip = (struct ip_hdr *)p->payload;
    ip_checksum(ip);
    if (!s_lan->output || !send_via(s_lan, p, &phone)) { drop(DR_BACK); }
    else { s_rx++; }
    pbuf_free(p);
    return 1;
}
