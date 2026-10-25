/* Classic SPP, the pm220-pico2w shape: inquiry, SDP for the Serial Port
 * class, RFCOMM channel 1 when SDP has no channel, and BATTERY?\r\n as the
 * first payload. LE central is a different handler. Nothing runs until
 * classic.scan or classic.connect. */
#include "bt/classic/spp.h"

#include <stdio.h>
#include <string.h>

#include "api/ops.h"
#include "bt/bt.h"
#include "btstack.h"

#define SPP_FALLBACK_CHANNEL 1
#define INQUIRY_LEN 8
#define SCAN_MAX 8

enum { ST_IDLE = 0, ST_SCANNING, ST_CONNECTING, ST_CONNECTED };

typedef struct {
    bd_addr_t addr;
    char name[32];
    int8_t rssi;
    uint32_t cod;
} scan_dev_t;

static btstack_packet_callback_registration_t s_hci;
static btstack_context_callback_registration_t s_sdp_reg;
static scan_dev_t s_scan[SCAN_MAX];
static int s_scan_n;
static bd_addr_t s_peer;
static int s_have_peer;
static int s_sdp_after_scan;
static uint8_t s_channel;
static uint16_t s_cid, s_mtu;
static uint8_t s_st;
static uint8_t s_pin_alt;
static int s_battery_pending;
static char s_last_rx[80];
static char s_error[64];

static const char *state_name(void) {
    switch (s_st) {
    case ST_SCANNING: return "scanning";
    case ST_CONNECTING: return "connecting";
    case ST_CONNECTED: return "connected";
    default: return "idle";
    }
}

