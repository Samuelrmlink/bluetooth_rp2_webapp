/* PANU to a phone NAP. One netif, "b0", which uplink may select. This file
 * does not call netif_set_default. Nothing connects until classic.nap. */
#include "bt/classic/nap.h"

#include <stdio.h>
#include <string.h>

#include "api/ops.h"
#include "btstack.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "net/mdns.h"
#include "net/uplink.h"

#define NAP_FRAME 1514

static struct netif s_nif;
static int s_added;
static uint16_t s_cid;
static int s_connecting;
static int s_in;
static int s_tx_armed;
static int s_dhcp;
static struct pbuf *s_tx;
static uint8_t s_frame[NAP_FRAME];

static void arm_tx(void) {
    if (s_in || s_tx_armed || !s_tx || !s_cid) { return; }
    s_tx_armed = 1;
    bnep_request_can_send_now_event(s_cid);
}

static err_t nap_linkoutput(struct netif *nif, struct pbuf *p) {
    (void)nif;
    if (!s_cid || s_tx || !p || p->tot_len == 0 || p->tot_len > NAP_FRAME) {
        return ERR_MEM;
    }
    pbuf_ref(p);
    s_tx = p;
    arm_tx();
    return ERR_OK;
}

static err_t nap_netif_init(struct netif *nif) {
    nif->mtu = 1500;
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    nif->name[0] = 'b';
    nif->name[1] = '0';
    nif->output = etharp_output;
    nif->linkoutput = nap_linkoutput;
    nif->hwaddr_len = 6;
    return ERR_OK;
}

static void nap_down(void) {
    if (s_tx) {
        pbuf_free(s_tx);
        s_tx = NULL;
    }
    s_tx_armed = 0;
    s_cid = 0;
    s_connecting = 0;
    if (s_dhcp) {
        dhcp_stop(&s_nif);
        s_dhcp = 0;
    }
    if (s_added) {
        netif_set_link_down(&s_nif);
        netif_set_down(&s_nif);
        mdns_remove_netif(&s_nif);
    }
    uplink_reselect();
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    s_in++;
    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(packet)) {
        case BNEP_EVENT_CHANNEL_OPENED:
            s_connecting = 0;
            if (bnep_event_channel_opened_get_status(packet) != ERROR_CODE_SUCCESS) {
                printf("bt: NAP open failed 0x%02x\n",
                       bnep_event_channel_opened_get_status(packet));
                break;
            }
            s_cid = bnep_event_channel_opened_get_bnep_cid(packet);
            {
                bd_addr_t local;
                gap_local_bd_addr(local);
                memcpy(s_nif.hwaddr, local, 6);
            }
            netif_set_up(&s_nif);
            netif_set_link_up(&s_nif);
            dhcp_start(&s_nif);
            s_dhcp = 1;
            mdns_add_netif(&s_nif);
            printf("bt: NAP up cid %u mtu %u\n", s_cid,
                   bnep_event_channel_opened_get_mtu(packet));
            uplink_reselect();
            break;
        case BNEP_EVENT_CHANNEL_CLOSED:
        case BNEP_EVENT_CHANNEL_TIMEOUT:
            printf("bt: NAP down\n");
            nap_down();
            break;
        case BNEP_EVENT_CAN_SEND_NOW:
            s_tx_armed = 0;
            if (s_tx && s_cid) {
                struct pbuf *p = s_tx;
                s_tx = NULL;
                uint16_t n = p->tot_len;
                if (n > NAP_FRAME) { n = NAP_FRAME; }
                pbuf_copy_partial(p, s_frame, n, 0);
                pbuf_free(p);
                bnep_send(s_cid, s_frame, n);
            }
            break;
        default:
            break;
        }
        break;
    case BNEP_DATA_PACKET:
        if (!s_cid || !s_added || size == 0 || size > NAP_FRAME) { break; }
        {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
            if (!p) { break; }
            pbuf_take(p, packet, size);
            if (s_nif.input(p, &s_nif) != ERR_OK) { pbuf_free(p); }
        }
        break;
    default:
        break;
    }
    s_in--;
    if (!s_in) { arm_tx(); }
}

void nap_init(void) {
    bnep_init();
    if (!s_added) {
        if (!netif_add(&s_nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4,
                       NULL, nap_netif_init, ethernet_input)) {
            printf("bt: NAP netif_add failed\n");
            return;
        }
        s_added = 1;
    }
    printf("bt: BNEP PANU ready (classic.nap connects)\n");
}

struct netif *nap_uplink_netif(void) {
    if (!s_cid || !netif_is_up(&s_nif) || !netif_is_link_up(&s_nif)) { return NULL; }
    return &s_nif;
}

void op_classic_nap(const char *params, api_reply_fn reply, void *ctx, long id) {
    if (!s_added) {
        api_reply_err(reply, ctx, id, "failed", "NAP netif is not up");
        return;
    }
    if (s_cid || s_connecting) {
        api_reply_err(reply, ctx, id, "busy", "a NAP link is up");
        return;
    }
    char addr[24];
    addr[0] = 0;
    if (!params || !api_json_str(params, "addr", addr, sizeof addr) || !addr[0]) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"addr\"");
        return;
    }
    bd_addr_t peer;
    if (!sscanf_bd_addr(addr, peer)) {
        api_reply_err(reply, ctx, id, "bad_request", "addr is AA:BB:CC:DD:EE:FF");
        return;
    }
    int err = bnep_connect(packet_handler, peer, BLUETOOTH_PSM_BNEP,
                           BLUETOOTH_SERVICE_CLASS_PANU, BLUETOOTH_SERVICE_CLASS_NAP);
    if (err) {
        api_reply_err(reply, ctx, id, "failed", "BNEP connect did not start");
        return;
    }
    s_connecting = 1;
    api_reply_ok(reply, ctx, id, "{\"pending\":true}");
}
