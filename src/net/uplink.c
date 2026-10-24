/*
 * Uplink selection: measure every interface that has a gateway, let
 * uplink_core.c decide, apply the decision. See uplink.h.
 *
 * Probing: ICMP echo through one raw pcb, sent with raw_sendto_if_src() on
 * the candidate interface with its own address as the source, so the next
 * hop is that interface's gateway whatever lwIP's default route says. The
 * replies come back to the same pcb; anything that is not one of ours is
 * passed on to lwIP untouched.
 *
 * Fast suspicion: if the tunnel has sent data and received nothing
 * authenticated for 1.5 s while the active uplink looked healthy, the
 * active one is probed at once (3 pings, 300 ms apart) instead of waiting
 * out the 2 s cadence -- or the 25 s keepalive, which is how long WireGuard
 * alone would take to notice.
 */
#include "net/uplink.h"

#if CONFIG_NET_UPLINK

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "lwip/netif.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/sys.h"

#include "net/uplink_core.h"
#include "net/wg.h"
#include "api/api.h"
#include "api/ops.h"
#include "config/config.h"
#if CONFIG_NET_WIFI
#include "net/wifi.h"
#endif
#include "net/eth_w5500.h"
#if CONFIG_BT_CLASSIC_NAP
#include "bt/classic/nap.h"
#endif

#define UPLINK_CFG_PATH   "cfg/uplink.json"
#define PROBE_EVERY_MS    2000
#define PROBE_TIMEOUT_MS  1000
#define BURST_GAP_MS      300
#define BURST_N           3
#define SUSPECT_MS        1500
#define SUSPECT_HOLDOFF_MS 5000
#define PING_ID_BASE      0xB7E0

typedef struct {
    const char    *name;          /* the netif's own name: "w0", "e0" */
    const char    *kind;
    struct netif *(*get)(void);
    upl_cand_t     m;
    struct netif  *nif;
    uint16_t       seq;
    bool           outstanding;
    uint32_t       sent_ms, next_ms;
    uint8_t        burst;
    uint32_t       sent, replies;
} cand_t;

static cand_t cands[] = {
#if CONFIG_NET_WIFI
    { .name = "w0", .kind = "wifi", .get = wifi_uplink_netif },
#endif
#if CONFIG_NET_ETH_W5500
    { .name = "e0", .kind = "ethernet", .get = eth_uplink_netif },
#endif
#if CONFIG_BT_CLASSIC_NAP
    { .name = "b0", .kind = "nap", .get = nap_uplink_netif },
#endif
};
#define N_CANDS ((int)(sizeof cands / sizeof cands[0]))

static upl_policy_t policy = UPL_BEST;
static char fixed_name[4];
static ip4_addr_t probe_cfg;          /* any = use the WireGuard endpoint */
static bool cfg_dirty;

static int active = -1;
static uint32_t switches, last_switch_ms, last_suspect_ms;
static const char *last_reason = "";
static char last_from[3], last_to[3];
static struct raw_pcb *ping_pcb;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static bool probe_target(ip4_addr_t *t) {
    if (!ip4_addr_isany_val(probe_cfg)) { *t = probe_cfg; return true; }
    return wg_endpoint(t);
}

static int fixed_index(void) {
    for (int i = 0; i < N_CANDS; i++) {
        if (strcmp(cands[i].name, fixed_name) == 0) { return i; }
    }
    return -1;
}

static int find(const char *name) {
    for (int i = 0; i < N_CANDS; i++) {
        if (strcmp(cands[i].name, name) == 0) { return i; }
    }
    return -1;
}

/* --- probing ------------------------------------------------------------- */

static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    (void)arg; (void)pcb;
    struct ip_hdr iph;
    if (pbuf_copy_partial(p, &iph, sizeof iph, 0) != sizeof iph) { return 0; }
    u16_t hl = (u16_t)(IPH_HL_BYTES(&iph));
    struct icmp_echo_hdr e;
    if (pbuf_copy_partial(p, &e, sizeof e, hl) != sizeof e) { return 0; }
    if (ICMPH_TYPE(&e) != ICMP_ER) { return 0; }
    u16_t id = lwip_ntohs(e.id);
    if (id < PING_ID_BASE || id >= PING_ID_BASE + N_CANDS) { return 0; }
    cand_t *c = &cands[id - PING_ID_BASE];
    ip4_addr_t t;
    if (!c->outstanding || lwip_ntohs(e.seqno) != c->seq ||
        !probe_target(&t) || !ip4_addr_eq(ip_2_ip4(addr), &t)) {
        pbuf_free(p);          /* ours, but late or stale: eat it */
        return 1;
    }
    c->outstanding = false;
    c->replies++;
    upl_sample(&c->m, (int32_t)(now_ms() - c->sent_ms));
    pbuf_free(p);
    return 1;
}