static void set_error(const char *msg) {
    snprintf(s_error, sizeof s_error, "%s", msg ? msg : "");
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void open_rfcomm(uint8_t ch) {
    printf("bt: RFCOMM connect %s channel %u\n", bd_addr_to_str(s_peer), ch);
    uint8_t err = rfcomm_create_channel(packet_handler, s_peer, ch, &s_cid);
    if (err) {
        char msg[64];
        snprintf(msg, sizeof msg, "RFCOMM create failed 0x%02x", err);
        set_error(msg);
        s_st = ST_IDLE;
        s_cid = 0;
    }
}

static void start_sdp_query(void *context) {
    (void)context;
    s_channel = 0;
    uint8_t err = sdp_client_query_rfcomm_channel_and_name_for_uuid(
        packet_handler, s_peer, BLUETOOTH_SERVICE_CLASS_SERIAL_PORT);
    if (err) {
        char msg[64];
        snprintf(msg, sizeof msg, "SDP query start failed 0x%02x", err);
        set_error(msg);
        s_st = ST_IDLE;
    } else {
        printf("bt: SDP query for SPP on %s\n", bd_addr_to_str(s_peer));
    }
}

static void begin_sdp(void) {
    s_st = ST_CONNECTING;
    s_sdp_reg.callback = &start_sdp_query;
    sdp_client_register_query_callback(&s_sdp_reg);
}

static void note_scan(const uint8_t *packet) {
    bd_addr_t addr;
    gap_event_inquiry_result_get_bd_addr(packet, addr);
    for (int i = 0; i < s_scan_n; i++) {
        if (memcmp(s_scan[i].addr, addr, 6) != 0) { continue; }
        if (gap_event_inquiry_result_get_name_available(packet) && !s_scan[i].name[0]) {
            int n = gap_event_inquiry_result_get_name_len(packet);
            if (n > (int)sizeof s_scan[i].name - 1) { n = (int)sizeof s_scan[i].name - 1; }
            memcpy(s_scan[i].name, gap_event_inquiry_result_get_name(packet), (size_t)n);
            s_scan[i].name[n] = 0;
        }
        return;
    }
    if (s_scan_n >= SCAN_MAX) { return; }
    scan_dev_t *d = &s_scan[s_scan_n++];
    memset(d, 0, sizeof *d);
    memcpy(d->addr, addr, 6);
    d->cod = gap_event_inquiry_result_get_class_of_device(packet);
    d->rssi = gap_event_inquiry_result_get_rssi_available(packet)
                  ? gap_event_inquiry_result_get_rssi(packet) : 0;
    if (gap_event_inquiry_result_get_name_available(packet)) {
        int n = gap_event_inquiry_result_get_name_len(packet);
        if (n > (int)sizeof d->name - 1) { n = (int)sizeof d->name - 1; }
        memcpy(d->name, gap_event_inquiry_result_get_name(packet), (size_t)n);
        d->name[n] = 0;
    }
    printf("bt: inquiry %s COD %06lx rssi %d %s\n", bd_addr_to_str(addr),
           (unsigned long)d->cod, d->rssi, d->name);
}

static void handle_sdp(uint8_t *packet) {
    switch (hci_event_packet_get_type(packet)) {
    case SDP_EVENT_QUERY_RFCOMM_SERVICE: {
        uint8_t ch = sdp_event_query_rfcomm_service_get_rfcomm_channel(packet);
        const char *name = sdp_event_query_rfcomm_service_get_name(packet);
        printf("bt: SDP service '%s' channel %u\n", name ? name : "", ch);
        if (ch && (s_channel == 0 || ch == SPP_FALLBACK_CHANNEL)) { s_channel = ch; }
        break;
    }
    case SDP_EVENT_QUERY_COMPLETE: {
        uint8_t status = sdp_event_query_complete_get_status(packet);
        if (status) {
            char msg[64];
            snprintf(msg, sizeof msg, "SDP query failed 0x%02x, trying channel %d",
                     status, SPP_FALLBACK_CHANNEL);
            set_error(msg);
            s_channel = SPP_FALLBACK_CHANNEL;
        }
        if (s_channel == 0) {
            printf("bt: no SPP in SDP, falling back to channel %d\n", SPP_FALLBACK_CHANNEL);
            s_channel = SPP_FALLBACK_CHANNEL;
        }
        if (s_st == ST_CONNECTING) { open_rfcomm(s_channel); }
        break;
    }
    default:
        break;
    }
}

/* ACL and pairing, one line each. These events are marked handled, so they
 * never become bt.unhandled, and until now they only reached RTT. */
static const char *io_cap_name(uint8_t cap) {
    switch (cap) {
    case 0: return "display only";
    case 1: return "display yes/no";
    case 2: return "keyboard";
    case 3: return "no input/output";
    default: return "unknown";
    }
}

static const char *key_type_name(uint8_t t) {
    switch (t) {
    case 0: return "combination";
    case 3: return "debug";
    case 4: return "unauthenticated";
    case 5: return "authenticated";
    case 6: return "changed";
    default: return "other";
    }
}

static const char *link_type_name(uint8_t t) {
    return t == 1 ? "acl" : t == 0 ? "sco" : "other";
}

static const char *mode_name(uint8_t m) {
    switch (m) {
    case 0: return "active";
    case 1: return "hold";
    case 2: return "sniff";
    case 3: return "park";
    default: return "other";
    }
}

static void log_link_event(uint8_t event, const uint8_t *packet, uint16_t size) {
    bd_addr_t addr;
    uint8_t st;
    switch (event) {
    case HCI_EVENT_CONNECTION_REQUEST:
        hci_event_connection_request_get_bd_addr(packet, addr);
        bt_log("info", "page from %s cod 0x%06lx %s",
               bd_addr_to_str(addr),
               (unsigned long)hci_event_connection_request_get_class_of_device(packet),
               link_type_name(hci_event_connection_request_get_link_type(packet)));
        break;
    case HCI_EVENT_CONNECTION_COMPLETE:
        st = hci_event_connection_complete_get_status(packet);
        hci_event_connection_complete_get_bd_addr(packet, addr);
        bt_log(st ? "warn" : "info", "acl %s %s (0x%02x) handle %u %s encrypt %u",
               bd_addr_to_str(addr), bt_hci_reason(st), st,
               hci_event_connection_complete_get_connection_handle(packet),
               link_type_name(hci_event_connection_complete_get_link_type(packet)),
               hci_event_connection_complete_get_encryption_enabled(packet));
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        st = hci_event_disconnection_complete_get_reason(packet);
        bt_log("info", "acl down handle %u %s (0x%02x)",
               hci_event_disconnection_complete_get_connection_handle(packet),
               bt_hci_reason(st), st);
        break;
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        st = hci_event_authentication_complete_get_status(packet);
        bt_log(st ? "warn" : "info", "authentication handle %u %s (0x%02x)",
               hci_event_authentication_complete_get_connection_handle(packet),
               bt_hci_reason(st), st);
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE:
        st = hci_event_encryption_change_get_status(packet);
        bt_log(st ? "warn" : "info", "encryption handle %u %s (0x%02x) enabled %u",
               hci_event_encryption_change_get_connection_handle(packet),
               bt_hci_reason(st), st,
               hci_event_encryption_change_get_encryption_enabled(packet));
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE_V2:
        st = hci_event_encryption_change_v2_get_status(packet);
        bt_log(st ? "warn" : "info", "encryption v2 handle %u %s (0x%02x) enabled %u key %u",
               hci_event_encryption_change_v2_get_connection_handle(packet),
               bt_hci_reason(st), st,
               hci_event_encryption_change_v2_get_encryption_enabled(packet),
               hci_event_encryption_change_v2_get_encryption_key_size(packet));
        break;
    case HCI_EVENT_ENCRYPTION_KEY_REFRESH_COMPLETE:
        st = hci_event_encryption_key_refresh_complete_get_status(packet);
        bt_log(st ? "warn" : "info", "encryption refresh handle %u %s (0x%02x)",
               hci_event_encryption_key_refresh_complete_get_handle(packet),
               bt_hci_reason(st), st);
        break;
    case HCI_EVENT_LINK_KEY_REQUEST:
        hci_event_link_key_request_get_bd_addr(packet, addr);
        bt_log("info", "link key requested %s", bd_addr_to_str(addr));
        break;
    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        if (size < 25) { break; }
        hci_event_link_key_request_get_bd_addr(packet, addr);
        {
            int null_key = 1;
            for (int i = 0; i < 16; i++) { if (packet[8 + i]) { null_key = 0; } }
            if (null_key) {
                bt_log("warn", "null link key from %s, not stored", bd_addr_to_str(addr));
            } else {
                bt_log("info", "link key from %s type %s (%u)",
                       bd_addr_to_str(addr), key_type_name(packet[24]), packet[24]);
            }
        }
        break;
    case HCI_EVENT_IO_CAPABILITY_REQUEST:
        hci_event_io_capability_request_get_bd_addr(packet, addr);
        bt_log("info", "SSP io capability request %s", bd_addr_to_str(addr));
        break;
    case HCI_EVENT_IO_CAPABILITY_RESPONSE:
        hci_event_io_capability_response_get_bd_addr(packet, addr);
        bt_log("info", "peer io %s auth 0x%02x %s",
               io_cap_name(hci_event_io_capability_response_get_io_capability(packet)),
               hci_event_io_capability_response_get_authentication_requirements(packet),
               bd_addr_to_str(addr));
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        bt_log("info", "SSP confirm %s, accepting", bd_addr_to_str(addr));
        break;
    case HCI_EVENT_USER_PASSKEY_REQUEST:
        hci_event_user_passkey_request_get_bd_addr(packet, addr);
        bt_log("warn", "passkey requested %s", bd_addr_to_str(addr));
        break;
    case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
        st = hci_event_simple_pairing_complete_get_status(packet);
        hci_event_simple_pairing_complete_get_bd_addr(packet, addr);
        bt_log(st ? "warn" : "info", "pairing complete %s %s (0x%02x)",
               bd_addr_to_str(addr), bt_hci_reason(st), st);
        break;
    case HCI_EVENT_ROLE_CHANGE:
        st = hci_event_role_change_get_status(packet);
        hci_event_role_change_get_bd_addr(packet, addr);
        bt_log(st ? "warn" : "info", "role %s %s (0x%02x) local %s",
               bd_addr_to_str(addr), bt_hci_reason(st), st,
               hci_event_role_change_get_role(packet) ? "slave" : "master");
        break;
    case HCI_EVENT_MODE_CHANGE:
        st = hci_event_mode_change_get_status(packet);
        bt_log(st ? "warn" : "info", "mode handle %u %s interval %u %s (0x%02x)",
               hci_event_mode_change_get_handle(packet),
               mode_name(hci_event_mode_change_get_mode(packet)),
               hci_event_mode_change_get_interval(packet),
               bt_hci_reason(st), st);
        break;
    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
        st = hci_event_remote_name_request_complete_get_status(packet);
        hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
        if (st) {
            bt_log("warn", "remote name %s %s (0x%02x)",
                   bd_addr_to_str(addr), bt_hci_reason(st), st);
        } else {
            char name[33];
            const char *n = hci_event_remote_name_request_complete_get_remote_name(packet);
            size_t i = 0;
            for (; i < sizeof name - 1 && n[i] >= 32 && n[i] < 127; i++) { name[i] = n[i]; }
            name[i] = 0;
            bt_log("info", "remote name %s \"%s\"", bd_addr_to_str(addr), name);
        }
        break;
    case HCI_EVENT_COMMAND_STATUS:
        st = hci_event_command_status_get_status(packet);
        if (st) {
            bt_log("warn", "HCI command 0x%04x %s (0x%02x)",
                   hci_event_command_status_get_command_opcode(packet),
                   bt_hci_reason(st), st);
        }
        break;
    default:
        break;
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type == RFCOMM_DATA_PACKET) {
        if (!size) { return; }
        size_t n = size < sizeof s_last_rx - 1 ? size : sizeof s_last_rx - 1;
        memcpy(s_last_rx, packet, n);
        s_last_rx[n] = 0;
        printf("bt: RFCOMM rx %u\n", (unsigned)size);
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) { return; }
    uint8_t event = hci_event_packet_get_type(packet);
    log_link_event(event, packet, size);
    if (event == SDP_EVENT_QUERY_RFCOMM_SERVICE || event == SDP_EVENT_QUERY_COMPLETE) {
        handle_sdp(packet);
        return;
    }
    switch (event) {
    case GAP_EVENT_INQUIRY_RESULT:
        if (s_st == ST_SCANNING) { note_scan(packet); }
        break;
    case GAP_EVENT_INQUIRY_COMPLETE:
        bt_log("info", "inquiry complete, %d device(s)", s_scan_n);
        if (s_st == ST_SCANNING) { s_st = ST_IDLE; }
        if (s_sdp_after_scan) {
            s_sdp_after_scan = 0;
            begin_sdp();
        }
        break;
    case HCI_EVENT_PIN_CODE_REQUEST: {
        bd_addr_t addr;
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        const char *pin = s_pin_alt ? "1234" : "0000";
        s_pin_alt = 1;
        bt_log("info", "legacy PIN %s answering %s", bd_addr_to_str(addr), pin);
        gap_pin_code_response(addr, pin);
        break;
    }
    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        bd_addr_t addr;
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        gap_ssp_confirmation_response(addr);
        break;
    }
    case RFCOMM_EVENT_CHANNEL_OPENED: {
        uint8_t status = rfcomm_event_channel_opened_get_status(packet);
        if (status == ERROR_CODE_SUCCESS) {
            s_cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
            s_mtu = rfcomm_event_channel_opened_get_max_frame_size(packet);
            s_st = ST_CONNECTED;
            s_error[0] = 0;
            s_battery_pending = 1;
            printf("bt: RFCOMM open cid %u mtu %u %s\n", s_cid, s_mtu, bd_addr_to_str(s_peer));
            rfcomm_request_can_send_now_event(s_cid);
        } else {
            char msg[64];
            snprintf(msg, sizeof msg, "RFCOMM open failed 0x%02x", status);
            set_error(msg);
            s_st = ST_IDLE;
            s_cid = 0;
        }
        break;
    }
    case RFCOMM_EVENT_CAN_SEND_NOW:
        if (s_battery_pending && s_cid) {
            s_battery_pending = 0;
            const char *q = "BATTERY?\r\n";
            printf("bt: send BATTERY?\n");
            rfcomm_send(s_cid, (uint8_t *)q, (uint16_t)strlen(q));
        }
        break;
    case RFCOMM_EVENT_CHANNEL_CLOSED:
        printf("bt: RFCOMM closed\n");
        s_cid = 0;
        s_mtu = 0;
        s_battery_pending = 0;
        if (s_st == ST_CONNECTED || s_st == ST_CONNECTING) { s_st = ST_IDLE; }
        break;
    default:
        break;
    }
}

