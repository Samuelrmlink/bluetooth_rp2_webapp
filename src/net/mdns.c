/* mDNS responder: a ".local" hostname and a runtime-configurable set of
 * advertised services. See mdns.h for which record type comes from where.
 *
 * Originally a fixed "_http on 80" responder adapted from the pm220-pico2w
 * reference. The services are now a table so they can be declared from Lua,
 * which is where device-specific naming belongs. */
#include "net/mdns.h"

#if CONFIG_NET_MDNS

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#include "lwip/apps/mdns.h"
#include "lwip/apps/mdns_priv.h"   /* get_mdns_pcb() */
#include "lwip/netif.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"

#include "net/mdns_records.h"
#include "net/net_log.h"
#include "api/ops.h"
#include "util/crumb.h"
#include "api/api.h"
#include "config/config.h"

#define MDNS_NAME_PATH "cfg/hostname.json"

static bool host_dirty;
static char host_pending[64];
static void send_hostname_goodbye(const char *old);

/* Larger first: tools/run_tests.py reads the first numeric define. The
 * companion netif is the fifth binding, and only while the gateway LAN
 * is in the image. */
#if CONFIG_NET_GW_LAN
#define MDNS_NETIFS 5
#else
#define MDNS_NETIFS 4
#endif

static char hostname[64] = BTGW_MDNS_HOSTNAME;
static struct netif *bound[MDNS_NETIFS];
/* Netifs held while the responder is off, so turning it back on rejoins. */
static struct netif *parked[MDNS_NETIFS];
static bool responder_on = true;
static uint32_t mdns_drops, mdns_drops_logged, mdns_drop_log_ms;

typedef struct {
    bool     used;
    bool     track_hostname;         /* instance follows the hostname */
    char     instance[48];
    char     type[24];               /* "_http" */
    bool     udp;
    uint16_t port;
    char     txt[MDNS_TXT_MAX][MDNS_TXT_LEN + 1];
    int      n_txt;
    s8_t     slot[MDNS_NETIFS];      /* lwIP's handle, per bound netif */
    int      owner;                  /* script slot that added it; -1 = nobody */
} service_t;

static service_t svc[MDNS_SERVICES_MAX];

/* lwIP asks for the TXT items at announce time rather than taking a copy, so
 * this has to find its way back to the right service. The userdata is the
 * table index, which is stable for the life of the entry. */
static void srv_txt(struct mdns_service *service, void *txt_userdata) {
    size_t i = (size_t)(uintptr_t)txt_userdata;
    crumb_d(CRUMB_MDNS_TXT, (uint32_t)i);
    if (i >= MDNS_SERVICES_MAX || !svc[i].used) { return; }
    for (int t = 0; t < svc[i].n_txt; t++) {
        const char *item = svc[i].txt[t];
        size_t n = strlen(item);
        if (n == 0 || n > MDNS_TXT_LEN) { continue; }
        mdns_resp_add_service_txtitem(service, item, (u8_t)n);
    }
}

static int find_netif(struct netif *netif) {
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (bound[i] == netif) { return i; }
    }
    return -1;
}

static const char *svc_instance(const service_t *s) {
    return s->track_hostname ? hostname : s->instance;
}

/* Register one service on one interface. */
static void bind_service(int si, int ni) {
    if (!svc[si].used || !bound[ni]) { return; }
    s8_t h = mdns_resp_add_service(bound[ni], svc_instance(&svc[si]), svc[si].type,
                                   svc[si].udp ? DNSSD_PROTO_UDP : DNSSD_PROTO_TCP,
                                   svc[si].port, srv_txt, (void *)(uintptr_t)si);
    svc[si].slot[ni] = h;
    if (h < 0) {
        printf("mdns: add %s on netif %d failed: %d\n", svc[si].type, ni, (int)h);
    }
}

static void unbind_service(int si, int ni) {
    if (svc[si].slot[ni] >= 0 && bound[ni]) {
        crumb_d(CRUMB_MDNS_SVC_DEL, (uint32_t)(si << 8 | ni));
        mdns_resp_del_service(bound[ni], (u8_t)svc[si].slot[ni]);
    }
    svc[si].slot[ni] = -1;
}

