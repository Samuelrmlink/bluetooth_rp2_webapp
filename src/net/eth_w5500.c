/*
 * lwIP glue for the W5500 (src/net/w5500.c is the chip).
 *
 * Frames never get a static buffer of their own:
 *
 *   RX  the chip state machine asks for buffers once it knows a frame's
 *       length; it gets a PBUF_POOL pbuf (one 1516-byte pool buffer holds any
 *       frame) and the DMA writes straight into it. Out of pool -> the frame
 *       is skipped in the ring and counted.
 *   TX  linkoutput cannot wait for the bus, so it CLONES the frame into one
 *       contiguous PBUF_RAM pbuf from lwIP's heap and queues the pointer; the
 *       state machine writes it out and frees it. A clone, not pbuf_ref():
 *       lwIP hands drivers PBUF_REF/ROM chains whose memory belongs to the
 *       caller, and a queued reference to those would be read after it had
 *       been reused. Bounded at ETH_TXQ frames, so at most ~9 KB of heap.
 *
 * Interrupts: the INT pin is sampled by the state machine as a level (a
 * frame waiting holds it low until Sn_IR is cleared), so no GPIO IRQ is
 * needed and nothing here ever runs outside the main loop.
 */
#include "net/eth_w5500.h"
#include "net/mdns.h"

#if CONFIG_NET_ETH_W5500

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/pbuf.h"
#include "lwip/igmp.h"
#include "lwip/autoip.h"
#include "lwip/acd.h"
#include "dhcpserver.h"
#include "netif/ethernet.h"

#include "io/spibus.h"
#include "net/w5500.h"
#include "net/mdns.h"
#include "net/uplink.h"
#include "api/api.h"
#include "api/ops.h"
#include "config/config.h"
#include "net/eth_cfg.h"
#include "net/gw_lan.h"
#include "lwip/prot/ethernet.h"

#define ETH_CFG_PATH "cfg/eth.json"
#define ETH_TXQ 6

static w5500_t chip;
static struct netif eth_if;
static bool present, netif_added;
#if CONFIG_NET_GW_LAN
static struct netif eth_ll;
static bool ll_added;
static bool ll_had_addr;
#endif

static struct pbuf *txq[ETH_TXQ];
static uint8_t tx_head, tx_count;
static uint32_t txq_full, tx_dropped, rx_nomem;
static struct pbuf *rx_p;

/* Config: what is saved, and what eth.config staged for the loop. */
static eth_cfg_t cfg, staged;
static bool staged_pending, cfg_dirty;

static bool had_addr;
static uint32_t ll_since;     /* link up, or lease lost: when DHCP was left alone */
static bool ll_active;        /* AutoIP started */
static bool had_lease;
static uint32_t last_kick;
static uint32_t ll_starts, leases;
static dhcp_server_t dsrv;
static bool srv_on;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

#if CONFIG_NET_GW_LAN
struct netif *eth_ll_netif(void) { return ll_added ? &eth_ll : NULL; }

/* ARP and ethernet multicast (bit 0 of the dest MAC, which covers
 * 224.0.0.251) go to both netifs. Unicast IPv4 stays on e0: ip4_input
 * walks the other netifs for an address it owns. */
static bool rx_share(const struct pbuf *p) {
    uint8_t hdr[14];
    if (pbuf_copy_partial(p, hdr, 14, 0) != 14) { return false; }
    if (hdr[0] & 0x01) { return true; }
    uint16_t et = (uint16_t)(((uint16_t)hdr[12] << 8) | hdr[13]);
    return et == ETHTYPE_ARP;
}

static void ll_note_addr(void) {
    if (!ll_added) { return; }
    bool has = !ip4_addr_isany_val(*netif_ip4_addr(&eth_ll)) && netif_is_link_up(&eth_ll);
    if (has == ll_had_addr) { return; }
    ll_had_addr = has;
    if (has) {
        printf("eth: link-local %s\n", ip4addr_ntoa(netif_ip4_addr(&eth_ll)));
        mdns_add_netif(&eth_ll);
    } else {
        mdns_remove_netif(&eth_ll);
    }
}

static void ll_companion_down(void) {
    if (!ll_added) { return; }
    autoip_stop(&eth_ll);
    /* autoip_stop() leaves ACD defending the old 169.254 address. */
    struct autoip *a = netif_autoip_data(&eth_ll);
    if (a) { acd_stop(&a->acd); }
    netif_set_link_down(&eth_ll);
    ll_note_addr();
}

