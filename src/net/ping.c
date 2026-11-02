/* Callable ping. See ping.h. The uplink probe is a different raw pcb and a
 * different ICMP id (PING_ID_BASE in uplink.c). */
#include "net/ping.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/raw.h"

#include "api/api.h"
#include "api/ops.h"
#include "autoconf.h"

/* Below uplink's 0xB7E0 base, and clear of 0xB7E0 .. 0xB7E0+candidates. */
#define PING_ID          0xB750
#define PING_TIMEOUT_MS  1000

enum { PING_IDLE = 0, PING_WAIT, PING_REPLY, PING_TIMEOUT };

static struct raw_pcb *pcb;
static int state = PING_IDLE;
static uint16_t seq;
static ip4_addr_t dest;
static uint32_t sent_ms, rtt_ms;
static char dest_str[16];
static char iface[8];

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void note_iface(struct netif *nif) {
    /* The driver stores the product name in the two characters ("e0", "w0").
     * netif->num is lwIP's separate counter and would turn that into "e02". */
    snprintf(iface, sizeof iface, "%c%c", nif->name[0], nif->name[1]);
}

/* On-link first, including 169.254/16 on the netif that holds a link-local
 * address. Otherwise the default route, and only when that netif has a
 * gateway. An address with no such path is an error, not a silent skip. */
static struct netif *route_for(const ip4_addr_t *dst) {
    struct netif *nif;
    NETIF_FOREACH(nif) {
        if (!netif_is_up(nif) || !netif_is_link_up(nif)) { continue; }
        if (ip4_addr_isany(netif_ip4_addr(nif))) { continue; }
        if (ip4_addr_netcmp(dst, netif_ip4_addr(nif), netif_ip4_netmask(nif))) {
            return nif;
        }
    }
    nif = netif_default;
    if (nif && netif_is_up(nif) && netif_is_link_up(nif) &&
        !ip4_addr_isany(netif_ip4_addr(nif)) && !ip4_addr_isany(netif_ip4_gw(nif))) {
        return nif;
    }
    return NULL;
}

static void emit(void) {
    char b[120];
    ping_status_json(b, sizeof b);
    api_emit("net.ping", b);
}

static u8_t ping_recv(void *arg, struct raw_pcb *raw, struct pbuf *p, const ip_addr_t *addr) {
    (void)arg; (void)raw;
    struct ip_hdr iph;
    if (pbuf_copy_partial(p, &iph, sizeof iph, 0) != sizeof iph) { return 0; }
    u16_t hl = (u16_t)IPH_HL_BYTES(&iph);
    struct icmp_echo_hdr e;
    if (pbuf_copy_partial(p, &e, sizeof e, hl) != sizeof e) { return 0; }
    if (ICMPH_TYPE(&e) != ICMP_ER || lwip_ntohs(e.id) != PING_ID) { return 0; }
    if (state != PING_WAIT || lwip_ntohs(e.seqno) != seq || !ip4_addr_eq(ip_2_ip4(addr), &dest)) {
        pbuf_free(p);
        return 1;
    }
    state = PING_REPLY;
    rtt_ms = now_ms() - sent_ms;
    pbuf_free(p);
    printf("ping: %s via %s %lu ms\n", dest_str, iface, (unsigned long)rtt_ms);
    emit();
    return 1;
}

static bool ensure_pcb(void) {
    if (pcb) { return true; }
    pcb = raw_new(IP_PROTO_ICMP);
    if (!pcb) { return false; }
    raw_recv(pcb, ping_recv, NULL);
    return true;
}

