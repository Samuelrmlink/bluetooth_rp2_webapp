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

#include "lwip/apps/mdns.h"
#include "lwip/apps/mdns_priv.h"   /* get_mdns_pcb() */
#include "lwip/netif.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"

#include "net/mdns_records.h"
#include "api/ops.h"
#include "api/api.h"

#define MDNS_NETIFS 4

static char hostname[64] = BTGW_MDNS_HOSTNAME;
static struct netif *bound[MDNS_NETIFS];

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
    for (int n = 0; n < MDNS_NETIFS; n++) { bind_service(si, n); }
    mdns_announce_all();
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

void mdns_add_netif(struct netif *netif) {
    if (!netif || find_netif(netif) >= 0) { return; }
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
    int i = find_netif(netif);
    if (i < 0) { return; }
    for (int s = 0; s < MDNS_SERVICES_MAX; s++) { svc[s].slot[i] = -1; }
    mdns_resp_remove_netif(netif);
    bound[i] = NULL;
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
                                "\"netifs\":%d,\"services\":[",
                                hostname, hostname, bound_count());
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

void btgw_mdns_query_hook(struct netif *netif, struct pbuf *p, const ip_addr_t *addr,
                          u16_t port, int recv_unicast) {
    if (!mrec_group_count() || find_netif(netif) < 0) { return; }
    u16_t n = pbuf_copy_partial(p, rec_in, sizeof rec_in, 0);
    bool legacy = port != 5353, unicast = false;
    size_t len = mrec_build_answer(rec_in, n, legacy, rec_out, sizeof rec_out, &unicast);
    if (!len) { return; }
    if (legacy || unicast || recv_unicast) {
        send_pkt(netif, rec_out, len, addr, port);
    } else {
        send_pkt(netif, rec_out, len, &mdns_v4group, 5353);
    }
}

static void announce_again(void *arg) {
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

static const op_def_t mdns_ops[] = {
    { "mdns.status", op_mdns_status },
    { "mdns.groups", op_mdns_groups },
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
    add_default_http();
}

#endif /* CONFIG_NET_MDNS */
