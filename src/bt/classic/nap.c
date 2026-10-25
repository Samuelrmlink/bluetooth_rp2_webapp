/* BNEP in either direction. One netif, "b0", which uplink may select.
 * This file does not call netif_set_default.
 *
 * Connect dials the phone's NAP: the board is PANU and takes a DHCP lease.
 * Discoverable (cfg/nap.json "listen", default on) lets the phone place the
 * call. The board is then the NAP, at 192.168.44.1/24. The DHCP offer names
 * this board as the phone's router. Share internet (cfg "forward") masquerades
 * the phone's other traffic out "iface" (see src/net/nap_fwd.c). Off, the
 * phone can open this web app and has no route to anywhere else.
 *
 * The UUID pair on an open channel is PANU/NAP either way. s_outgoing is
 * what says who placed the call. */
#include "bt/classic/nap.h"

#include <stdio.h>
#include <string.h>

#include "api/ops.h"
#include "bt/bt.h"
#include "btstack.h"
#include "pico/stdlib.h"
#include "classic/pan.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "config/config.h"
#include "dhcpserver.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "net/mdns.h"
#include "net/nap_fwd.h"
#include "net/uplink.h"

#define NAP_CFG "cfg/nap.json"
#define NAP_FRAME 1514
#define NAP_SDP 256

/* 192.168.44.1/24. Not the USB-NCM 192.168.8.0/24 and not the LAN. */
#define NAP_IP0 192
#define NAP_IP1 168
#define NAP_IP2 44
#define NAP_IP3 1

enum {
    ROLE_NONE = 0,
    ROLE_PANU,
    ROLE_NAP,
};

static struct netif s_nif;
static int s_added;
static uint16_t s_cid;
static int s_connecting;
static uint32_t s_connect_ms;
static uint32_t s_connect_log_ms;
static int s_outgoing;
static int s_role;
static int s_listen = 1;
static int s_forward;
static char s_fwd_iface[4] = "w0";
static int s_in;
static int s_tx_armed;
static int s_dhcp;
static int s_srv;
static int s_cfg_dirty;
/* The phone's web page fits in a handful of frames. Share internet does
 * not: one page load returns a burst, and a full queue used to discard
 * the reply the phone was waiting on. Sixteen holds a short burst. When
 * it is full the oldest frame is the one dropped, so a new reply still
 * goes out. */
#define NAP_TX_Q 16
static struct pbuf *s_txq[NAP_TX_Q];
static int s_tx_n;
static uint32_t s_rx_frames;
static uint32_t s_tx_frames;
static uint32_t s_tx_drop;
static int s_log_arp;
static int s_log_dhcp;
static int s_log_tcp;
static int s_log_mac;
static dhcp_server_t s_dhcp_srv;
static uint8_t s_frame[NAP_FRAME];
static uint8_t s_panu_sdp[NAP_SDP];
static uint8_t s_nap_sdp[NAP_SDP];
static char s_addr[18];
static hci_con_handle_t s_handle;
static btstack_packet_callback_registration_t s_hci;
/* Set on a dial, cleared once the channel is up or the attempt is over.
 * Distinct from s_connecting: the failure text stays after the link is
 * down, and a later Connect is not refused. */
static int s_dial;
static char s_fail[48];
static uint16_t s_net_types[] = { 0x0800, 0x0806, 0 }; /* IPv4, ARP */

/* L2CAP's own timer starts only after the ACL is up and the channel is
 * opening. If the phone drops the ACL before that, nothing tells us, and
 * "connecting" stays set so the next dial is refused. */
#define NAP_CONNECT_TIMEOUT_MS 20000

static void tx_clear(void) {
    while (s_tx_n > 0) {
        s_tx_n--;
        pbuf_free(s_txq[s_tx_n]);
        s_txq[s_tx_n] = NULL;
    }
}

static void arm_tx(void) {
    if (s_in || s_tx_armed || s_tx_n == 0 || !s_cid) { return; }
    s_tx_armed = 1;
    bnep_request_can_send_now_event(s_cid);
}