static void ll_companion_up(void) {
    if (!ll_added) { return; }
    netif_set_link_up(&eth_ll);
    autoip_start(&eth_ll);
    ll_starts++;
    printf("eth: link-local companion up (kept beside any lease)\n");
}
#else
struct netif *eth_ll_netif(void) { return NULL; }
#endif

static void ll_stop(void) {
    if (!ll_active) { return; }
    ll_active = false;
    autoip_stop(&eth_if);           /* clears the address if it was link-local */
    /* lwIP 2.2.1's autoip_stop() leaves its ACD defending the old 169.254
     * address; stop that too, or a later conflict on it would knock a DHCP
     * lease off the interface. */
    struct autoip *a = netif_autoip_data(&eth_if);
    if (a) { acd_stop(&a->acd); }
}

/* --- chip callbacks (main loop) ------------------------------------------ */

static int rx_begin(void *ctx, uint16_t len, w5500_buf_t *b, int max) {
    (void)ctx;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (!p) { rx_nomem++; return 0; }
    int n = 0;
    for (struct pbuf *q = p; q; q = q->next) {
        if (n >= max) { pbuf_free(p); rx_nomem++; return 0; }
        b[n].p = (uint8_t *)q->payload;
        b[n].len = q->len;
        n++;
    }
    rx_p = p;
    return n;
}

static void rx_deliver(struct netif *nif, struct pbuf *p) {
    if (!nif || nif->input(p, nif) != ERR_OK) { pbuf_free(p); }
}

static void rx_end(void *ctx, bool ok) {
    (void)ctx;
    struct pbuf *p = rx_p;
    rx_p = NULL;
    if (!p) { return; }
    if (!ok) { pbuf_free(p); return; }
    /* The SPI fill already took one pool buffer. Free it before input when
     * the responder is off, so the frame is not processed further. */
    {
        uint8_t hdr[64];
        u16_t hn = pbuf_copy_partial(p, hdr, (u16_t)sizeof hdr, 0);
        if (mdns_should_drop(hdr, hn)) { pbuf_free(p); return; }
    }
#if CONFIG_NET_GW_LAN
    struct netif *ll = eth_ll_netif();
    if (ll && rx_share(p)) {
        /* ethernet_input frees the pbuf, so the companion gets a clone.
         * A failed clone still delivers the original to e0. */
        struct pbuf *c = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
        rx_deliver(&eth_if, p);
        if (c) { rx_deliver(ll, c); }
        return;
    }
#endif
    rx_deliver(&eth_if, p);
}

static int tx_peek(void *ctx, w5500_buf_t *b, int max) {
    (void)ctx; (void)max;
    if (!tx_count) { return 0; }
    struct pbuf *p = txq[tx_head];
    b[0].p = (uint8_t *)p->payload;
    b[0].len = p->len;
    return 1;
}

static void tx_done(void *ctx, bool ok) {
    (void)ctx;
    if (!tx_count) { return; }
    if (!ok) { tx_dropped++; }
    pbuf_free(txq[tx_head]);
    txq[tx_head] = NULL;
    tx_head = (uint8_t)((tx_head + 1) % ETH_TXQ);
    tx_count--;
}

static void emit_state(void);

static void on_link(void *ctx, bool up, int speed, bool full) {
    (void)ctx;
    printf("eth: link %s%s\n", up ? "up " : "down", up ? (speed == 100 ? (full ? "100/full" : "100/half")
                                                        : (full ? "10/full" : "10/half")) : "");
    if (up) {
        netif_set_link_up(&eth_if);     /* lwIP restarts DHCP itself */
        ll_since = now_ms();            /* DHCP first, link-local later */
#if CONFIG_NET_GW_LAN
        if (gw_lan_hold_ll()) { ll_companion_up(); }
#endif
    } else {
        ll_stop();
#if CONFIG_NET_GW_LAN
        ll_companion_down();
#endif
        netif_set_link_down(&eth_if);
    }
    emit_state();
    uplink_reselect();
}

static const w5500_ops_t chip_ops = { rx_begin, rx_end, tx_peek, tx_done, on_link };