bool mdns_set_hostname(const char *name) {
    if (!name || !name[0] || strlen(name) > 63 || name[0] == '-') { return false; }
    char tmp[64];
    size_t n = 0;
    for (const char *p = name; *p && n + 1 < sizeof(tmp); p++) {
        char c = *p;
        if (isupper((unsigned char)c)) { c = (char)tolower((unsigned char)c); }
        /* A DNS label is letters, digits and hyphens. Underscores are legal
         * in a SERVICE type ("_http") and not in a hostname, which is why
         * the project's own name has to be spelled with hyphens here. */
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '-')) {
            return false;
        }
        tmp[n++] = c;
    }
    tmp[n] = 0;
    if (!tmp[0] || tmp[0] == '-') { return false; }
    if (strcmp(hostname, tmp) == 0) { return true; }

    /* lwIP's rename restarts probing and does not send a goodbye, so peers
     * would keep the old name until its TTL. Drop our own row now: a PTR
     * creates it with an empty address and DROP_MS would keep it 15 s. */
    mdns_peers_drop_name(hostname);
    send_hostname_goodbye(hostname);
    snprintf(hostname, sizeof(hostname), "%s", tmp);
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (!bound[i]) { continue; }
        netif_set_hostname(bound[i], hostname);
        mdns_resp_rename_netif(bound[i], hostname);
        for (int s = 0; s < MDNS_SERVICES_MAX; s++) {
            if (svc[s].used && svc[s].track_hostname && svc[s].slot[i] >= 0) {
                mdns_resp_rename_service(bound[i], (u8_t)svc[s].slot[i], hostname);
            }
        }
    }
    printf("mdns: hostname %s.local\n", hostname);
    return true;
}

const char *mdns_hostname(void) { return hostname; }

void mdns_hostname_remember(void) {
    snprintf(host_pending, sizeof host_pending, "%s", hostname);
    host_dirty = true;
}

static bool json_name(const char *json, char *out, size_t cap) {
    const char *p = strstr(json, "\"name\"");
    if (!p) { return false; }
    p += 6;
    while (*p && *p != ':') { p++; }
    if (*p != ':') { return false; }
    p++;
    while (*p == ' ' || *p == '\t') { p++; }
    if (*p != '"') { return false; }
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\') { return false; }
        out[n++] = *p++;
    }
    if (*p != '"') { return false; }
    out[n] = 0;
    return n > 0;
}

static void load_hostname(void) {
    char buf[128], name[64];
    if (!cfg_file_read(MDNS_NAME_PATH, buf, sizeof buf)) { return; }
    if (!json_name(buf, name, sizeof name)) { return; }
    if (mdns_set_hostname(name)) {
        printf("mdns: hostname %s from %s\n", hostname, MDNS_NAME_PATH);
    }
}

static int find_parked(struct netif *netif) {
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (parked[i] == netif) { return i; }
    }
    return -1;
}

static void park_put(struct netif *netif) {
    if (!netif || find_parked(netif) >= 0) { return; }
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (!parked[i]) { parked[i] = netif; return; }
    }
}

/* Ethernet (one 802.1Q tag allowed), IPv4, UDP 5353 or 224.0.0.251. */
static bool frame_is_mdns(const uint8_t *f, size_t len) {
    if (!f || len < 14) { return false; }
    size_t off = 12;
    uint16_t et = (uint16_t)((f[off] << 8) | f[off + 1]);
    if (et == 0x8100) {
        if (len < 18) { return false; }
        off = 16;
        et = (uint16_t)((f[off] << 8) | f[off + 1]);
    }
    if (et != 0x0800) { return false; }
    off += 2;
    if (len < off + 20) { return false; }
    uint8_t vihl = f[off];
    if ((vihl >> 4) != 4) { return false; }
    unsigned ihl = (unsigned)(vihl & 0x0f) * 4u;
    if (ihl < 20 || len < off + ihl + 8) { return false; }
    if (f[off + 9] != 17) { return false; }
    bool mcast = f[off + 16] == 224 && f[off + 17] == 0 && f[off + 18] == 0 && f[off + 19] == 251;
    size_t udp = off + ihl;
    uint16_t dport = (uint16_t)((f[udp + 2] << 8) | f[udp + 3]);
    return mcast || dport == 5353;
}