static err_t nap_linkoutput(struct netif *nif, struct pbuf *p) {
    (void)nif;
    if (!s_cid || !p || p->tot_len == 0 || p->tot_len > NAP_FRAME) {
        s_tx_drop++;
        return ERR_MEM;
    }
    if (s_tx_n >= NAP_TX_Q) {
        pbuf_free(s_txq[0]);
        memmove(&s_txq[0], &s_txq[1], (size_t)(NAP_TX_Q - 1) * sizeof s_txq[0]);
        s_tx_n--;
        s_txq[s_tx_n] = NULL;
        s_tx_drop++;
    }
    pbuf_ref(p);
    s_txq[s_tx_n++] = p;
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

static int write_cfg(void) {
    char body[128];
    snprintf(body, sizeof body,
             "{\"addr\":\"%s\",\"listen\":%s,\"forward\":%s,\"iface\":\"%s\"}\n",
             s_addr, s_listen ? "true" : "false",
             s_forward ? "true" : "false", s_fwd_iface);
    return cfg_file_write(NAP_CFG, body);
}

/* API handlers run on the main loop and write now. A BNEP event only marks
 * the file dirty: flash_safe_execute parks the other core, and that must not
 * run inside the radio poll that delivered the event. */
static void remember(void) {
    if (write_cfg()) { s_cfg_dirty = 0; }
    else { s_cfg_dirty = 1; }
}

static int iface_ok(const char *s) {
    return s && s[0] && s[1] && !s[2] && !(s[0] == 'b' && s[1] == '0');
}

static void load_cfg(void) {
    s_listen = 1;
    char buf[160];
    char iface[8];
    if (!cfg_file_read(NAP_CFG, buf, sizeof buf)) { return; }
    api_json_str(buf, "addr", s_addr, sizeof s_addr);
    bool listen = true;
    if (api_json_bool(buf, "listen", &listen)) { s_listen = listen ? 1 : 0; }
    bool fwd = false;
    if (api_json_bool(buf, "forward", &fwd)) { s_forward = fwd ? 1 : 0; }
    iface[0] = 0;
    api_json_str(buf, "iface", iface, sizeof iface);
    if (iface_ok(iface)) {
        s_fwd_iface[0] = iface[0];
        s_fwd_iface[1] = iface[1];
        s_fwd_iface[2] = 0;
    }
}

static void apply_listen(void) {
    gap_discoverable_control(s_listen ? 1 : 0);
    gap_connectable_control(s_listen ? 1 : 0);
    bt_log("info", "discoverable %s", s_listen ? "on" : "off");
}

static void stop_dhcp(void) {
    if (s_dhcp) {
        dhcp_stop(&s_nif);
        s_dhcp = 0;
    }
    if (s_srv) {
        dhcp_server_deinit(&s_dhcp_srv);
        s_srv = 0;
    }
}

static void nap_down(void) {
    tx_clear();
    s_tx_armed = 0;
    s_log_arp = s_log_dhcp = s_log_tcp = s_log_mac = 0;
    s_cid = 0;
    s_connecting = 0;
    s_handle = 0;
    s_outgoing = 0;
    s_role = ROLE_NONE;
    stop_dhcp();
    if (s_added) {
        netif_set_addr(&s_nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        netif_set_link_down(&s_nif);
        netif_set_down(&s_nif);
        mdns_remove_netif(&s_nif);
    }
    uplink_reselect();
}

static void fail_set(const char *why) {
    snprintf(s_fail, sizeof s_fail, "%s", why && why[0] ? why : "failed");
}

static void fail_keep(const char *why) {
    if (!s_fail[0]) { fail_set(why); }
}

/* Free a BNEP channel that L2CAP never reported, then drop our state.
 * bnep_disconnect crashes if the channel is already gone, so only call it
 * while we still believe one exists. drop_acl is for a link that is still up.
 * s_fail is left as the caller set it. */
static void abandon(int drop_acl) {
    hci_con_handle_t handle = s_handle;
    bd_addr_t peer;
    int have = s_addr[0] && sscanf_bd_addr(s_addr, peer);
    int live = s_connecting || s_cid;
    if (have && live) { bnep_disconnect(peer); }
    if (drop_acl && handle) { gap_disconnect(handle); }
    s_handle = 0;
    if (s_connecting || s_cid) { nap_down(); }
}

static int peer_is_ours(const bd_addr_t addr) {
    bd_addr_t peer;
    if (!s_addr[0] || !sscanf_bd_addr(s_addr, peer)) { return 0; }
    return memcmp(peer, addr, 6) == 0;
}

static void remember_handle(void) {
    bd_addr_t peer;
    hci_connection_t *c;
    if (!sscanf_bd_addr(s_addr, peer)) { return; }
    c = hci_connection_for_bd_addr_and_type(peer, BD_ADDR_TYPE_ACL);
    if (c) { s_handle = c->con_handle; }
}

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    bd_addr_t addr;
    uint8_t st;
    uint16_t handle;
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) { return; }
    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_CONNECTION_COMPLETE:
        hci_event_connection_complete_get_bd_addr(packet, addr);
        if (!peer_is_ours(addr)) { break; }
        st = hci_event_connection_complete_get_status(packet);
        if (st) {
            if (s_dial) {
                fail_set(bt_hci_reason(st));
                s_dial = 0;
            }
            if (!s_connecting) { break; }
            bt_log("warn", "BNEP connect failed %s (%s)", s_addr, bt_hci_reason(st));
            abandon(0);
            break;
        }
        s_handle = hci_event_connection_complete_get_connection_handle(packet);
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        handle = hci_event_disconnection_complete_get_connection_handle(packet);
        if (!s_handle || handle != s_handle) { break; }
        st = hci_event_disconnection_complete_get_reason(packet);
        /* L2CAP runs first and may already have cleared s_connecting.
         * s_dial still means this ACL was our attempt, so keep its reason. */
        if (!s_cid && s_dial) {
            fail_set(bt_hci_reason(st));
            s_dial = 0;
        }
        if (!s_connecting && !s_cid) {
            s_handle = 0;
            break;
        }
        bt_log("info", "BNEP link down %s (%s)", s_addr, bt_hci_reason(st));
        abandon(0);
        break;
    case GAP_EVENT_SECURITY_LEVEL:
        if (!s_connecting || !s_handle) { break; }
        if (gap_event_security_level_get_handle(packet) != s_handle) { break; }
        bt_log("info", "BNEP security %u slots %d reserved %d key %u",
               gap_event_security_level_get_security_level(packet),
               hci_number_free_acl_slots_for_handle(s_handle),
               hci_is_packet_buffer_reserved() ? 1 : 0,
               (unsigned)gap_encryption_key_size(s_handle));
        break;
    default:
        break;
    }
}