static bool send_echo(struct netif *nif) {
    const u16_t len = sizeof(struct icmp_echo_hdr) + 8;
    struct pbuf *p = pbuf_alloc(PBUF_IP, len, PBUF_RAM);
    if (!p) { return false; }
    struct icmp_echo_hdr *e = (struct icmp_echo_hdr *)p->payload;
    ICMPH_TYPE_SET(e, ICMP_ECHO);
    ICMPH_CODE_SET(e, 0);
    e->id = lwip_htons(PING_ID);
    e->seqno = lwip_htons(seq);
    memset((uint8_t *)e + sizeof *e, 0x5a, 8);
    e->chksum = 0;
    e->chksum = inet_chksum(e, len);
    ip_addr_t dst, src;
    ip_addr_copy_from_ip4(dst, dest);
    ip_addr_copy_from_ip4(src, *netif_ip4_addr(nif));
    err_t err = raw_sendto_if_src(pcb, p, &dst, nif, &src);
    pbuf_free(p);
    return err == ERR_OK;
}

bool ping_start(const char *ip, const char **why) {
    ip4_addr_t d;
    if (!ip || !ip4addr_aton(ip, &d) || ip4_addr_isany_val(d)) {
        if (why) { *why = "need an IPv4 address"; }
        return false;
    }
    if (state == PING_WAIT) {
        if (why) { *why = "busy"; }
        return false;
    }
    struct netif *nif = route_for(&d);
    if (!nif) {
        if (why) { *why = "no route"; }
        return false;
    }
    if (!ensure_pcb()) {
        if (why) { *why = "no socket"; }
        return false;
    }
    dest = d;
    snprintf(dest_str, sizeof dest_str, "%s", ip4addr_ntoa(&dest));
    note_iface(nif);
    seq++;
    if (!seq) { seq = 1; }
    if (!send_echo(nif)) {
        if (why) { *why = "send failed"; }
        return false;
    }
    sent_ms = now_ms();
    state = PING_WAIT;
    printf("ping: %s via %s\n", dest_str, iface);
    return true;
}

size_t ping_status_json(char *dst, size_t cap) {
    if (!dst || !cap) { return 0; }
    if (state == PING_IDLE) {
        return (size_t)snprintf(dst, cap, "{\"state\":\"idle\"}");
    }
    const char *name = state == PING_WAIT ? "sent" : state == PING_REPLY ? "reply" : "timeout";
    if (state == PING_REPLY) {
        return (size_t)snprintf(dst, cap,
                                "{\"state\":\"%s\",\"ip\":\"%s\",\"iface\":\"%s\",\"id\":%u,\"rtt_ms\":%lu}",
                                name, dest_str, iface, (unsigned)seq, (unsigned long)rtt_ms);
    }
    return (size_t)snprintf(dst, cap,
                            "{\"state\":\"%s\",\"ip\":\"%s\",\"iface\":\"%s\",\"id\":%u}",
                            name, dest_str, iface, (unsigned)seq);
}

void ping_tick(void) {
    if (state == PING_WAIT && (uint32_t)(now_ms() - sent_ms) >= PING_TIMEOUT_MS) {
        state = PING_TIMEOUT;
        printf("ping: %s via %s timeout\n", dest_str, iface);
        emit();
    }
}

void ping_op(const char *params, api_reply_fn reply, void *ctx, long id) {
    char ip[20];
    ip[0] = 0;
    if (params && api_json_str(params, "ip", ip, sizeof ip) && ip[0]) {
        const char *why = "bad request";
        if (!ping_start(ip, &why)) {
            api_reply_err(reply, ctx, id, "bad_request", why);
            return;
        }
    }
    ping_status_json(api_scratch(), api_scratch_size());
    api_reply_ok(reply, ctx, id, api_scratch());
}

#if !CONFIG_NET_FORWARD
/* With forwarding on, fwd.c owns op_module_net and lists net.ping itself. */
static const op_def_t net_ops[] = {
    { "net.ping", ping_op },
    { NULL, NULL }
};
static const char *const net_topics[] = { "net.ping", NULL };
const op_module_t op_module_net = {
    .ns = "net", .ops = net_ops, .topics = net_topics, .init = NULL
};
#endif
