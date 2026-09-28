/* USB CDC-NCM <-> lwIP netif glue. Adapted from the confirmed-working
 * reference at /home/samuel/Projects/pm220-pico2w/src/net/usb_ncm.c —
 * already netif-generic (no Wi-Fi/AP specifics despite living next to
 * that project's Wi-Fi code). The only real change: dropped the
 * cyw43_arch_lwip_begin/end() locking, which only mattered there because
 * cyw43's driver runs concurrently from its own IRQ context; this
 * project has a single polling loop with no such concurrent access to
 * guard against. */
#include "net/usb_ncm.h"

#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "pico/unique_id.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "dhcpserver.h"
#include "net/mdns.h"

/* The subnet comes from Kconfig (app_config.h). It was hardcoded to
 * 192.168.7.1 in the project this file came from, which is precisely the
 * subnet this firmware must NOT use -- that project is still live on the same
 * bench, and two NCM interfaces on one subnet is a routing coin-toss. */
#include "app_config.h"

#define NCM_RX_N    4
#define NCM_RX_MAX  1524

uint8_t tud_network_mac_address[6];

static struct netif ncm_netif;
static dhcp_server_t dhcp_ncm;
static uint8_t ncm_rx_data[NCM_RX_N][NCM_RX_MAX];
static volatile uint16_t ncm_rx_len[NCM_RX_N];
static volatile uint8_t ncm_rx_w;
static uint8_t ncm_rx_r;
static unsigned ncm_rx_ok, ncm_rx_drop, ncm_tx_ok, ncm_tx_drop;
/* How many times the pull-up had to be bounced before the host
 * enumerated us. Zero is the expected reading. */
static unsigned ncm_recoveries;
static unsigned ncm_rx_ok_last, ncm_tx_ok_last, ncm_rx_drop_last, ncm_tx_drop_last;

void usb_ncm_stats(usb_ncm_stats_t *out) {
    out->rx_ok = ncm_rx_ok; out->rx_drop = ncm_rx_drop;
    out->tx_ok = ncm_tx_ok; out->tx_drop = ncm_tx_drop;
    out->recoveries = ncm_recoveries;
}

static err_t ncm_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;
    if (!tud_ready() || !tud_network_can_xmit(p->tot_len)) {
        ncm_tx_drop++;
        return ERR_USE;
    }
    tud_network_xmit(p, 0);
    ncm_tx_ok++;
    return ERR_OK;
}

static err_t ncm_input(struct pbuf *p, struct netif *inp) {
    dhcp_try_eth_input(&dhcp_ncm, p);
    return ethernet_input(p, inp);
}

static err_t ncm_netif_init(struct netif *netif) {
    usb_ncm_ensure_mac();
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
    netif->name[0] = 'u';
    netif->name[1] = 's';
    netif->output = etharp_output;
    netif->linkoutput = ncm_linkoutput;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, tud_network_mac_address, 6);
    netif->hwaddr[5] ^= 0x01;
    return ERR_OK;
}

void usb_ncm_ensure_mac(void) {
    if (tud_network_mac_address[0]) {
        return;
    }
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    tud_network_mac_address[0] = 0x02;
    memcpy(&tud_network_mac_address[1], id.id + 3, 5);
}

struct netif *usb_ncm_netif(void) {
    return &ncm_netif;
}

void usb_ncm_init(void) {
    usb_ncm_ensure_mac();
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, USB_IP0, USB_IP1, USB_IP2, USB_IP3);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, USB_IP0, USB_IP1, USB_IP2, USB_IP3);
    netif_add(&ncm_netif, &ip, &mask, &gw, NULL, ncm_netif_init, ncm_input);
    netif_set_up(&ncm_netif);
    netif_set_link_up(&ncm_netif);
    dhcp_server_init(&dhcp_ncm, &ncm_netif, (const ip_addr_t *)&gw, (const ip_addr_t *)&mask);
    mdns_add_netif(&ncm_netif);
    printf("usb ncm %s mac %02x:%02x:%02x:%02x:%02x:%02x  (host %d.%d.%d.16)\n",
           ip4addr_ntoa(&ip),
           ncm_netif.hwaddr[0], ncm_netif.hwaddr[1], ncm_netif.hwaddr[2],
           ncm_netif.hwaddr[3], ncm_netif.hwaddr[4], ncm_netif.hwaddr[5],
           USB_IP0, USB_IP1, USB_IP2);
}

void usb_ncm_poll(void) {
    for (;;) {
        uint16_t n = ncm_rx_len[ncm_rx_r];
        if (!n) {
            break;
        }
        struct pbuf *p = pbuf_alloc(PBUF_RAW, n, PBUF_POOL);
        if (p) {
            pbuf_take(p, ncm_rx_data[ncm_rx_r], n);
            if (ncm_netif.input(p, &ncm_netif) != ERR_OK) {
                pbuf_free(p);
            }
            ncm_rx_ok++;
        } else {
            ncm_rx_drop++;
        }
        ncm_rx_len[ncm_rx_r] = 0;
        ncm_rx_r = (uint8_t)((ncm_rx_r + 1) % NCM_RX_N);
    }
    tud_network_recv_renew();
}

