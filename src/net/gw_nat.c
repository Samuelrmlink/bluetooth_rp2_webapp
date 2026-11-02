/* Masquerade IPv4 from the gateway LAN out the current default route.
 * See gw_nat.h. TCP, UDP, and ICMP echo. Fragments that would be rewritten
 * are dropped. On-link traffic is forwarded unchanged. */
#include "net/gw_nat.h"

#if CONFIG_NET_NAT

#include <string.h>

#include "pico/stdlib.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/udp.h"
#include "lwip/udp.h"

#include "net/eth_w5500.h"
#include "net/gw_core.h"
#include "net/gw_lan.h"

#define GW_NAT_N    32
#define GW_NAT_PORT 49152
#define GW_NAT_IDLE 120000u

struct flow {
    uint8_t  used;
    uint8_t  proto;
    uint16_t client_port;
    uint16_t peer_port;
    uint16_t map_port;
    uint32_t client;
    uint32_t peer;
    uint32_t last_ms;
    char     out[3];
};

static struct flow tab[GW_NAT_N];
static int logged;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void nif_name(const struct netif *n, char out[3]) {
    out[0] = n ? n->name[0] : 0;
    out[1] = n ? n->name[1] : 0;
    out[2] = 0;
}

static bool on_lan(const struct netif *n) {
    char name[3];
    if (!n) { return false; }
    if (n == eth_netif() || n == eth_ll_netif()) { return true; }
    nif_name(n, name);
    return gw_is_lan_iface(name);
}

static bool addr_ours(const ip4_addr_t *a) {
    struct netif *n;
    if (!a || ip4_addr_isany(a) || ip4_addr_ismulticast(a)) { return true; }
    if (ip4_addr_get_u32(a) == PP_HTONL(0xffffffffu)) { return true; }
    NETIF_FOREACH(n) {
        if (!ip4_addr_isany_val(*netif_ip4_addr(n)) && ip4_addr_eq(a, netif_ip4_addr(n))) {
            return true;
        }
        if (ip4_addr_isbroadcast(a, n)) { return true; }
    }
    return false;
}

static bool same_lan(const ip4_addr_t *a) {
    struct netif *e0 = eth_netif();
    struct netif *ll = eth_ll_netif();
    if (ip4_addr_islinklocal(a)) { return true; }
    if (e0 && !ip4_addr_isany_val(*netif_ip4_addr(e0)) &&
        ip4_addr_net_eq(a, netif_ip4_addr(e0), netif_ip4_netmask(e0))) {
        return true;
    }
    if (ll && !ip4_addr_isany_val(*netif_ip4_addr(ll)) &&
        ip4_addr_net_eq(a, netif_ip4_addr(ll), netif_ip4_netmask(ll))) {
        return true;
    }
    return false;
}

static struct netif *lan_for(const ip4_addr_t *dest) {
    struct netif *ll = eth_ll_netif();
    struct netif *e0 = eth_netif();
    if (ll && !ip4_addr_isany_val(*netif_ip4_addr(ll)) &&
        (ip4_addr_islinklocal(dest) ||
         ip4_addr_net_eq(dest, netif_ip4_addr(ll), netif_ip4_netmask(ll)))) {
        return ll;
    }
    if (e0) { return e0; }
    return ll;
}

static int send_on(struct netif *out, struct pbuf *q, const ip4_addr_t *dest) {
    ip4_addr_t nh;
    if (!out || !out->output || !dest) { return 0; }
    if (!ip4_addr_isany_val(*netif_ip4_addr(out)) &&
        !ip4_addr_isany_val(*netif_ip4_netmask(out)) &&
        ip4_addr_net_eq(dest, netif_ip4_addr(out), netif_ip4_netmask(out))) {
        ip4_addr_copy(nh, *dest);
    } else if (!ip4_addr_isany_val(*netif_ip4_gw(out))) {
        ip4_addr_copy(nh, *netif_ip4_gw(out));
    } else {
        ip4_addr_copy(nh, *dest);
    }
    return out->output(out, q, &nh) == ERR_OK;
}

static void ip_checksum(struct ip_hdr *ip) {
    IPH_CHKSUM_SET(ip, 0);
    IPH_CHKSUM_SET(ip, inet_chksum(ip, IPH_HL_BYTES(ip)));
}

static int write_l4(struct pbuf *p, const ip4_addr_t *src, const ip4_addr_t *dst) {
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
    for (int i = 0; i < GW_NAT_N; i++) {
        if (tab[i].used && tab[i].map_port == port) { return 1; }
    }
    return 0;
}

static struct flow *lookup_out(uint8_t proto, uint32_t client, uint16_t cport,
                               uint32_t peer, uint16_t pport) {
    uint32_t now = now_ms();
    for (int i = 0; i < GW_NAT_N; i++) {
        struct flow *f = &tab[i];
        if (!f->used) { continue; }
        if ((uint32_t)(now - f->last_ms) > GW_NAT_IDLE) { f->used = 0; continue; }
        if (f->proto == proto && f->client == client && f->client_port == cport &&
            f->peer == peer && f->peer_port == pport) {
            f->last_ms = now;
            return f;
        }
    }
    return NULL;
}