bool mdns_should_drop(const uint8_t *frame, size_t len) {
    if (responder_on || !frame_is_mdns(frame, len)) { return false; }
    mdns_drops++;
    return true;
}

bool mdns_responder_on(void) { return responder_on; }

void mdns_responder_set(bool on) {
    if (responder_on == on) { return; }
    responder_on = on;
    net_log("mdns responder %s", on ? "on" : "off");
}

static void mdns_drop_log(void) {
    if (mdns_drops == mdns_drops_logged) { return; }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (mdns_drop_log_ms && (uint32_t)(now - mdns_drop_log_ms) < 1000) { return; }
    net_log("mdns drop %lu", (unsigned long)mdns_drops);
    mdns_drops_logged = mdns_drops;
    mdns_drop_log_ms = now;
}

static void responder_apply(void);

void mdns_poll(void) {
    responder_apply();
    mdns_drop_log();
    if (!host_dirty) { return; }
    char body[96];
    snprintf(body, sizeof body, "{\"name\":\"%s\"}", host_pending);
    if (!cfg_file_write(MDNS_NAME_PATH, body)) { return; }
    host_dirty = false;
    printf("mdns: hostname %s stored\n", host_pending);
}

bool mdns_service_add(const char *instance, const char *type, bool udp,
                      uint16_t port, const char *const *txt, int n_txt) {
    return mdns_service_add_owned(-1, instance, type, udp, port, txt, n_txt);
}

bool mdns_service_add_owned(int owner, const char *instance, const char *type, bool udp,
                            uint16_t port, const char *const *txt, int n_txt) {
    if (!type || type[0] != '_' || !port) { return false; }
    if (n_txt < 0) { n_txt = 0; }
    if (n_txt > MDNS_TXT_MAX) { n_txt = MDNS_TXT_MAX; }

    /* Replace an existing (type, proto) rather than advertising it twice --
     * two SRV records for one service is a configuration error that resolves
     * intermittently, which is worse than failing. */
    int si = -1;
    for (int i = 0; i < MDNS_SERVICES_MAX; i++) {
        if (svc[i].used && svc[i].udp == udp && strcmp(svc[i].type, type) == 0) {
            si = i;
            for (int n = 0; n < MDNS_NETIFS; n++) { unbind_service(i, n); }
            break;
        }
    }
    if (si < 0) {
        for (int i = 0; i < MDNS_SERVICES_MAX; i++) {
            if (!svc[i].used) { si = i; break; }
        }
    }
    if (si < 0) { return false; }

    memset(&svc[si], 0, sizeof svc[si]);
    for (int n = 0; n < MDNS_NETIFS; n++) { svc[si].slot[n] = -1; }
    svc[si].used = true;
    svc[si].owner = owner;
    svc[si].udp = udp;
    svc[si].port = port;
    snprintf(svc[si].type, sizeof svc[si].type, "%s", type);
    if (instance && instance[0]) {
        snprintf(svc[si].instance, sizeof svc[si].instance, "%s", instance);
    } else {
        svc[si].track_hostname = true;
    }
    for (int t = 0; t < n_txt; t++) {
        if (!txt[t]) { continue; }
        snprintf(svc[si].txt[svc[si].n_txt], MDNS_TXT_LEN + 1, "%s", txt[t]);
        svc[si].n_txt++;
    }
    crumb_d(CRUMB_MDNS_SVC_ADD, (uint32_t)si);
    for (int n = 0; n < MDNS_NETIFS; n++) { bind_service(si, n); }
    crumb_d(CRUMB_MDNS_SVC_BOUND, (uint32_t)si);
    mdns_announce_all();
    crumb_d(CRUMB_MDNS_SVC_DONE, (uint32_t)si);
    return true;
}

