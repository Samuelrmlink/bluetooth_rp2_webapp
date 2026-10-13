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
#include "netif/ethernet.h"

#include "io/spibus.h"
#include "net/w5500.h"
#include "net/mdns.h"
#include "net/uplink.h"
#include "api/api.h"
#include "api/ops.h"
#include "config/config.h"

#define ETH_CFG_PATH "cfg/eth.json"
#define ETH_TXQ 6

static w5500_t chip;
static struct netif eth_if;
static bool present, netif_added;

static struct pbuf *txq[ETH_TXQ];
static uint8_t tx_head, tx_count;
static uint32_t txq_full, tx_dropped, rx_nomem;
static struct pbuf *rx_p;

/* Config: what is saved, and what is staged by eth.config for the loop. */
typedef struct {
    bool manual;
    ip4_addr_t ip, mask, gw, dns;
} eth_cfg_t;
static eth_cfg_t cfg, staged;
static bool staged_pending, cfg_dirty;

static bool had_addr;

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

static void rx_end(void *ctx, bool ok) {
    (void)ctx;
    struct pbuf *p = rx_p;
    rx_p = NULL;
    if (!p) { return; }
    if (!ok || eth_if.input(p, &eth_if) != ERR_OK) { pbuf_free(p); }
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
    if (up) { netif_set_link_up(&eth_if); }
    else { netif_set_link_down(&eth_if); }
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

/* --- config -------------------------------------------------------------- */

static bool parse_ip(const char *json, const char *key, ip4_addr_t *out, bool required) {
    char s[24];
    if (!api_json_str(json, key, s, sizeof s) || !s[0]) {
        ip4_addr_set_zero(out);
        return !required;
    }
    return ip4addr_aton(s, out) != 0;
}

static bool mask_ok(const ip4_addr_t *m) {
    uint32_t v = lwip_ntohl(ip4_addr_get_u32(m));
    return v != 0 && ((~v + 1) & ~v) == 0;    /* contiguous ones */
}

static bool parse_cfg(const char *json, eth_cfg_t *c, const char **why) {
    char mode[12] = "dhcp";
    api_json_str(json, "mode", mode, sizeof mode);
    memset(c, 0, sizeof *c);
    if (strcmp(mode, "dhcp") == 0) {
        /* Keep any manual fields so switching back restores them in the UI. */
        parse_ip(json, "ip", &c->ip, false);
        parse_ip(json, "mask", &c->mask, false);
        parse_ip(json, "gw", &c->gw, false);
        parse_ip(json, "dns", &c->dns, false);
        return true;
    }
    if (strcmp(mode, "manual") != 0) { *why = "mode is dhcp or manual"; return false; }
    c->manual = true;
    if (!parse_ip(json, "ip", &c->ip, true) || ip4_addr_isany_val(c->ip)) { *why = "manual needs a valid ip"; return false; }
    if (!parse_ip(json, "mask", &c->mask, true) || !mask_ok(&c->mask)) { *why = "manual needs a valid mask"; return false; }
    if (!parse_ip(json, "gw", &c->gw, false)) { *why = "gw is not an address"; return false; }
    if (!ip4_addr_isany_val(c->gw) && !ip4_addr_net_eq(&c->gw, &c->ip, &c->mask)) {
        *why = "gw is not on the ip/mask subnet"; return false;
    }
    if (!parse_ip(json, "dns", &c->dns, false)) { *why = "dns is not an address"; return false; }
    return true;
}

static void load_cfg(void) {
    static char buf[256];
    memset(&cfg, 0, sizeof cfg);
    if (!cfg_file_read(ETH_CFG_PATH, buf, sizeof buf)) { return; }
    const char *why = NULL;
    if (!parse_cfg(buf, &cfg, &why)) {
        printf("eth: %s ignored (%s), using DHCP\n", ETH_CFG_PATH, why);
        memset(&cfg, 0, sizeof cfg);
    }
}

static void save_cfg(void) {
    char b[200], ip[16], m[16], g[16], d[16];
    ip4addr_ntoa_r(&cfg.ip, ip, sizeof ip);
    ip4addr_ntoa_r(&cfg.mask, m, sizeof m);
    ip4addr_ntoa_r(&cfg.gw, g, sizeof g);
    ip4addr_ntoa_r(&cfg.dns, d, sizeof d);
    snprintf(b, sizeof b, "{\"mode\":\"%s\",\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\"}",
             cfg.manual ? "manual" : "dhcp",
             ip4_addr_isany_val(cfg.ip) ? "" : ip, ip4_addr_isany_val(cfg.mask) ? "" : m,
             ip4_addr_isany_val(cfg.gw) ? "" : g, ip4_addr_isany_val(cfg.dns) ? "" : d);
    if (cfg_file_write(ETH_CFG_PATH, b)) { cfg_dirty = false; }
}

static void apply_cfg(void) {
    if (!netif_added) { return; }
    if (cfg.manual) {
        dhcp_release_and_stop(&eth_if);
        netif_set_addr(&eth_if, &cfg.ip, &cfg.mask, &cfg.gw);
        printf("eth: manual %s\n", ip4addr_ntoa(&cfg.ip));
    } else {
        netif_set_addr(&eth_if, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        dhcp_start(&eth_if);
        printf("eth: DHCP\n");
    }
}

bool eth_configure(const char *json, const char **why) {
    eth_cfg_t c;
    if (!parse_cfg(json, &c, why)) { return false; }
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
        printf("eth: W5500 not present\n");
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

size_t eth_status_json(char *dst, size_t cap) {
    char ip[16], m[16], g[16], d[16], cip[16], cm[16], cg[16];
    ip4addr_ntoa_r(netif_ip4_addr(&eth_if), ip, sizeof ip);
    ip4addr_ntoa_r(netif_ip4_netmask(&eth_if), m, sizeof m);
    ip4addr_ntoa_r(netif_ip4_gw(&eth_if), g, sizeof g);
    ip4addr_ntoa_r(&cfg.dns, d, sizeof d);
    ip4addr_ntoa_r(&cfg.ip, cip, sizeof cip);
    ip4addr_ntoa_r(&cfg.mask, cm, sizeof cm);
    ip4addr_ntoa_r(&cfg.gw, cg, sizeof cg);
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
        "\"is_default_route\":%s}",
        present ? "true" : "false", chip.link_up ? "true" : "false", chip.speed,
        chip.link_up ? (chip.full_duplex ? "full" : "half") : "",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        cfg.manual ? "manual" : "dhcp",
        present ? ip : "", present ? m : "", present ? g : "",
        ip4_addr_isany_val(cfg.dns) ? "" : d,
        dh ? (int)dh->state : -1,
        ip4_addr_isany_val(cfg.ip) ? "" : cip, ip4_addr_isany_val(cfg.mask) ? "" : cm,
        ip4_addr_isany_val(cfg.gw) ? "" : cg, ip4_addr_isany_val(cfg.dns) ? "" : d,
        (unsigned long)chip.rx_frames, (unsigned long)chip.tx_frames,
        (unsigned long)chip.rx_bytes, (unsigned long)chip.tx_bytes,
        (unsigned long)chip.rx_dropped, (unsigned long)rx_nomem,
        (unsigned long)tx_dropped, (unsigned long)txq_full,
        (unsigned long)chip.resyncs, (unsigned long)chip.tx_timeouts,
        (unsigned long)chip.bus_busy,
        (netif_added && netif_default == &eth_if) ? "true" : "false");
}

#endif /* CONFIG_NET_ETH_W5500 */