/* Board placed the call: take an address from the phone. */
static void start_panu(void) {
    stop_dhcp();
    netif_set_addr(&s_nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
    dhcp_start(&s_nif);
    s_dhcp = 1;
    s_role = ROLE_PANU;
}

/* The next offer uses the shared resolver when Share internet is on, and
 * this board's own address when it is off. A phone that already has a
 * lease keeps the old resolver until it asks again. */
static void apply_fwd_dns(void) {
    ip4_addr_t dns;
    if (nap_fwd_resolver(&dns)) {
        s_dhcp_srv.dns_set = true;
        ip4_addr_copy(s_dhcp_srv.dns, dns);
    } else {
        s_dhcp_srv.dns_set = false;
    }
}

/* Phone placed the call. The board's gateway stays unset, so uplink does
 * not move our default route onto b0. The offer names us as the phone's
 * router. DNS is the upstream resolver only while Share internet is on. */
static void start_nap_server(void) {
    stop_dhcp();
    ip4_addr_t ip, mask;
    IP4_ADDR(&ip, NAP_IP0, NAP_IP1, NAP_IP2, NAP_IP3);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    netif_set_addr(&s_nif, &ip, &mask, IP4_ADDR_ANY4);
    ip_addr_t sip, smask;
    ip_addr_copy_from_ip4(sip, ip);
    ip_addr_copy_from_ip4(smask, mask);
    dhcp_server_init(&s_dhcp_srv, &s_nif, &sip, &smask);
    apply_fwd_dns();
    s_srv = s_dhcp_srv.udp != NULL;
    s_role = ROLE_NAP;
    bt_log("info", "BNEP DHCP server %s%s", s_srv ? "192.168.44.1" : "failed",
           s_forward ? " forward on" : "");
}

/* A few lines only. A frame log on every packet would flood the console. */
static void note_rx(const uint8_t *frm, uint16_t size) {
    uint16_t et = (uint16_t)((frm[12] << 8) | frm[13]);
    if (!s_log_mac && (frm[0] & 1) == 0) {
        s_log_mac = 1;
        bt_log("info",
               "BNEP rx dst %02x:%02x:%02x:%02x:%02x:%02x hw %02x:%02x:%02x:%02x:%02x:%02x",
               frm[0], frm[1], frm[2], frm[3], frm[4], frm[5],
               s_nif.hwaddr[0], s_nif.hwaddr[1], s_nif.hwaddr[2],
               s_nif.hwaddr[3], s_nif.hwaddr[4], s_nif.hwaddr[5]);
    }
    if (et == 0x0806) {
        if (s_log_arp < 4) {
            s_log_arp++;
            bt_log("info", "BNEP arp %u bytes", size);
        }
        return;
    }
    if (et != 0x0800 || size < 34) { return; }
    unsigned ihl = (unsigned)(frm[14] & 0x0f) * 4u;
    unsigned off = 14u + ihl;
    if (off + 4u > size) { return; }
    uint8_t proto = frm[23];
    uint16_t dport = (uint16_t)((frm[off + 2] << 8) | frm[off + 3]);
    if (proto == 17 && dport == 67 && s_log_dhcp < 6) {
        s_log_dhcp++;
        bt_log("info", "BNEP dhcp from %u.%u.%u.%u", frm[26], frm[27], frm[28], frm[29]);
    } else if (proto == 6 && dport == 80 && s_log_tcp < 6) {
        s_log_tcp++;
        bt_log("info", "BNEP tcp %u.%u.%u.%u -> :80", frm[26], frm[27], frm[28], frm[29]);
    }
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
                uint8_t st = bnep_event_channel_opened_get_status(packet);
                bt_log("warn", "BNEP open failed %s (0x%02x)", bt_hci_reason(st), st);
                s_outgoing = 0;
                if (s_dial) { fail_keep(bt_hci_reason(st)); }
                break;
            }
            s_dial = 0;
            s_fail[0] = 0;
            s_cid = bnep_event_channel_opened_get_bnep_cid(packet);
            {
                bd_addr_t local;
                gap_local_bd_addr(local);
                memcpy(s_nif.hwaddr, local, 6);
                bd_addr_t peer;
                bnep_event_channel_opened_get_remote_address(packet, peer);
                snprintf(s_addr, sizeof s_addr, "%s", bd_addr_to_str(peer));
                s_cfg_dirty = 1;
            }
            {
                uint16_t src = bnep_event_channel_opened_get_source_uuid(packet);
                uint16_t dst = bnep_event_channel_opened_get_destination_uuid(packet);
                /* Outgoing stores our role as the source. Incoming stores
                 * the service they asked for as the destination. */
                int panu = s_outgoing ? src == BLUETOOTH_SERVICE_CLASS_PANU
                                      : dst == BLUETOOTH_SERVICE_CLASS_PANU;
                s_outgoing = 0;
                remember_handle();
                netif_set_up(&s_nif);
                netif_set_link_up(&s_nif);
                if (panu) { start_panu(); }
                else { start_nap_server(); }
                bt_log("info", "BNEP up %s %s cid %u mtu %u",
                       panu ? "PANU" : "NAP", s_addr, s_cid,
                       bnep_event_channel_opened_get_mtu(packet));
            }
            mdns_add_netif(&s_nif);
            uplink_reselect();
            break;
        case BNEP_EVENT_CHANNEL_CLOSED:
        case BNEP_EVENT_CHANNEL_TIMEOUT:
            bt_log("info", "BNEP down (%s)",
                   hci_event_packet_get_type(packet) == BNEP_EVENT_CHANNEL_TIMEOUT
                       ? "timeout" : "closed");
            if (!s_cid && s_dial && !s_fail[0]) {
                fail_set(hci_event_packet_get_type(packet) == BNEP_EVENT_CHANNEL_TIMEOUT
                             ? "timed out" : "link dropped");
            }
            if (!s_cid) { s_dial = 0; }
            nap_down();
            break;
        case BNEP_EVENT_CAN_SEND_NOW:
            s_tx_armed = 0;
            if (s_tx_n > 0 && s_cid) {
                struct pbuf *p = s_txq[0];
                memmove(&s_txq[0], &s_txq[1], (size_t)(s_tx_n - 1) * sizeof s_txq[0]);
                s_tx_n--;
                s_txq[s_tx_n] = NULL;
                uint16_t n = p->tot_len;
                if (n > NAP_FRAME) { n = NAP_FRAME; }
                pbuf_copy_partial(p, s_frame, n, 0);
                pbuf_free(p);
                bnep_send(s_cid, s_frame, n);
                s_tx_frames++;
            }
            break;
        default:
            break;
        }
        break;
    case BNEP_DATA_PACKET:
        if (!s_cid || !s_added || size < 14 || size > NAP_FRAME) { break; }
        s_rx_frames++;
        note_rx(packet, size);
        {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
            if (!p) { break; }
            pbuf_take(p, packet, size);
            /* Same side door as USB-NCM: answer DHCP from the frame itself,
             * then let lwIP see it too. */
            if (s_srv) { dhcp_try_eth_input(&s_dhcp_srv, p); }
            if (s_role == ROLE_NAP && nap_fwd_out(p)) {
                pbuf_free(p);
            } else if (s_nif.input(p, &s_nif) != ERR_OK) {
                pbuf_free(p);
            }
        }
        break;
    default:
        break;
    }
    s_in--;
    if (!s_in) { arm_tx(); }
}