/* --- netif --------------------------------------------------------------- */

static err_t eth_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;
    if (!present || !chip.link_up) { tx_dropped++; return ERR_IF; }
    if (tx_count >= ETH_TXQ) { txq_full++; return ERR_MEM; }
    struct pbuf *c = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
    if (!c) { txq_full++; return ERR_MEM; }
    txq[(tx_head + tx_count) % ETH_TXQ] = c;
    tx_count++;
    return ERR_OK;
}

static err_t eth_netif_init(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = '0';
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
    netif->output = etharp_output;
    netif->linkoutput = eth_linkoutput;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, chip.mac, 6);
    return ERR_OK;
}

#if CONFIG_NET_GW_LAN
static err_t eth_ll_netif_init(struct netif *netif) {
    err_t e = eth_netif_init(netif);
    if (e != ERR_OK) { return e; }
    netif->name[0] = 'e';
    netif->name[1] = 'l';
    return ERR_OK;
}
#endif

/* --- config -------------------------------------------------------------- */

static void to_ip4(const uint8_t a[4], ip4_addr_t *o) { IP4_ADDR(o, a[0], a[1], a[2], a[3]); }

static void load_cfg(void) {
    static char buf[256];
    bool have = cfg_file_read(ETH_CFG_PATH, buf, sizeof buf);
    eth_cfg_load(have ? buf : NULL, &cfg);       /* anything wrong -> DHCP client */
    if (have) { printf("eth: %s -> mode %s\n", ETH_CFG_PATH, eth_mode_name(cfg.mode)); }
}

static void save_cfg(void) {
    char b[200];
    eth_cfg_json(&cfg, b, sizeof b);
    if (cfg_file_write(ETH_CFG_PATH, b)) { cfg_dirty = false; }
}