bool mdns_service_remove(const char *type, bool udp) {
    if (!type) { return false; }
    for (int i = 0; i < MDNS_SERVICES_MAX; i++) {
        if (!svc[i].used || svc[i].udp != udp || strcmp(svc[i].type, type) != 0) {
            continue;
        }
        for (int n = 0; n < MDNS_NETIFS; n++) { unbind_service(i, n); }
        memset(&svc[i], 0, sizeof svc[i]);
        mdns_announce_all();
        return true;
    }
    return false;
}

void mdns_services_clear(void) {
    for (int i = 0; i < MDNS_SERVICES_MAX; i++) {
        if (!svc[i].used) { continue; }
        for (int n = 0; n < MDNS_NETIFS; n++) { unbind_service(i, n); }
        memset(&svc[i], 0, sizeof svc[i]);
    }
    mdns_announce_all();
}

void mdns_announce_all(void) {
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (bound[i]) { mdns_resp_announce(bound[i]); }
    }
}

struct netif *mdns_dns_netif(void) {
    struct netif *any = NULL;
    for (int i = 0; i < MDNS_NETIFS; i++) {
        struct netif *n = bound[i];
        if (!n || !netif_is_up(n) || !netif_is_link_up(n)) { continue; }
        if (ip4_addr_isany_val(*netif_ip4_addr(n))) { continue; }
        if (!any) { any = n; }
        /* 'us' is USB-NCM, the management link. A .local name is on a LAN. */
        if (!(n->name[0] == 'u' && n->name[1] == 's')) { return n; }
    }
    return any;
}

void mdns_add_netif(struct netif *netif) {
    if (!netif || find_netif(netif) >= 0) { return; }
    if (!responder_on) { park_put(netif); return; }
    int p = find_parked(netif);
    if (p >= 0) { parked[p] = NULL; }
    int ni = -1;
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (!bound[i]) { ni = i; break; }
    }
    if (ni < 0) { printf("mdns: no netif slot\n"); return; }

    err_t err = mdns_resp_add_netif(netif, hostname);
    if (err != ERR_OK) {
        printf("mdns: add netif failed: %d\n", (int)err);
        return;
    }
    netif_set_hostname(netif, hostname);
    bound[ni] = netif;
    for (int s = 0; s < MDNS_SERVICES_MAX; s++) { bind_service(s, ni); }
}

void mdns_remove_netif(struct netif *netif) {
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (parked[i] == netif) { parked[i] = NULL; }
    }
    int i = find_netif(netif);
    if (i < 0) { return; }
    for (int s = 0; s < MDNS_SERVICES_MAX; s++) { svc[s].slot[i] = -1; }
    mdns_resp_remove_netif(netif);
    bound[i] = NULL;
}

static void responder_apply(void) {
    if (!responder_on) {
        for (int i = 0; i < MDNS_NETIFS; i++) {
            struct netif *n = bound[i];
            if (!n) { continue; }
            mdns_remove_netif(n);
            park_put(n);
        }
        return;
    }
    for (int i = 0; i < MDNS_NETIFS; i++) {
        struct netif *n = parked[i];
        if (!n) { continue; }
        parked[i] = NULL;
        mdns_add_netif(n);
    }
}

static int bound_count(void) {
    int c = 0;
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (bound[i]) { c++; }
    }
    return c;
}