static void register_one(uint16_t uuid, const char *name) {
    uint8_t err = bnep_register_service(packet_handler, uuid, NAP_FRAME);
    if (err) { bt_log("warn", "BNEP %s service failed %u", name, err); }
    else { bt_log("info", "BNEP %s service registered", name); }
}

/* The second register hits the same L2CAP PSM. BTstack refuses that PSM
 * again and still keeps the second BNEP service. Do not unregister one:
 * bnep_unregister_service drops the PSM for both. */
static void register_services(void) {
    bnep_set_required_security_level(LEVEL_2);
    register_one(BLUETOOTH_SERVICE_CLASS_PANU, "PANU");
    register_one(BLUETOOTH_SERVICE_CLASS_NAP, "NAP");
}

/* Called from the logged SDP server copy after it builds the reply.
 * A phone that never asks produces none of these lines. */
static void sdp_pattern(uint8_t *packet, char *out, size_t cap) {
    uint16_t param_len = (uint16_t)big_endian_read_16(packet, 3);
    uint8_t *pat = packet + 5;
    uint32_t plen = de_get_len_safe(pat, param_len);
    des_iterator_t it;
    size_t used = 0;
    int n = 0;
    out[0] = 0;
    if (plen == 0 || de_get_element_type(pat) != DE_DES || !des_iterator_init(&it, pat)) {
        snprintf(out, cap, "unreadable");
        return;
    }
    while (des_iterator_has_more(&it) && n < 4) {
        uint8_t *el = des_iterator_get_element(&it);
        if (de_get_element_type(el) == DE_UUID) {
            uint32_t uuid = de_get_uuid32(el);
            const char *name = "";
            if (uuid == BLUETOOTH_SERVICE_CLASS_NAP) { name = " NAP"; }
            else if (uuid == BLUETOOTH_SERVICE_CLASS_PANU) { name = " PANU"; }
            else if (uuid == BLUETOOTH_SERVICE_CLASS_GN) { name = " GN"; }
            int w = snprintf(out + used, cap - used, "%s0x%04lx%s",
                             n ? " " : "", (unsigned long)uuid, name);
            if (w < 0 || (size_t)w >= cap - used) { break; }
            used += (size_t)w;
            n++;
        }
        des_iterator_next(&it);
    }
    if (n == 0) { snprintf(out, cap, "no uuid"); }
}