void classic_init(void) {
    rfcomm_init();
    sdp_init();
    sdp_client_init();
    s_hci.callback = &packet_handler;
    hci_add_event_handler(&s_hci);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(
        SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(1);
    printf("bt: Classic RFCOMM ready (inquiry is classic.scan)\n");
}

void op_classic_scan(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    if (s_st == ST_CONNECTING || s_st == ST_CONNECTED) {
        api_reply_err(reply, ctx, id, "busy", "a Classic link is up");
        return;
    }
    s_scan_n = 0;
    s_st = ST_SCANNING;
    s_error[0] = 0;
    int err = gap_inquiry_start(INQUIRY_LEN);
    if (err) {
        s_st = ST_IDLE;
        api_reply_err(reply, ctx, id, "failed", "inquiry did not start");
        return;
    }
    api_reply_ok(reply, ctx, id, "{\"started\":true}");
}

void op_classic_connect(const char *params, api_reply_fn reply, void *ctx, long id) {
    bd_addr_t target;
    char addr[24];
    addr[0] = 0;
    if (params) { api_json_str(params, "addr", addr, sizeof addr); }
    if (addr[0]) {
        if (!sscanf_bd_addr(addr, target)) {
            api_reply_err(reply, ctx, id, "bad_request", "addr is AA:BB:CC:DD:EE:FF");
            return;
        }
    } else if (s_scan_n > 0) {
        memcpy(target, s_scan[0].addr, 6);
    } else if (s_have_peer) {
        memcpy(target, s_peer, 6);
    } else {
        api_reply_err(reply, ctx, id, "bad_request", "scan first or pass addr");
        return;
    }
    memcpy(s_peer, target, 6);
    s_have_peer = 1;
    s_pin_alt = 0;
    s_last_rx[0] = 0;
    s_error[0] = 0;
    if (s_st == ST_SCANNING) {
        s_sdp_after_scan = 1;
        gap_inquiry_stop();
        api_reply_ok(reply, ctx, id, "{\"pending\":true}");
        return;
    }
    begin_sdp();
    api_reply_ok(reply, ctx, id, "{\"pending\":true}");
}

void op_classic_state(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char rx[160];
    char err[128];
    api_json_escape(rx, sizeof rx, s_last_rx);
    api_json_escape(err, sizeof err, s_error);
    char *b = api_scratch();
    size_t cap = api_scratch_size();
    int n = snprintf(b, cap,
                     "{\"state\":\"%s\",\"address\":\"%s\",\"channel\":%u,\"mtu\":%u,"
                     "\"last_rx\":\"%s\",\"error\":\"%s\",\"devices\":[",
                     state_name(), s_have_peer ? bd_addr_to_str(s_peer) : "",
                     s_channel, s_mtu, rx, err);
    if (n < 0 || (size_t)n >= cap) {
        api_reply_err(reply, ctx, id, "too_large", "state");
        return;
    }
    size_t used = (size_t)n;
    for (int i = 0; i < s_scan_n && used + 96 < cap; i++) {
        char name[64];
        api_json_escape(name, sizeof name, s_scan[i].name);
        int m = snprintf(b + used, cap - used,
                         "%s{\"address\":\"%s\",\"name\":\"%s\",\"rssi\":%d}",
                         i ? "," : "", bd_addr_to_str(s_scan[i].addr), name, s_scan[i].rssi);
        if (m < 0) { break; }
        used += (size_t)m;
    }
    snprintf(b + used, cap - used, "]}");
    api_reply_ok(reply, ctx, id, b);
}