size_t mdns_status_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap,
                                "{\"hostname\":\"%s\",\"fqdn\":\"%s.local\","
                                "\"responder\":%s,\"netifs\":%d,\"services\":[",
                                hostname, hostname, responder_on ? "true" : "false", bound_count());
    bool first = true;
    for (int i = 0; i < MDNS_SERVICES_MAX && n + 96 < cap; i++) {
        if (!svc[i].used) { continue; }
        n += (size_t)snprintf(dst + n, cap - n,
                              "%s{\"instance\":\"%s\",\"type\":\"%s\",\"proto\":\"%s\","
                              "\"port\":%u,\"owner\":%d,\"txt\":[",
                              first ? "" : ",", svc_instance(&svc[i]), svc[i].type,
                              svc[i].udp ? "udp" : "tcp", (unsigned)svc[i].port, svc[i].owner);
        first = false;
        for (int t = 0; t < svc[i].n_txt && n + 80 < cap; t++) {
            n += (size_t)snprintf(dst + n, cap - n, "%s\"%s\"", t ? "," : "", svc[i].txt[t]);
        }
        n += (size_t)snprintf(dst + n, cap - n, "]}");
    }
    n += (size_t)snprintf(dst + n, cap - n, "]}");
    return n;
}

/* The device's own web UI, tracking the hostname. A default rather than a
 * fixture: Lua may replace or remove it -- and when the script that replaced
 * it stops, it comes back (mdns_release_owner). */
static void add_default_http(void) {
    static const char *const http_txt[] = { "path=/" };
    mdns_service_add(NULL, "_http", false, 80, http_txt, 1);
    /* So a board that does serve the web app can be found the same way it
     * finds a board that only has the websocket. assets=1 means this one
     * has the pages. */
    static const char *const btgw_txt[] = {
        "board=pico2w", "ws=/ws", "api=/api", "assets=1"
    };
    mdns_service_add(NULL, "_btgw", false, 80, btgw_txt, 4);
}

/* --- record groups (src/net/mdns_records.c) -------------------------------
 * Answered from the hook in third_party/lwip-mdns/mdns.c, announced twice on
 * add (RFC 6762 8.3), withdrawn with a TTL-0 goodbye on remove (10.1). */

static const ip_addr_t mdns_v4group = IPADDR4_INIT_BYTES(224, 0, 0, 251);
static uint8_t rec_in[256], rec_out[512];

static void send_pkt(struct netif *netif, const uint8_t *data, size_t len,
                     const ip_addr_t *dst, u16_t port) {
    struct udp_pcb *pcb = get_mdns_pcb();
    if (!pcb || !len) { return; }
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) { return; }
    memcpy(p->payload, data, len);
    udp_sendto_if(pcb, p, dst, port, netif);
    pbuf_free(p);
}

static void send_all(const uint8_t *data, size_t len) {
    for (int i = 0; i < MDNS_NETIFS; i++) {
        if (bound[i] && !ip4_addr_isany_val(*netif_ip4_addr(bound[i]))) {
            send_pkt(bound[i], data, len, &mdns_v4group, 5353);
        }
    }
}

static int put_u16(uint8_t *b, int cap, int o, unsigned v) {
    if (o < 0 || o + 2 > cap) { return -1; }
    b[o] = (uint8_t)(v >> 8);
    b[o + 1] = (uint8_t)v;
    return o + 2;
}

static int put_u32(uint8_t *b, int cap, int o, uint32_t v) {
    o = put_u16(b, cap, o, (unsigned)(v >> 16));
    return put_u16(b, cap, o, (unsigned)v);
}

static int put_name(uint8_t *b, int cap, int o, const char *name) {
    if (o < 0 || !name || !name[0]) { return -1; }
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        int n = dot ? (int)(dot - s) : (int)strlen(s);
        if (n <= 0 || n > 63 || o + 1 + n >= cap) { return -1; }
        b[o++] = (uint8_t)n;
        memcpy(b + o, s, (size_t)n);
        o += n;
        if (!dot || !dot[1]) { break; }
        s = dot + 1;
    }
    if (o >= cap) { return -1; }
    b[o++] = 0;
    return o;
}

/* TTL 0 for the old hostname's A and for each service instance that was
 * following it. lwIP's mdns_resp_rename_netif does not send this. */