void bt_sdp_note_search(uint8_t *packet, int reply) {
    char uuids[64];
    sdp_pattern(packet, uuids, sizeof uuids);
    bt_log("info", "SDP search %s, reply %d", uuids, reply);
}

void bt_sdp_note_attr(uint8_t *packet, int reply) {
    uint16_t param_len = (uint16_t)big_endian_read_16(packet, 3);
    uint32_t handle = param_len >= 6 ? (uint32_t)big_endian_read_32(packet, 5) : 0;
    bt_log("info", "SDP attr handle 0x%08lx, reply %d", (unsigned long)handle, reply);
}

void bt_sdp_note_search_attr(uint8_t *packet, int reply) {
    char uuids[64];
    sdp_pattern(packet, uuids, sizeof uuids);
    bt_log("info", "SDP search-attr %s, reply %d", uuids, reply);
}

static void register_sdp(uint8_t *rec, size_t cap, int nap) {
    memset(rec, 0, cap);
    if (nap) {
        /* A type of none and a rate of 0 means no network, and Android
         * disconnects before opening BNEP. Working NAP examples advertise
         * security none. The channel still requires level 2. */
        pan_create_nap_sdp_record(rec, sdp_create_service_record_handle(),
                                  s_net_types, NULL, NULL,
                                  BNEP_SECURITY_NONE,
                                  PAN_NET_ACCESS_TYPE_OTHER, 1000000, NULL, NULL);
    } else {
        pan_create_panu_sdp_record(rec, sdp_create_service_record_handle(),
                                   s_net_types, NULL, NULL,
                                   BNEP_SECURITY_SERVICE_LEVEL_ENFORCED);
    }
    uint32_t n = de_get_len(rec);
    if (n == 0 || n > cap) {
        bt_log("error", "BNEP SDP record %u does not fit", (unsigned)n);
        return;
    }
    uint8_t err = sdp_register_service(rec);
    if (err) {
        bt_log("warn", "BNEP SDP %s failed %u (%u bytes)", nap ? "NAP" : "PANU", err, (unsigned)n);
    } else {
        bt_log("info", "BNEP SDP %s registered (%u bytes)", nap ? "NAP" : "PANU", (unsigned)n);
    }
}