static void apply_cfg(void) {
    if (!netif_added) { return; }
    if (srv_on) { dhcp_server_deinit(&dsrv); srv_on = false; }
    ll_stop();
    ip4_addr_t ip, mask, gw;
    to_ip4(cfg.ip, &ip); to_ip4(cfg.mask, &mask); to_ip4(cfg.gw, &gw);
    switch (cfg.mode) {
    case ETH_MODE_MANUAL:
        dhcp_release_and_stop(&eth_if);
        netif_set_addr(&eth_if, &ip, &mask, &gw);
        printf("eth: manual %s\n", ip4addr_ntoa(&ip));
        break;
    case ETH_MODE_SERVER: {
        dhcp_release_and_stop(&eth_if);
        netif_set_addr(&eth_if, &ip, &mask, IP4_ADDR_ANY4);   /* no gateway: never an uplink */
        ip_addr_t sip, smask;
        ip_addr_copy_from_ip4(sip, ip);
        ip_addr_copy_from_ip4(smask, mask);
        dhcp_server_init(&dsrv, &eth_if, &sip, &smask);
        /* No router/DNS option unless asked: the gateway does no routing, so
         * a PC on this cable must keep its own default route. */
        dsrv.no_router = !cfg.dhcp_router;
        srv_on = true;
        printf("eth: DHCP server on %s%s\n", ip4addr_ntoa(&ip),
               cfg.dhcp_router ? " (advertising itself as router)" : "");
        break;
    }
    default:
        netif_set_addr(&eth_if, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        dhcp_start(&eth_if);
        ll_since = now_ms();
        if (gw_lan_hold_ll()) {
            printf("eth: DHCP client (link-local stays on the companion)\n");
        } else {
            printf("eth: DHCP client (link-local after %u s without a lease)\n", ETH_LL_AFTER_MS / 1000);
        }
        break;
    }
}

bool eth_configure(const char *json, const char **why) {
    eth_cfg_t c;
    if (!eth_cfg_parse(json, &c, why)) { return false; }
    staged = c;
    staged_pending = true;
    return true;
}

/* --- lifecycle ----------------------------------------------------------- */

bool eth_init(void) {
    load_cfg();
    spibus_t *bus = spibus_init(CONFIG_ETH_W5500_SPI, CONFIG_ETH_W5500_SCK,
                                CONFIG_ETH_W5500_MOSI, CONFIG_ETH_W5500_MISO, "w5500");
    if (!bus) { printf("eth: SPI%d unavailable\n", CONFIG_ETH_W5500_SPI); return false; }

    /* Locally administered, from the board ID, distinct from USB-NCM's
     * (02:...) so the two never collide on one host. */
    uint8_t mac[6];
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    mac[0] = 0x06;
    memcpy(&mac[1], id.id + 3, 5);

    present = w5500_init(&chip, bus, CONFIG_ETH_W5500_CS, CONFIG_ETH_W5500_BAUD,
                         CONFIG_ETH_W5500_INT, CONFIG_ETH_W5500_RST, mac,
#if CONFIG_ETH_W5500_MAC_FILTER
                         true,
#else
                         false,
#endif
                         &chip_ops, NULL);
    if (!present) {
        /* No chip: give everything back -- pins, SPI block, DMA channels --
         * so the board is as if the driver were not built in. */
        w5500_release(&chip, CONFIG_ETH_W5500_RST);
        spibus_deinit(bus);
        printf("eth: W5500 not present (pins and SPI%d released)\n", CONFIG_ETH_W5500_SPI);
        return false;
    }
    if (!netif_add(&eth_if, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL,
                   eth_netif_init, ethernet_input)) {
        printf("eth: netif_add failed\n");
        present = false;
        return false;
    }
    netif_added = true;
    netif_set_hostname(&eth_if, mdns_hostname());
    netif_set_up(&eth_if);
    /* DELIBERATELY NOT netif_set_default(): src/net/uplink.c decides. */
#if CONFIG_NET_GW_LAN
    if (gw_lan_hold_ll() &&
        netif_add(&eth_ll, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL,
                  eth_ll_netif_init, ethernet_input)) {
        ll_added = true;
        netif_set_up(&eth_ll);
    } else if (gw_lan_hold_ll()) {
        printf("eth: link-local netif_add failed\n");
    }
#endif
    apply_cfg();
    return true;
}

bool eth_present(void) { return present; }
struct netif *eth_netif(void) { return present ? &eth_if : NULL; }

static void emit_state(void) {
    char b[96], ip[16];
    ip4addr_ntoa_r(netif_ip4_addr(&eth_if), ip, sizeof ip);
    snprintf(b, sizeof b, "{\"present\":%s,\"link\":%s,\"ip\":\"%s\"}",
             present ? "true" : "false", chip.link_up ? "true" : "false", ip);
    api_emit("eth.state", b);
}

void eth_poll(void) {
    /* Config first: it is saved even with no chip fitted, so the module can
     * be configured before it is plugged in. */
    if (staged_pending) {
        staged_pending = false;
        cfg = staged;
        cfg_dirty = true;
        apply_cfg();
    }
    if (cfg_dirty) { save_cfg(); }
    if (!present) { return; }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    /* Several rounds per pass: a register transaction is a few microseconds
     * of DMA, so it is usually complete by the next round. A frame is still
     * moved in the background; this never waits on the bus. */
    for (int k = 0; k < 8; k++) {
        spibus_poll();
        if (!w5500_step(&chip, now)) { break; }
    }

    /* DHCP mode: link-local beside DHCP when no lease comes; a lease
     * replaces it; a lost lease starts the wait again. */
    if (cfg.mode == ETH_MODE_DHCP && netif_added) {
        uint32_t now = now_ms();
        bool lease = dhcp_supplied_address(&eth_if);
        if (lease != had_lease) {
            had_lease = lease;
            if (lease) {
                leases++;
                /* The companion keeps its address for the life of the link. */
                if (!gw_lan_hold_ll()) { ll_stop(); }
                printf("eth: DHCP lease %s\n", ip4addr_ntoa(netif_ip4_addr(&eth_if)));
            } else { ll_since = now; }
        }
        if (!gw_lan_hold_ll() && eth_ll_due(cfg.mode, chip.link_up, lease, ll_active, ll_since, now)) {
            ll_active = true;
            ll_starts++;
            printf("eth: no DHCP lease after %u s -- adding link-local (DHCP keeps trying)\n",
                   ETH_LL_AFTER_MS / 1000);
            autoip_start(&eth_if);
            last_kick = now;
        }
        if (eth_dhcp_kick_due(cfg.mode, ll_active, lease, last_kick, now)) {
            last_kick = now;
            dhcp_start(&eth_if);    /* back to a fast discover; the link-local address stays */
        }
    }

#if CONFIG_NET_GW_LAN
    ll_note_addr();
#endif

    bool has = !ip4_addr_isany_val(*netif_ip4_addr(&eth_if)) && netif_is_link_up(&eth_if);
    if (has != had_addr) {
        had_addr = has;
        if (has) {
            printf("eth: address %s\n", ip4addr_ntoa(netif_ip4_addr(&eth_if)));
            mdns_add_netif(&eth_if);
        } else {
            mdns_remove_netif(&eth_if);
        }
        emit_state();
        uplink_reselect();
    }
}

static const char *addr_source(void) {
    if (!netif_added || ip4_addr_isany_val(*netif_ip4_addr(&eth_if))) { return "none"; }
    if (cfg.mode == ETH_MODE_MANUAL) { return "manual"; }
    if (cfg.mode == ETH_MODE_SERVER) { return "server"; }
    if (dhcp_supplied_address(&eth_if)) { return "dhcp"; }
    if (ip4_addr_islinklocal(netif_ip4_addr(&eth_if))) { return "link-local"; }
    return "dhcp";
}

/* For uplink.c: e0 as a candidate. Never in server mode (it has no gateway
 * then anyway; this makes it explicit). */
struct netif *eth_uplink_netif(void) {
    return (present && cfg.mode != ETH_MODE_SERVER) ? &eth_if : NULL;
}

static void fmt4(const uint8_t a[4], char *s, size_t cap) {
    if (!(a[0] | a[1] | a[2] | a[3])) { s[0] = 0; return; }
    snprintf(s, cap, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

size_t eth_status_json(char *dst, size_t cap) {
    char ip[16], m[16], g[16], d[16], cip[16], cm[16], cg[16];
    ip4addr_ntoa_r(netif_ip4_addr(&eth_if), ip, sizeof ip);
    ip4addr_ntoa_r(netif_ip4_netmask(&eth_if), m, sizeof m);
    ip4addr_ntoa_r(netif_ip4_gw(&eth_if), g, sizeof g);
    fmt4(cfg.dns, d, sizeof d);
    fmt4(cfg.ip, cip, sizeof cip);
    fmt4(cfg.mask, cm, sizeof cm);
    fmt4(cfg.gw, cg, sizeof cg);
    const struct dhcp *dh = netif_added ? netif_dhcp_data(&eth_if) : NULL;
    const uint8_t *mac = chip.mac;
    return (size_t)snprintf(dst, cap,
        "{\"present\":%s,\"link\":%s,\"speed\":%d,\"duplex\":\"%s\","
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"mode\":\"%s\","
        "\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\",\"dhcp_state\":%d,"
        "\"cfg\":{\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\"},"
        "\"rx_frames\":%lu,\"tx_frames\":%lu,\"rx_bytes\":%lu,\"tx_bytes\":%lu,"
        "\"rx_dropped\":%lu,\"rx_nomem\":%lu,\"tx_dropped\":%lu,\"txq_full\":%lu,"
        "\"resyncs\":%lu,\"tx_timeouts\":%lu,\"bus_busy\":%lu,"
        "\"addr_source\":\"%s\",\"link_local_starts\":%lu,\"leases\":%lu,\"dhcp_router\":%s,"
        "\"is_default_route\":%s}",
        present ? "true" : "false", chip.link_up ? "true" : "false", chip.speed,
        chip.link_up ? (chip.full_duplex ? "full" : "half") : "",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        eth_mode_name(cfg.mode),
        present ? ip : "", present ? m : "", present ? g : "",
        d,
        dh ? (int)dh->state : -1,
        cip, cm, cg, d,
        (unsigned long)chip.rx_frames, (unsigned long)chip.tx_frames,
        (unsigned long)chip.rx_bytes, (unsigned long)chip.tx_bytes,
        (unsigned long)chip.rx_dropped, (unsigned long)rx_nomem,
        (unsigned long)tx_dropped, (unsigned long)txq_full,
        (unsigned long)chip.resyncs, (unsigned long)chip.tx_timeouts,
        (unsigned long)chip.bus_busy,
        addr_source(), (unsigned long)ll_starts, (unsigned long)leases,
        cfg.dhcp_router ? "true" : "false",
        (netif_added && netif_default == &eth_if) ? "true" : "false");
}

#endif /* CONFIG_NET_ETH_W5500 */