static void send_ping(int i, const ip4_addr_t *target, uint32_t now) {
    cand_t *c = &cands[i];
    const u16_t len = sizeof(struct icmp_echo_hdr) + 8;
    struct pbuf *p = pbuf_alloc(PBUF_IP, len, PBUF_RAM);
    if (!p) { return; }
    struct icmp_echo_hdr *e = (struct icmp_echo_hdr *)p->payload;
    ICMPH_TYPE_SET(e, ICMP_ECHO);
    ICMPH_CODE_SET(e, 0);
    e->id = lwip_htons((u16_t)(PING_ID_BASE + i));
    e->seqno = lwip_htons(++c->seq);
    memset((uint8_t *)e + sizeof *e, 0x5a, 8);
    e->chksum = 0;
    e->chksum = inet_chksum(e, len);
    ip_addr_t dst, src;
    ip_addr_copy_from_ip4(dst, *target);
    ip_addr_copy_from_ip4(src, *netif_ip4_addr(c->nif));
    if (raw_sendto_if_src(ping_pcb, p, &dst, c->nif, &src) == ERR_OK) {
        c->outstanding = true;
        c->sent_ms = now;
        c->sent++;
    }
    pbuf_free(p);
}

/* --- applying ------------------------------------------------------------ */

static struct netif *fallback(void) {
#if CONFIG_NET_WIFI
    return wifi_fallback_netif();
#else
    return NULL;
#endif
}

static void emit(void) {
    char b[96];
    snprintf(b, sizeof b, "{\"active\":\"%s\",\"reason\":\"%s\",\"switches\":%lu}",
             active >= 0 ? cands[active].name : "", last_reason, (unsigned long)switches);
    api_emit("uplink.state", b);
}

static void apply_default(void) {
    struct netif *t = active >= 0 ? cands[active].nif : fallback();
    if (t && netif_default != t) {
        netif_set_default(t);
    }
}

static void switch_to(int i, const char *why) {
    int was = active;
    active = i;
    snprintf(last_from, sizeof last_from, "%s", was >= 0 ? cands[was].name : "");
    snprintf(last_to, sizeof last_to, "%s", i >= 0 ? cands[i].name : "");
    last_reason = why && why[0] ? why : "manual";
    switches++;
    last_switch_ms = now_ms();
    apply_default();
    /* Make-before-break: the old interface is left exactly as it was, so
     * whatever is in flight to it still arrives; only egress moves. */
    wg_set_uplink(i >= 0 ? cands[i].nif : NULL);
    printf("uplink: %s -> %s (%s)\n", last_from[0] ? last_from : "none",
           last_to[0] ? last_to : "none", last_reason);
    emit();
}

/* Gateways and probe targets as they are now. True when anything changed. */
static bool refresh(void) {
    bool changed = false;
    ip4_addr_t t;
    bool probing = probe_target(&t);
    for (int i = 0; i < N_CANDS; i++) {
        cand_t *c = &cands[i];
        struct netif *n = c->get();
        bool gw = n && netif_is_up(n) && netif_is_link_up(n) &&
                  !ip4_addr_isany_val(*netif_ip4_addr(n)) && !ip4_addr_isany_val(*netif_ip4_gw(n));
        if (gw != c->m.has_gw || n != c->nif) {
            c->nif = n;
            c->m.has_gw = gw;
            upl_reset(&c->m);
            c->outstanding = false;
            c->next_ms = 0;
            changed = true;
        }
        if (probing != c->m.probing) {
            c->m.probing = probing;
            upl_reset(&c->m);
            changed = true;
        }
    }
    return changed;
}

static void decide(void) {
    const char *why = "";
    upl_cand_t m[UPL_MAX];
    for (int i = 0; i < N_CANDS; i++) { m[i] = cands[i].m; }
    int pick = upl_choose(policy, fixed_index(), m, N_CANDS, active, now_ms(), &why);
    for (int i = 0; i < N_CANDS; i++) { cands[i].m.better_since = m[i].better_since; }
    if (pick != active) { switch_to(pick, why); }
}

void uplink_reselect(void) {
    refresh();
    decide();
    apply_default();     /* the cyw43 driver moves it when an interface comes up */
}