static void reply_status(api_reply_fn reply, void *ctx, long id) {
    const char *st = s_cid ? "up" : s_connecting ? "connecting" : "down";
    const char *role = s_role == ROLE_PANU ? "panu" : s_role == ROLE_NAP ? "nap" : "";
    char ip[16] = "0.0.0.0", mask[16] = "0.0.0.0", gw[16] = "0.0.0.0";
    if (s_added) {
        ip4addr_ntoa_r(netif_ip4_addr(&s_nif), ip, sizeof ip);
        ip4addr_ntoa_r(netif_ip4_netmask(&s_nif), mask, sizeof mask);
        ip4addr_ntoa_r(netif_ip4_gw(&s_nif), gw, sizeof gw);
    }
    char fail[96];
    api_json_escape(fail, sizeof fail, s_fail);
    uint32_t ftx = 0, frx = 0, fdrop = 0;
    nap_fwd_counts(&ftx, &frx, &fdrop);
    char *b = api_scratch();
    snprintf(b, api_scratch_size(),
             "{\"state\":\"%s\",\"addr\":\"%s\",\"ip\":\"%s\",\"mask\":\"%s\","
             "\"gw\":\"%s\",\"listen\":%s,\"role\":\"%s\",\"fail\":\"%s\","
             "\"rx\":%lu,\"tx\":%lu,\"tx_drop\":%lu,"
             "\"forward\":%s,\"forward_iface\":\"%s\",\"forward_why\":\"%s\","
             "\"fwd\":%lu,\"fwd_rx\":%lu,\"fwd_drop\":%lu}",
             st, s_addr, ip, mask, gw, s_listen ? "true" : "false", role, fail,
             (unsigned long)s_rx_frames, (unsigned long)s_tx_frames,
             (unsigned long)s_tx_drop,
             s_forward ? "true" : "false", s_fwd_iface, nap_fwd_why(),
             (unsigned long)ftx, (unsigned long)frx, (unsigned long)fdrop);
    api_reply_ok(reply, ctx, id, b);
}

void nap_init(void) {
    load_cfg();
    bnep_init();
    if (!s_added) {
        if (!netif_add(&s_nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4,
                       NULL, nap_netif_init, ethernet_input)) {
            printf("bt: NAP netif_add failed\n");
            return;
        }
        s_added = 1;
    }
    s_hci.callback = &hci_handler;
    hci_add_event_handler(&s_hci);
    register_services();
    register_sdp(s_panu_sdp, sizeof s_panu_sdp, 0);
    register_sdp(s_nap_sdp, sizeof s_nap_sdp, 1);
    /* The pointer is kept. The hostname is a string literal. */
    gap_set_local_name(CONFIG_NET_HOSTNAME);
    /* Networking service class, major class LAN. */
    gap_set_class_of_device(0x020300);
    /* Linux allows a role switch on the call it places. The phone becomes
     * master. Sniff stays available. */
    gap_set_allow_role_switch(true);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH |
                                         LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    /* Registering the PSM turns page scan on. Apply the saved choice after. */
    apply_listen();
    nap_fwd_set(s_forward, s_fwd_iface, &s_nif);
    bt_log("info", "BNEP ready");
}

