/*
 * lwIP glue for the W5500 on the C5. Frames follow the Pico glue: one
 * pool buffer for a received frame, a cloned RAM buffer for a transmit,
 * and the chip state machine never waits on the wire.
 *
 * Link-local stays on companion netif "el" for the life of the link when
 * cfg/gw.json names e0. A missing file names e0, because this image has
 * the W5500. A stored "none" keeps today's other rule: link-local on e0
 * after ETH_LL_AFTER_MS, and a DHCP lease replaces it. A lease is never
 * the only address while el is up.
 *
 * DELIBERATELY NOT netif_set_default().
 *
 * eth_init and eth_poll run off the tcpip thread. The caller holds
 * LOCK_TCPIP_CORE() for the whole call, which requires
 * CONFIG_LWIP_TCPIP_CORE_LOCKING.
 */
#include "eth_c5.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include "lwip/acd.h"
#include "lwip/autoip.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ethernet.h"
#include "netif/ethernet.h"

#include "../../include/dhcpserver.h"
#include "fs_lfs.h"
#include "io/spibus.h"
#include "net/eth_cfg.h"
#include "net/gw_core.h"
#include "net/w5500.h"

static const char *TAG = "eth";

#define ETH_CFG_PATH "cfg/eth.json"
#define GW_CFG_PATH  "cfg/gw.json"
#define ETH_TXQ 6

static w5500_t chip;
static struct netif eth_if;
static struct netif eth_ll;
static bool present, netif_added, ll_added, ll_had_addr, keep_ll;

static struct pbuf *txq[ETH_TXQ];
static uint8_t tx_head, tx_count;
static struct pbuf *rx_p;

static eth_cfg_t cfg;
static bool had_addr;
static uint32_t ll_since;
static bool ll_active;
static bool had_lease;
static uint32_t last_kick;
static dhcp_server_t dsrv;
static bool srv_on;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

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
    if (has) { ESP_LOGI(TAG, "link-local %s", ip4addr_ntoa(netif_ip4_addr(&eth_ll))); }
}

static void acd_halt(struct netif *nif) {
    struct autoip *a = netif_autoip_data(nif);
    if (a) { acd_stop(&a->acd); }
}

static void ll_companion_down(void) {
    if (!ll_added) { return; }
    autoip_stop(&eth_ll);
    acd_halt(&eth_ll);
    netif_set_link_down(&eth_ll);
    ll_note_addr();
}

static void ll_companion_up(void) {
    if (!ll_added) { return; }
    netif_set_link_up(&eth_ll);
    autoip_start(&eth_ll);
    ESP_LOGI(TAG, "link-local companion up (kept beside any lease)");
}

static void ll_stop(void) {
    if (!ll_active) { return; }
    ll_active = false;
    autoip_stop(&eth_if);
    acd_halt(&eth_if);
}