static void send_hostname_goodbye(const char *old) {
    if (!old || !old[0]) { return; }
    char host[80], inst[96], svcname[48];
    snprintf(host, sizeof host, "%s.local", old);
    for (int i = 0; i < MDNS_NETIFS; i++) {
        struct netif *nif = bound[i];
        if (!nif || ip4_addr_isany_val(*netif_ip4_addr(nif))) { continue; }
        uint8_t pkt[384];
        memset(pkt, 0, 12);
        pkt[2] = 0x84;
        int answers = 0;
        int o = 12;
        for (int s = 0; s < MDNS_SERVICES_MAX; s++) {
            if (!svc[s].used || !svc[s].track_hostname) { continue; }
            /* type is already "_btgw" / "_http". */
            snprintf(svcname, sizeof svcname, "%s._%s.local", svc[s].type,
                     svc[s].udp ? "udp" : "tcp");
            snprintf(inst, sizeof inst, "%s.%s", old, svcname);
            int at = o;
            o = put_name(pkt, (int)sizeof pkt, o, svcname);
            o = put_u16(pkt, (int)sizeof pkt, o, 12);
            o = put_u16(pkt, (int)sizeof pkt, o, 1);
            o = put_u32(pkt, (int)sizeof pkt, o, 0);
            int rd = o;
            o = put_u16(pkt, (int)sizeof pkt, o, 0);
            int rs = o;
            o = put_name(pkt, (int)sizeof pkt, o, inst);
            if (o < 0) { o = at; continue; }
            put_u16(pkt, (int)sizeof pkt, rd, (unsigned)(o - rs));
            answers++;
        }
        int at = o;
        o = put_name(pkt, (int)sizeof pkt, o, host);
        o = put_u16(pkt, (int)sizeof pkt, o, 1);
        o = put_u16(pkt, (int)sizeof pkt, o, 1);
        o = put_u32(pkt, (int)sizeof pkt, o, 0);
        o = put_u16(pkt, (int)sizeof pkt, o, 4);
        if (o < 0 || o + 4 > (int)sizeof pkt) { o = at; }
        else {
            const ip4_addr_t *ip = netif_ip4_addr(nif);
            memcpy(pkt + o, &ip->addr, 4);
            o += 4;
            answers++;
        }
        if (answers <= 0 || o < 12) { continue; }
        pkt[6] = (uint8_t)(answers >> 8);
        pkt[7] = (uint8_t)answers;
        send_pkt(nif, pkt, (size_t)o, &mdns_v4group, 5353);
    }
}

void btgw_mdns_query_hook(struct netif *netif, struct pbuf *p, const ip_addr_t *addr,
                          u16_t port, int recv_unicast) {
    if (!mrec_group_count() || find_netif(netif) < 0) { return; }
    crumb_d(CRUMB_MDNS_HOOK, (uint32_t)p->tot_len);
    u16_t n = pbuf_copy_partial(p, rec_in, sizeof rec_in, 0);
    bool legacy = port != 5353, unicast = false;
    size_t len = mrec_build_answer(rec_in, n, legacy, rec_out, sizeof rec_out, &unicast);
    if (!len) { return; }
    crumb(CRUMB_MDNS_HOOK_BUILT);
    if (legacy || unicast || recv_unicast) {
        send_pkt(netif, rec_out, len, addr, port);
    } else {
        send_pkt(netif, rec_out, len, &mdns_v4group, 5353);
    }
}

static void announce_again(void *arg) {
    crumb_d(CRUMB_MDNS_ANNOUNCE, (uint32_t)(intptr_t)arg);
    int id = (int)(intptr_t)arg;
    size_t len = mrec_build_announce(id, MREC_TTL_DEFAULT, rec_out, sizeof rec_out);
    if (len) { send_all(rec_out, len); }
}

int mdns_group_add(int owner, const mrec_t *recs, int n, const char **why) {
    int id = mrec_group_add(owner, recs, n, why);
    if (id < 0) { return -1; }
    announce_again((void *)(intptr_t)id);
    sys_timeout(1000, announce_again, (void *)(intptr_t)id);
    return id;
}

bool mdns_group_remove(int id) {
    if (!mrec_group_exists(id)) { return false; }
    size_t len = mrec_build_announce(id, 0, rec_out, sizeof rec_out);
    if (len) { send_all(rec_out, len); }
    sys_untimeout(announce_again, (void *)(intptr_t)id);
    return mrec_group_remove(id);
}