/* Re-presents the USB disconnect if the host never enumerated us.
 *
 * The disconnect at boot (src/main.c) covers the usual case, but one
 * fixed delay cannot cover them all: whether the host notices depends
 * on when its hub polling happens to fall, and a missed detach leaves
 * the device unconfigured for good — tud_ready() false forever, NCM
 * dead, and the host still holding the previous session. Rather than
 * tune the delay, notice and try again.
 *
 * The test is tud_mounted(), NOT tud_ready(): ready is also false
 * while the host has merely suspended us, and bouncing the pull-up at a
 * healthy suspended link would create the very problem this exists to
 * fix. And only in the first few seconds after boot — a board on a
 * bench with no host plugged in is never mounted either, and there is
 * nothing there to re-enumerate. */
#define NCM_RECOVER_TRIES 3
#define NCM_RECOVER_AFTER_MS 2500
#define NCM_RECOVER_WINDOW_MS 20000

void usb_ncm_recover_tick(void) {
    static uint8_t tries;
    static uint32_t last_ms;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (tud_mounted()) {
        tries = 0;
        last_ms = 0;
        return;
    }
    if (tries >= NCM_RECOVER_TRIES || now > NCM_RECOVER_WINDOW_MS) {
        return;
    }
    if (last_ms == 0) {
        last_ms = now;
        return;
    }
    if (now - last_ms < NCM_RECOVER_AFTER_MS) {
        return;
    }
    last_ms = now;
    tries++;
    ncm_recoveries++;
    tud_disconnect();
    busy_wait_ms(200);
    tud_connect();
}

/* The same counters usb_ncm_log() prints, for /api/debug/crash — which
 * is to say, readable over WiFi while USB-NCM itself is the thing that
 * is not working. The UART those prints go to is not wired on this
 * board, so when NCM wedges there was previously no way to see whether
 * the device was still receiving, still transmitting, or had stopped
 * being asked at all. */
int usb_ncm_debug_report(char *buf, size_t cap) {
    const struct netif *n = &ncm_netif;
    return snprintf(buf, cap,
                    "ncm: rx=%u drop=%u tx=%u drop=%u ready=%d mounted=%d can_xmit=%d link=%d up=%d "
                    "rx_ring_w=%u rx_ring_r=%u recoveries=%u\n",
                    ncm_rx_ok, ncm_rx_drop, ncm_tx_ok, ncm_tx_drop, (int)tud_ready(), (int)tud_mounted(),
                    (int)tud_network_can_xmit(64), netif_is_link_up(n) ? 1 : 0, netif_is_up(n) ? 1 : 0,
                    (unsigned)ncm_rx_w, (unsigned)ncm_rx_r, ncm_recoveries);
}

void usb_ncm_log(void) {
    if (ncm_rx_ok == ncm_rx_ok_last && ncm_tx_ok == ncm_tx_ok_last &&
        ncm_rx_drop == ncm_rx_drop_last && ncm_tx_drop == ncm_tx_drop_last) {
        return;
    }
    ncm_rx_ok_last = ncm_rx_ok;
    ncm_tx_ok_last = ncm_tx_ok;
    ncm_rx_drop_last = ncm_rx_drop;
    ncm_tx_drop_last = ncm_tx_drop;
    printf("ncm rx %u drop %u  tx %u drop %u  ready %d can_xmit %d\n",
           ncm_rx_ok, ncm_rx_drop, ncm_tx_ok, ncm_tx_drop,
           (int)tud_ready(), (int)tud_network_can_xmit(64));
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (!size) {
        return true;
    }
    if (size > NCM_RX_MAX) {
        ncm_rx_drop++;
        return false;
    }
    uint8_t w = ncm_rx_w;
    if (ncm_rx_len[w] != 0) {
        ncm_rx_drop++;
        return false;
    }
    memcpy(ncm_rx_data[w], src, size);
    ncm_rx_len[w] = size;
    ncm_rx_w = (uint8_t)((w + 1) % NCM_RX_N);
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    (void)arg;
    struct pbuf *p = (struct pbuf *)ref;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void tud_network_init_cb(void) {
    for (int i = 0; i < NCM_RX_N; i++) {
        ncm_rx_len[i] = 0;
    }
    ncm_rx_w = 0;
    ncm_rx_r = 0;
}

void tud_mount_cb(void) {
    printf("usb: mounted (NCM+CDC)\n");
}

void tud_umount_cb(void) {
    printf("usb: unmounted\n");
}