/* --- config ---------------------------------------------------------------- */

static bool parse_policy(const char *s, upl_policy_t *p, char fixed[4]) {
    if (strcmp(s, "best") == 0) { *p = UPL_BEST; fixed[0] = 0; return true; }
    if (strcmp(s, "failover") == 0) { *p = UPL_FAILOVER; fixed[0] = 0; return true; }
    if (strncmp(s, "fixed:", 6) == 0 && find(s + 6) >= 0) {
        *p = UPL_FIXED;
        snprintf(fixed, 4, "%s", s + 6);
        return true;
    }
    return false;
}

static const char *policy_str(void) {
    static char b[12];
    if (policy == UPL_FIXED) { snprintf(b, sizeof b, "fixed:%s", fixed_name); return b; }
    return policy == UPL_FAILOVER ? "failover" : "best";
}

bool uplink_configure(const char *pol, const char *probe_ip, const char **why) {
    upl_policy_t p = policy;
    char f[4];
    snprintf(f, sizeof f, "%s", fixed_name);
    ip4_addr_t ip = probe_cfg;
    if (pol && !parse_policy(pol, &p, f)) {
        *why = "policy is best, failover or fixed:<iface> (an uplink this build has)";
        return false;
    }
    if (probe_ip) {
        if (!probe_ip[0]) { ip4_addr_set_any(&ip); }
        else if (!ip4addr_aton(probe_ip, &ip)) { *why = "probe_ip is not an address"; return false; }
    }
    bool target_changed = !ip4_addr_eq(&ip, &probe_cfg);
    policy = p;
    memcpy(fixed_name, f, sizeof fixed_name);
    probe_cfg = ip;
    cfg_dirty = true;
    if (target_changed) {
        for (int i = 0; i < N_CANDS; i++) { upl_reset(&cands[i].m); cands[i].outstanding = false; }
    }
    return true;
}

bool uplink_force(const char *iface, const char **why) {
    int i = find(iface);
    if (i < 0) { *why = "no such uplink in this build"; return false; }
    if (!cands[i].m.has_gw) { *why = "that uplink has no gateway"; return false; }
    for (int k = 0; k < N_CANDS; k++) { cands[k].m.better_since = 0; }
    if (i != active) { switch_to(i, "manual"); }
    return true;
}

static void load_cfg(void) {
    char buf[160], s[24];
    if (!cfg_file_read(UPLINK_CFG_PATH, buf, sizeof buf)) { return; }
    if (api_json_str(buf, "policy", s, sizeof s) && !parse_policy(s, &policy, fixed_name)) {
        printf("uplink: policy \"%s\" not understood, using best\n", s);
    }
    if (api_json_str(buf, "probe_ip", s, sizeof s) && s[0]) { ip4addr_aton(s, &probe_cfg); }
}

static void save_cfg(void) {
    char b[96], ip[16] = "";
    if (!ip4_addr_isany_val(probe_cfg)) { ip4addr_ntoa_r(&probe_cfg, ip, sizeof ip); }
    snprintf(b, sizeof b, "{\"policy\":\"%s\",\"probe_ip\":\"%s\"}", policy_str(), ip);
    if (cfg_file_write(UPLINK_CFG_PATH, b)) { cfg_dirty = false; }
}

/* --- lifecycle ------------------------------------------------------------- */

void uplink_init(void) {
    load_cfg();
    ping_pcb = raw_new(IP_PROTO_ICMP);
    if (ping_pcb) {
        raw_recv(ping_pcb, ping_recv, NULL);
        raw_bind(ping_pcb, IP4_ADDR_ANY);
    }
    printf("uplink: %d candidate(s), policy %s\n", N_CANDS, policy_str());
    uplink_reselect();
    /* Interfaces may have come up (and been selected) before the tunnel
     * existed; hand the choice to it now. */
    wg_set_uplink(uplink_active());
}

bool uplink_ping_recent(void) {
    if (active < 0 || active >= N_CANDS) return false;
    const upl_cand_t *m = &cands[active].m;
    return m->answered && m->consec_loss == 0;
}

bool uplink_any_up(void) {
    for (int i = 0; i < N_CANDS; i++) {
        struct netif *n = cands[i].get();
        if (n && netif_is_up(n) && netif_is_link_up(n) &&
            !ip4_addr_isany_val(*netif_ip4_addr(n)) && !ip4_addr_isany_val(*netif_ip4_gw(n))) {
            return true;
        }
    }
    return false;
}