size_t mdns_groups_json(char *dst, size_t cap) { return mrec_groups_json(dst, cap); }

/* A script has stopped: everything it declared goes with it -- its groups
 * (with a goodbye) and its services. The services used to stay advertised
 * for the life of the boot, pointing at whatever the script had been
 * serving. A replaced default _http comes back. */
int mdns_release_owner(int owner) {
    if (owner < 0) { return 0; }
    int n = 0, ids[MREC_GROUPS];
    int k = mrec_groups_of(owner, ids, MREC_GROUPS);
    for (int i = 0; i < k; i++) { n += mdns_group_remove(ids[i]); }
    bool removed_http = false;
    for (int i = 0; i < MDNS_SERVICES_MAX; i++) {
        if (!svc[i].used || svc[i].owner != owner) { continue; }
        if (!svc[i].udp && strcmp(svc[i].type, "_http") == 0) { removed_http = true; }
        for (int b = 0; b < MDNS_NETIFS; b++) { unbind_service(i, b); }
        memset(&svc[i], 0, sizeof svc[i]);
        n++;
    }
    if (removed_http) { add_default_http(); }
    else if (n) { mdns_announce_all(); }
    return n;
}

/* The Lua state is going (VM restart): every script's declarations go. */
int mdns_release_all_owned(void) {
    int n = 0;
    for (int owner = 0; owner < 32; owner++) { n += mdns_release_owner(owner); }
    return n;
}

/* --- ops: read-only views for the UI and the CLI ----------------------------- */

/* The shared scratch buffer (api.h): filled and handed off within the call. */
#define mops_buf      api_scratch()
#define mops_buf_size api_scratch_size()

static void op_mdns_status(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    mdns_status_json(mops_buf, mops_buf_size);
    api_reply_ok(reply, ctx, id, mops_buf);
}

static void op_mdns_groups(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char *b = mops_buf;
    size_t cap = mops_buf_size;
    size_t n = (size_t)snprintf(b, cap, "{\"groups\":");
    n += mrec_groups_json(b + n, cap - n);
    snprintf(b + n, cap - n, "}");
    api_reply_ok(reply, ctx, id, mops_buf);
}

static void op_mdns_peers(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    mdns_peers_json(mops_buf, mops_buf_size);
    api_reply_ok(reply, ctx, id, mops_buf);
}

static void op_mdns_browse(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    mdns_browse_kick();
    mdns_peers_json(mops_buf, mops_buf_size);
    api_reply_ok(reply, ctx, id, mops_buf);
}

static void op_mdns_responder(const char *params, api_reply_fn reply, void *ctx, long id) {
    bool en = mdns_responder_on();
    if (api_json_bool(params, "enabled", &en)) { mdns_responder_set(en); }
    char buf[40];
    snprintf(buf, sizeof buf, "{\"responder\":%s}", mdns_responder_on() ? "true" : "false");
    api_reply_ok(reply, ctx, id, buf);
}

static const op_def_t mdns_ops[] = {
    { "mdns.status", op_mdns_status },
    { "mdns.groups", op_mdns_groups },
    { "mdns.peers", op_mdns_peers },
    { "mdns.browse", op_mdns_browse },
    { "mdns.responder", op_mdns_responder },
    { NULL, NULL }
};

const op_module_t op_module_mdns = {
    .ns = "mdns", .ops = mdns_ops, .topics = NULL, .init = NULL
};

void mdns_start(void) {
    for (int i = 0; i < MDNS_NETIFS; i++) { bound[i] = NULL; }
    for (int s = 0; s < MDNS_SERVICES_MAX; s++) {
        memset(&svc[s], 0, sizeof svc[s]);
        for (int n = 0; n < MDNS_NETIFS; n++) { svc[s].slot[n] = -1; }
    }
    mdns_resp_init();
    mrec_reset();
    load_hostname();
    add_default_http();
}

#endif /* CONFIG_NET_MDNS */