static int rx_begin(void *ctx, uint16_t len, w5500_buf_t *b, int max) {
    (void)ctx;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (!p) { return 0; }
    int n = 0;
    for (struct pbuf *q = p; q; q = q->next) {
        if (n >= max) { pbuf_free(p); return 0; }
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
    if (ll_added && rx_share(p)) {
        struct pbuf *c = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
        rx_deliver(&eth_if, p);
        if (c) { rx_deliver(&eth_ll, c); }
        return;
    }
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
    (void)ok;
    pbuf_free(txq[tx_head]);
    txq[tx_head] = NULL;
    tx_head = (uint8_t)((tx_head + 1) % ETH_TXQ);
    tx_count--;
}

static void on_link(void *ctx, bool up, int speed, bool full) {
    (void)ctx;
    ESP_LOGI(TAG, "link %s%s", up ? "up " : "down",
             up ? (speed == 100 ? (full ? "100/full" : "100/half")
                                : (full ? "10/full" : "10/half"))
                : "");
    if (up) {
        netif_set_link_up(&eth_if);
        ll_since = now_ms();
        if (keep_ll) { ll_companion_up(); }
    } else {
        ll_stop();
        ll_companion_down();
        netif_set_link_down(&eth_if);
    }
}

static const w5500_ops_t chip_ops = { rx_begin, rx_end, tx_peek, tx_done, on_link };

static err_t eth_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;
    if (!present || !chip.link_up) { return ERR_IF; }
    if (tx_count >= ETH_TXQ) { return ERR_MEM; }
    struct pbuf *c = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
    if (!c) { return ERR_MEM; }
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

static err_t eth_ll_netif_init(struct netif *netif) {
    err_t e = eth_netif_init(netif);
    if (e != ERR_OK) { return e; }
    netif->name[0] = 'e';
    netif->name[1] = 'l';
    return ERR_OK;
}

static void to_ip4(const uint8_t a[4], ip4_addr_t *o) { IP4_ADDR(o, a[0], a[1], a[2], a[3]); }

/* Missing cfg/gw.json is e0 on this image. Only e0 keeps the companion. */
static bool gw_iface_holds_e0(void) {
    char buf[128];
    size_t got = 0;
    char iface[GW_IFACE_LEN];
    const char *json = NULL;
    if (fs_read_file(GW_CFG_PATH, buf, sizeof buf - 1, &got) == 0 && got > 0) {
        buf[got] = 0;
        json = buf;
    }
    gw_cfg_load(json, true, iface, sizeof iface);
    ESP_LOGI(TAG, "%s -> iface %s", json ? GW_CFG_PATH : "no cfg/gw.json", iface);
    return strcmp(iface, "e0") == 0;
}

static void load_cfg(void) {
    char buf[256];
    size_t got = 0;
    const char *json = NULL;
    if (fs_read_file(ETH_CFG_PATH, buf, sizeof buf - 1, &got) == 0 && got > 0) {
        buf[got] = 0;
        json = buf;
    }
    eth_cfg_load(json, &cfg);
    if (json) { ESP_LOGI(TAG, "%s -> mode %s", ETH_CFG_PATH, eth_mode_name(cfg.mode)); }
}

static void apply_cfg(void) {
    if (!netif_added) { return; }
    if (srv_on) { dhcp_server_deinit(&dsrv); srv_on = false; }
    ll_stop();
    ip4_addr_t ip, mask, gw;
    to_ip4(cfg.ip, &ip);
    to_ip4(cfg.mask, &mask);
    to_ip4(cfg.gw, &gw);
    switch (cfg.mode) {
    case ETH_MODE_MANUAL:
        dhcp_release_and_stop(&eth_if);
        netif_set_addr(&eth_if, &ip, &mask, &gw);
        ESP_LOGI(TAG, "manual %s", ip4addr_ntoa(&ip));
        break;
    case ETH_MODE_SERVER: {
        dhcp_release_and_stop(&eth_if);
        netif_set_addr(&eth_if, &ip, &mask, IP4_ADDR_ANY4);
        ip_addr_t sip, smask;
        ip_addr_copy_from_ip4(sip, ip);
        ip_addr_copy_from_ip4(smask, mask);
        dhcp_server_init(&dsrv, &eth_if, &sip, &smask);
        dsrv.no_router = !cfg.dhcp_router;
        srv_on = true;
        ESP_LOGI(TAG, "DHCP server on %s%s", ip4addr_ntoa(&ip),
                 cfg.dhcp_router ? " (advertising itself as router)" : "");
        break;
    }
    default:
        netif_set_addr(&eth_if, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        dhcp_start(&eth_if);
        ll_since = now_ms();
        if (keep_ll) {
            ESP_LOGI(TAG, "DHCP client (link-local stays on the companion)");
        } else {
            ESP_LOGI(TAG, "DHCP client (link-local after %u s without a lease)",
                     ETH_LL_AFTER_MS / 1000);
        }
        break;
    }
}

bool eth_init(void) {
    load_cfg();
    keep_ll = gw_iface_holds_e0();
    spibus_t *bus = spibus_init(CONFIG_ETH_W5500_SPI, CONFIG_ETH_W5500_SCK,
                                CONFIG_ETH_W5500_MOSI, CONFIG_ETH_W5500_MISO, "w5500");
    if (!bus) {
        ESP_LOGE(TAG, "SPI%d unavailable", CONFIG_ETH_W5500_SPI);
        return false;
    }

    /* Locally administered, distinct from the station MAC. */
    uint8_t id[6], mac[6];
    if (esp_read_mac(id, ESP_MAC_BASE) != ESP_OK) { memset(id, 0, sizeof id); }
    mac[0] = 0x06;
    memcpy(&mac[1], id + 1, 5);

    present = w5500_init(&chip, bus, CONFIG_ETH_W5500_CS, CONFIG_ETH_W5500_BAUD,
                         CONFIG_ETH_W5500_INT, CONFIG_ETH_W5500_RST, mac,
#if CONFIG_ETH_W5500_MAC_FILTER
                         true,
#else
                         false,
#endif
                         &chip_ops, NULL);
    if (!present) {
        w5500_release(&chip, CONFIG_ETH_W5500_RST);
        spibus_deinit(bus);
        ESP_LOGW(TAG, "W5500 not present (pins and SPI%d released)", CONFIG_ETH_W5500_SPI);
        return false;
    }
    if (!netif_add(&eth_if, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL,
                   eth_netif_init, ethernet_input)) {
        ESP_LOGE(TAG, "netif_add failed");
        present = false;
        w5500_release(&chip, CONFIG_ETH_W5500_RST);
        spibus_deinit(bus);
        return false;
    }
    netif_added = true;
    netif_set_up(&eth_if);
    if (keep_ll) {
        if (netif_add(&eth_ll, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL,
                      eth_ll_netif_init, ethernet_input)) {
            ll_added = true;
            netif_set_up(&eth_ll);
        } else {
            ESP_LOGE(TAG, "link-local netif_add failed");
        }
    }
    apply_cfg();
    return true;
}

bool eth_present(void) { return present; }

void eth_poll(void) {
    if (!present) { return; }
    uint32_t now = now_ms();
    for (int k = 0; k < 8; k++) {
        spibus_poll();
        if (!w5500_step(&chip, now)) { break; }
    }

    if (cfg.mode == ETH_MODE_DHCP && netif_added) {
        now = now_ms();
        bool lease = dhcp_supplied_address(&eth_if);
        if (lease != had_lease) {
            had_lease = lease;
            if (lease) {
                if (!keep_ll) { ll_stop(); }
                ESP_LOGI(TAG, "DHCP lease %s", ip4addr_ntoa(netif_ip4_addr(&eth_if)));
            } else {
                ll_since = now;
            }
        }
        if (!keep_ll && eth_ll_due(cfg.mode, chip.link_up, lease, ll_active, ll_since, now)) {
            ll_active = true;
            ESP_LOGI(TAG, "no DHCP lease after %u s -- adding link-local (DHCP keeps trying)",
                     ETH_LL_AFTER_MS / 1000);
            autoip_start(&eth_if);
            last_kick = now;
        }
        if (eth_dhcp_kick_due(cfg.mode, ll_active, lease, last_kick, now)) {
            last_kick = now;
            dhcp_start(&eth_if);
        }
    }

    if (keep_ll) { ll_note_addr(); }

    bool has = !ip4_addr_isany_val(*netif_ip4_addr(&eth_if)) && netif_is_link_up(&eth_if);
    if (has != had_addr) {
        had_addr = has;
        if (has) { ESP_LOGI(TAG, "address %s", ip4addr_ntoa(netif_ip4_addr(&eth_if))); }
    }
}