struct netif *uplink_active(void) { return active >= 0 ? cands[active].nif : NULL; }

void uplink_tick(void) {
    uint32_t now = now_ms();
    bool changed = refresh();

    ip4_addr_t target;
    bool probing = ping_pcb && probe_target(&target);

    /* Fast suspicion, on the active uplink only. */
    if (probing && active >= 0 && cands[active].m.has_gw) {
        uint32_t tx = wg_last_tx_ms(), rx = wg_last_rx_ms(), s = sys_now();
        if (tx && (int32_t)(tx - rx) > 0 && (uint32_t)(s - rx) > SUSPECT_MS &&
            (uint32_t)(now - last_suspect_ms) > SUSPECT_HOLDOFF_MS) {
            last_suspect_ms = now;
            cands[active].burst = BURST_N;
            cands[active].next_ms = now;
        }
    }

    for (int i = 0; probing && i < N_CANDS; i++) {
        cand_t *c = &cands[i];
        if (!c->m.has_gw) { continue; }
        if (c->outstanding && (uint32_t)(now - c->sent_ms) >= PROBE_TIMEOUT_MS) {
            c->outstanding = false;
            upl_sample(&c->m, -1);
            changed = true;
        }
        if (!c->outstanding && (int32_t)(now - c->next_ms) >= 0) {
            send_ping(i, &target, now);
            if (c->burst) { c->burst--; c->next_ms = now + BURST_GAP_MS; }
            else { c->next_ms = now + PROBE_EVERY_MS + (uint32_t)i * 97u; }   /* stagger */
        }
    }

    /* Decide on every new result, and once a second regardless (the
     * hysteresis timer runs on time, not on samples). */
    static uint32_t last_decide;
    if (changed || (uint32_t)(now - last_decide) >= 250) {
        last_decide = now;
        decide();
        if (changed) { apply_default(); }
    }
    if (cfg_dirty) { save_cfg(); }
}

/* --- source routing (LWIP_HOOK_IP4_ROUTE_SRC) ------------------------------ */

/*
 * A connection whose source address belongs to an uplink keeps leaving
 * through that uplink while it is up, whatever the default route has become
 * since -- so a switch does not break existing connections, only steers new
 * ones. Destinations on a directly attached subnet are left to ip4_route()
 * (return NULL), as is everything with no source yet.
 */
struct netif *uplink_route_src(const struct ip4_addr *src, const struct ip4_addr *dest) {
    if (!src || ip4_addr_isany(src)) { return NULL; }
    struct netif *n, *owner = NULL;
    NETIF_FOREACH(n) {
        if (!netif_is_up(n) || !netif_is_link_up(n) || ip4_addr_isany_val(*netif_ip4_addr(n))) { continue; }
        if (ip4_addr_net_eq(dest, netif_ip4_addr(n), netif_ip4_netmask(n))) { return NULL; }
        if (ip4_addr_eq(src, netif_ip4_addr(n))) { owner = n; }
    }
    if (owner && !ip4_addr_isany_val(*netif_ip4_gw(owner))) { return owner; }
    return NULL;
}

/* --- status + ops ------------------------------------------------------------ */

static const char *state_of(const cand_t *c) {
    return !c->m.has_gw ? "no_gateway" : !c->m.probing ? "unprobed"
         : upl_failed(&c->m) ? "failed" : upl_known(&c->m) ? "ok" : "measuring";
}

int uplink_count(void) { return N_CANDS; }
const char *uplink_policy_name(void) { return policy_str(); }

bool uplink_info(int i, uplink_info_t *o) {
    if (i < 0 || i >= N_CANDS) { return false; }
    const cand_t *c = &cands[i];
    memset(o, 0, sizeof *o);
    o->name = c->name;
    o->kind = c->kind;
    o->state = state_of(c);
    o->active = i == active;
    if (c->nif) {
        ip4addr_ntoa_r(netif_ip4_addr(c->nif), o->ip, sizeof o->ip);
        ip4addr_ntoa_r(netif_ip4_gw(c->nif), o->gw, sizeof o->gw);
    }
    bool k = upl_known(&c->m);
    int32_t m = upl_metric(&c->m);
    o->rtt_ms = k ? upl_rtt(&c->m) : -1;
    o->jitter_ms = k ? upl_jitter(&c->m) : -1;
    o->metric = m >= INT32_MAX - 1 ? -1 : m;
    o->loss_pct = upl_loss_pct(&c->m);
    return true;
}