void nap_tick(void) {
    nap_fwd_tick();
    if (s_cfg_dirty && write_cfg()) { s_cfg_dirty = 0; }
    /* The BNEP timer does not start until L2CAP opens. A connect that
     * never gets that far would otherwise sit on "connecting" with no
     * line at all. */
    if (!s_connecting) { return; }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((uint32_t)(now - s_connect_ms) >= NAP_CONNECT_TIMEOUT_MS) {
        bt_log("warn", "BNEP connect to %s timed out, dropping the link", s_addr);
        fail_set("timed out");
        s_dial = 0;
        abandon(1);
        return;
    }
    if (s_connect_log_ms != 0 && (uint32_t)(now - s_connect_log_ms) < 5000) { return; }
    s_connect_log_ms = now;
    bt_log("warn", "BNEP still connecting to %s, %lus, no channel, slots %d reserved %d sec %u key %u",
           s_addr, (unsigned long)((now - s_connect_ms) / 1000),
           s_handle ? hci_number_free_acl_slots_for_handle(s_handle) : -1,
           hci_is_packet_buffer_reserved() ? 1 : 0,
           s_handle ? (unsigned)gap_security_level(s_handle) : 0,
           s_handle ? (unsigned)gap_encryption_key_size(s_handle) : 0);
}

struct netif *nap_uplink_netif(void) {
    if (!s_cid || !netif_is_up(&s_nif) || !netif_is_link_up(&s_nif)) { return NULL; }
    return &s_nif;
}

void op_classic_nap(const char *params, api_reply_fn reply, void *ctx, long id) {
    bool listen = false;
    int has_listen = params && api_json_bool(params, "listen", &listen);
    char addr[24];
    addr[0] = 0;
    if (params) { api_json_str(params, "addr", addr, sizeof addr); }
    if (has_listen) {
        s_listen = listen ? 1 : 0;
        apply_listen();
        remember();
    }
    bool share = false;
    int has_share = params && api_json_bool(params, "forward", &share);
    char iface[8];
    iface[0] = 0;
    if (params) { api_json_str(params, "iface", iface, sizeof iface); }
    if (has_share || iface_ok(iface)) {
        if (has_share) { s_forward = share ? 1 : 0; }
        if (iface_ok(iface)) {
            s_fwd_iface[0] = iface[0];
            s_fwd_iface[1] = iface[1];
            s_fwd_iface[2] = 0;
        }
        nap_fwd_set(s_forward, s_fwd_iface, s_added ? &s_nif : NULL);
        apply_fwd_dns();
        remember();
        bt_log("info", "BNEP forward %s via %s", s_forward ? "on" : "off", s_fwd_iface);
    }
    if (!addr[0]) {
        reply_status(reply, ctx, id);
        return;
    }
    if (!s_added) {
        api_reply_err(reply, ctx, id, "failed", "NAP netif is not up");
        return;
    }
    if (s_cid || s_connecting) {
        api_reply_err(reply, ctx, id, "busy", "a NAP link is up");
        return;
    }
    bd_addr_t peer;
    if (!sscanf_bd_addr(addr, peer)) {
        api_reply_err(reply, ctx, id, "bad_request", "addr is AA:BB:CC:DD:EE:FF");
        return;
    }
    snprintf(s_addr, sizeof s_addr, "%s", addr);
    remember();
    /* Drop the previous attempt's handle so its late ACL event cannot
     * abandon this dial. Clearing the failure text is the new attempt. */
    s_handle = 0;
    s_outgoing = 1;
    s_dial = 1;
    s_fail[0] = 0;
    bt_log("info", "BNEP pool slots %u",
           (unsigned)hci_number_free_acl_slots_for_connection_type(BD_ADDR_TYPE_ACL));
    int err = bnep_connect(packet_handler, peer, BLUETOOTH_PSM_BNEP,
                           BLUETOOTH_SERVICE_CLASS_PANU, BLUETOOTH_SERVICE_CLASS_NAP);
    if (err) {
        s_outgoing = 0;
        s_dial = 0;
        fail_set("did not start");
        bt_log("warn", "dial %s did not start (%d)", s_addr, err);
        api_reply_err(reply, ctx, id, "failed", "BNEP connect did not start");
        return;
    }
    s_connecting = 1;
    s_connect_ms = to_ms_since_boot(get_absolute_time());
    s_connect_log_ms = s_connect_ms;
    bt_log("info", "dial %s as PANU, gap security %u, discoverable %s",
           s_addr, (unsigned)gap_get_security_level(), s_listen ? "on" : "off");
    api_reply_ok(reply, ctx, id, "{\"pending\":true}");
}