static struct flow *lookup_in(uint8_t proto, uint16_t map_port, const char *out) {
    uint32_t now = now_ms();
    for (int i = 0; i < GW_NAT_N; i++) {
        struct flow *f = &tab[i];
        if (!f->used) { continue; }
        if ((uint32_t)(now - f->last_ms) > GW_NAT_IDLE) { f->used = 0; continue; }
        if (f->proto == proto && f->map_port == map_port && strcmp(f->out, out) == 0) {
            f->last_ms = now;
            return f;
        }
    }
    return NULL;
}

static struct flow *alloc_flow(uint8_t proto, uint32_t client, uint16_t cport,
                               uint32_t peer, uint16_t pport, const char *out) {
    uint32_t now = now_ms();
    struct flow *slot = NULL;
    for (int i = 0; i < GW_NAT_N; i++) {
        if (!tab[i].used || (uint32_t)(now - tab[i].last_ms) > GW_NAT_IDLE) {
            slot = &tab[i];
            slot->used = 0;
            break;
        }
    }
    if (!slot) { return NULL; }
    uint16_t port = 0;
    for (int i = 0; i < GW_NAT_N; i++) {
        uint16_t cand = (uint16_t)(GW_NAT_PORT + i);
        if (map_taken(cand) || port_busy(cand)) { continue; }
        port = cand;
        break;
    }
    if (!port) { return NULL; }
    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    slot->proto = proto;
    slot->client = client;
    slot->client_port = cport;
    slot->peer = peer;
    slot->peer_port = pport;
    slot->map_port = port;
    slot->last_ms = now;
    snprintf(slot->out, sizeof slot->out, "%s", out);
    return slot;
}

/* ICMP echo id sits 4 bytes into the header. TCP and UDP share src at 0
 * and dest at 2. Write the bytes so a packed header is never addressed. */
static void set_l4_field(struct ip_hdr *ip, int src, uint16_t host) {
    u8_t *l4 = (u8_t *)ip + IPH_HL_BYTES(ip);
    unsigned off;
    if (IPH_PROTO(ip) == IP_PROTO_ICMP) {
        if (!src) { return; }
        off = 4;
    } else {
        off = src ? 0u : 2u;
    }
    u16_t net = lwip_htons(host);
    memcpy(l4 + off, &net, sizeof net);
}

/* p is a private copy at the IP header. Eaten by the caller either way. */
static int xmit(struct pbuf *q, struct netif *out, const ip4_addr_t *dst) {
    struct ip_hdr *ip = (struct ip_hdr *)q->payload;
    u8_t ttl = IPH_TTL(ip);
    if (ttl <= 1) { pbuf_free(q); return 0; }
    IPH_TTL_SET(ip, (u8_t)(ttl - 1));
    ip4_addr_t src;
    ip4_addr_copy(src, ip->src);
    if (!write_l4(q, &src, dst)) { pbuf_free(q); return 0; }
    ip = (struct ip_hdr *)q->payload;
    ip_checksum(ip);
    int ok = send_on(out, q, dst);
    pbuf_free(q);
    return ok;
}

static int forward_raw(struct pbuf *p, struct netif *out) {
    struct ip_hdr *ip = (struct ip_hdr *)p->payload;
    u16_t iplen = lwip_ntohs(IPH_LEN(ip));
    struct pbuf *q = pbuf_alloc(PBUF_LINK, iplen, PBUF_RAM);
    if (!q) { pbuf_free(p); return 1; }
    pbuf_copy_partial(p, q->payload, iplen, 0);
    pbuf_free(p);
    ip4_addr_t dst;
    ip = (struct ip_hdr *)q->payload;
    ip4_addr_copy(dst, ip->dest);
    xmit(q, out, &dst);
    return 1;
}