size_t uplink_status_json(char *dst, size_t cap) {
    ip4_addr_t t;
    bool probing = probe_target(&t);
    char ts[16] = "";
    if (probing) { ip4addr_ntoa_r(&t, ts, sizeof ts); }
    size_t n = (size_t)snprintf(dst, cap,
        "{\"policy\":\"%s\",\"active\":\"%s\",\"probe_ip\":\"%s\",\"probe_source\":\"%s\","
        "\"switches\":%lu,\"last_switch_ms_ago\":%ld,\"last_switch\":{\"from\":\"%s\",\"to\":\"%s\",\"reason\":\"%s\"},"
        "\"uplinks\":[",
        policy_str(), active >= 0 ? cands[active].name : "", ts,
        !probing ? "none" : ip4_addr_isany_val(probe_cfg) ? "wireguard" : "configured",
        (unsigned long)switches, switches ? (long)(now_ms() - last_switch_ms) : -1L,
        last_from, last_to, last_reason);
    for (int i = 0; i < N_CANDS && n + 260 < cap; i++) {
        cand_t *c = &cands[i];
        char ip[16] = "", gw[16] = "";
        if (c->nif) {
            ip4addr_ntoa_r(netif_ip4_addr(c->nif), ip, sizeof ip);
            ip4addr_ntoa_r(netif_ip4_gw(c->nif), gw, sizeof gw);
        }
        const char *state = state_of(c);
        int32_t metric = upl_metric(&c->m);
        n += (size_t)snprintf(dst + n, cap - n,
            "%s{\"name\":\"%s\",\"kind\":\"%s\",\"ip\":\"%s\",\"gw\":\"%s\",\"state\":\"%s\","
            "\"active\":%s,\"rtt_ms\":%ld,\"jitter_ms\":%ld,\"loss_pct\":%d,\"metric\":%ld,"
            "\"samples\":%lu,\"sent\":%lu,\"replies\":%lu}",
            i ? "," : "", c->name, c->kind, ip, gw, state, i == active ? "true" : "false",
            upl_known(&c->m) ? (long)upl_rtt(&c->m) : -1L,
            upl_known(&c->m) ? (long)upl_jitter(&c->m) : -1L,
            upl_loss_pct(&c->m), metric >= INT32_MAX - 1 ? -1L : (long)metric,
            (unsigned long)c->m.samples, (unsigned long)c->sent, (unsigned long)c->replies);
    }
    n += (size_t)snprintf(dst + n, cap - n, "]}");
    return n;
}

/* The shared scratch buffer (api.h): filled and handed off within the call. */
#define obuf      api_scratch()
#define OBUF_SIZE api_scratch_size()

static void op_status(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    uplink_status_json(obuf, OBUF_SIZE);
    api_reply_ok(reply, ctx, id, obuf);
}

static void op_policy(const char *params, api_reply_fn reply, void *ctx, long id) {
    char pol[16], ip[20];
    bool hp = api_json_str(params, "policy", pol, sizeof pol);
    bool hi = api_json_str(params, "probe_ip", ip, sizeof ip);
    const char *why = "bad request";
    if (!hp && !hi) { api_reply_err(reply, ctx, id, "bad_request", "need \"policy\" and/or \"probe_ip\""); return; }
    if (!uplink_configure(hp ? pol : NULL, hi ? ip : NULL, &why)) {
        api_reply_err(reply, ctx, id, "bad_request", why);
        return;
    }
    uplink_status_json(obuf, OBUF_SIZE);
    api_reply_ok(reply, ctx, id, obuf);
}

static void op_switch(const char *params, api_reply_fn reply, void *ctx, long id) {
    char ifn[8];
    const char *why = "bad request";
    if (!api_json_str(params, "iface", ifn, sizeof ifn)) { api_reply_err(reply, ctx, id, "bad_request", "need \"iface\""); return; }
    if (!uplink_force(ifn, &why)) { api_reply_err(reply, ctx, id, "refused", why); return; }
    uplink_status_json(obuf, OBUF_SIZE);
    api_reply_ok(reply, ctx, id, obuf);
}

static const op_def_t uplink_ops[] = {
    { "uplink.status", op_status },
    { "uplink.policy", op_policy },
    { "uplink.switch", op_switch },
    { NULL, NULL }
};

static const char *const uplink_topics[] = { "uplink.state", NULL };

const op_module_t op_module_uplink = {
    .ns = "uplink", .ops = uplink_ops, .topics = uplink_topics, .init = NULL
};

#endif /* CONFIG_NET_UPLINK */