static int masquerade(struct pbuf *p, struct netif *out) {
    struct ip_hdr *ip = (struct ip_hdr *)p->payload;
    u16_t ihl = IPH_HL_BYTES(ip);
    u16_t iplen = lwip_ntohs(IPH_LEN(ip));
    u8_t proto = IPH_PROTO(ip);
    uint16_t sport = 0, dport = 0;
    if (p->len < (u16_t)(ihl + 8) || iplen < (u16_t)(ihl + 8)) { return 0; }
    if (proto == IP_PROTO_ICMP) {
        struct icmp_echo_hdr *ie = (struct icmp_echo_hdr *)((u8_t *)ip + ihl);
        if (ie->type != ICMP_ECHO) { return 0; }
        sport = lwip_ntohs(ie->id);
        dport = 0;
    } else if (proto == IP_PROTO_TCP || proto == IP_PROTO_UDP) {
        struct udp_hdr *uh = (struct udp_hdr *)((u8_t *)ip + ihl);
        sport = lwip_ntohs(uh->src);
        dport = lwip_ntohs(uh->dest);
    } else {
        return 0;
    }
    char oname[3];
    nif_name(out, oname);
    uint32_t src_n = ip4_addr_get_u32(&ip->src);
    uint32_t dst_n = ip4_addr_get_u32(&ip->dest);
    struct flow *f = lookup_out(proto, src_n, sport, dst_n, dport);
    if (!f) { f = alloc_flow(proto, src_n, sport, dst_n, dport, oname); }
    if (!f) { pbuf_free(p); return 1; }

    struct pbuf *q = pbuf_alloc(PBUF_LINK, iplen, PBUF_RAM);
    if (!q) { pbuf_free(p); return 1; }
    pbuf_copy_partial(p, q->payload, iplen, 0);
    pbuf_free(p);
    ip = (struct ip_hdr *)q->payload;
    ip4_addr_copy(ip->src, *netif_ip4_addr(out));
    set_l4_field(ip, 1, f->map_port);
    ip4_addr_t dst;
    ip4_addr_copy(dst, ip->dest);
    if (!logged) {
        logged = 1;
        printf("gw: nat via %s\n", oname);
    }
    xmit(q, out, &dst);
    return 1;
}

static int reverse(struct pbuf *p, struct netif *inp) {
    struct ip_hdr *ip = (struct ip_hdr *)p->payload;
    if (!ip4_addr_eq(&ip->dest, netif_ip4_addr(inp))) { return 0; }
    u16_t ihl = IPH_HL_BYTES(ip);
    u16_t iplen = lwip_ntohs(IPH_LEN(ip));
    if (ihl < 20 || iplen > p->tot_len || p->len < (u16_t)(ihl + 8)) { return 0; }
    if (lwip_ntohs(IPH_OFFSET(ip)) & (IP_OFFMASK | IP_MF)) { return 0; }
    u8_t proto = IPH_PROTO(ip);
    uint16_t dport;
    if (proto == IP_PROTO_ICMP) {
        struct icmp_echo_hdr *ie = (struct icmp_echo_hdr *)((u8_t *)ip + ihl);
        if (ie->type != ICMP_ER) { return 0; }
        dport = lwip_ntohs(ie->id);
    } else if (proto == IP_PROTO_TCP || proto == IP_PROTO_UDP) {
        dport = lwip_ntohs(((struct udp_hdr *)((u8_t *)ip + ihl))->dest);
    } else {
        return 0;
    }
    char oname[3];
    nif_name(inp, oname);
    struct flow *f = lookup_in(proto, dport, oname);
    if (!f) { return 0; }

    struct pbuf *q = pbuf_alloc(PBUF_LINK, iplen, PBUF_RAM);
    if (!q) { pbuf_free(p); return 1; }
    pbuf_copy_partial(p, q->payload, iplen, 0);
    pbuf_free(p);
    ip = (struct ip_hdr *)q->payload;
    ip4_addr_t client, peer;
    ip4_addr_set_u32(&client, f->client);
    ip4_addr_copy(ip->dest, client);
    set_l4_field(ip, proto == IP_PROTO_ICMP ? 1 : 0, f->client_port);
    ip4_addr_copy(peer, ip->src);
    struct netif *lan = lan_for(&client);
    xmit(q, lan, &client);
    (void)peer;
    return 1;
}

int gw_nat_ip4_input(struct pbuf *p, struct netif *inp) {
    struct ip_hdr *ip;
    if (!p || !inp || p->len < IP_HLEN) { return 0; }
    if (strcmp(gw_lan_iface(), "none") == 0) { return 0; }
    ip = (struct ip_hdr *)p->payload;
    if (IPH_V(ip) != 4) { return 0; }
    u16_t ihl = IPH_HL_BYTES(ip);
    if (ihl < IP_HLEN || p->len < ihl) { return 0; }

    if (!on_lan(inp)) { return reverse(p, inp); }

    ip4_addr_t dest;
    ip4_addr_copy(dest, ip->dest);
    if (addr_ours(&dest)) { return 0; }
    if (lwip_ntohs(IPH_OFFSET(ip)) & (IP_OFFMASK | IP_MF)) {
        pbuf_free(p);
        return 1;
    }
    if (same_lan(&dest)) { return forward_raw(p, lan_for(&dest)); }

    struct netif *out = netif_default;
    char oname[3];
    nif_name(out, oname);
    if (!out || !gw_nat_out_ok(oname, gw_lan_iface())) {
        /* Peer route: the default netif is the link-local companion. Forward
         * unchanged so the other board masquerades. A tunnel is not an egress. */
        if (out && (strcmp(oname, "el") == 0 || gw_is_lan_iface(oname))) {
            return forward_raw(p, eth_ll_netif() ? eth_ll_netif() : out);
        }
        return 0;
    }
    if (ip4_addr_isany_val(*netif_ip4_addr(out))) { return 0; }
    return masquerade(p, out);
}

#endif /* CONFIG_NET_NAT */
