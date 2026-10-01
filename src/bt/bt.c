/*
 * Bluetooth bring-up and the transport-neutral half of the stack.
 *
 * Deliberately says nothing about LE: the device registry is keyed by
 * (transport, address) so Classic can be added later without reshaping the
 * data model, the JSON schema or the Lua API. Only src/bt/le/ knows about GATT.
 */
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "pico/cyw43_arch.h"

#include "bt/bt.h"
#include "api/ops.h"
#include "bt/registry.h"
#include "bt/le/central.h"
#include "bt/le/gattdb.h"
#include "bt/le/bond.h"
#include "fs/fs.h"
#include "autoconf.h"
#if CONFIG_NET_WIFI
#include "net/wifi.h"   /* wifi_bt_hold(): keep the BT controller off the bus during WiFi's boot join */
#endif
#include "bt/le/gattserver.h"
#include "net/ws.h"
#include "autoconf.h"

static btstack_packet_callback_registration_t hci_cb;
static bool s_ready;
/* Whether bt_init() ran, i.e. whether BTstack exists at all.
 *
 * NOT the same as s_ready, which means the controller reached HCI_STATE_WORKING.
 * main() calls bt_init() only when cyw43_arch_init() SUCCEEDS, but the main
 * loop calls bt_power_on_tick() and le_central_tick() unconditionally. On a
 * boot where the radio fails to come up ("cyw43: init failed"), BTstack is
 * never initialised, so btstack_run_loop's `the_run_loop` is still NULL --
 * and every btstack_run_loop_* entry point is a thunk that dereferences it:
 *
 *     ldr r3, [the_run_loop]   ; NULL
 *     ldr r3, [r3, #28]        ; reads boot ROM 0x0000001C
 *     blx r3                   ; = 0x30322029 -> unmapped -> BusFault
 *
 * That is the whole of the "intermittent HardFault" (task #42): a NULL-vtable
 * call that lands on a boot-ROM constant, which is why the faulting address
 * was byte-identical on every occurrence. */
static bool s_stack_up;

/* --- unhandled-event tracing ----------------------------------------------
 * A generic Bluetooth tool should not silently discard what the stack tells
 * it. Anything this firmware does not explicitly act on is surfaced as a
 * bt.unhandled event, which is how you find out that a peripheral is doing
 * something we have no code for yet -- rather than wondering why nothing
 * happened.
 *
 * On by default; bt.trace turns it off. The only high-rate event is the
 * advertising report, which IS handled, so this stays quiet in normal use. */
static bool s_trace_unhandled = true;
static uint32_t s_unhandled_count;
uint32_t bt_unhandled_count(void) { return s_unhandled_count; }

/* Events some part of this firmware acts on. Listed explicitly rather than
 * inferred, so adding a handler without adding it here shows up as noise and
 * gets noticed. */
static bool event_is_handled(uint8_t evt) {
    switch (evt) {
        case BTSTACK_EVENT_STATE:
        case HCI_EVENT_DISCONNECTION_COMPLETE:
        case HCI_EVENT_META_GAP:
        case GAP_EVENT_ADVERTISING_REPORT:
        /* BTstack delivers LE events twice: the raw HCI_EVENT_LE_META (0x3e)
         * and a cooked GAP_/GATT_ event. We act on the cooked form, so the raw
         * one is not "unhandled" -- and counting it as such buried the trace
         * under one entry per advertisement, which is exactly the noise that
         * makes a diagnostic useless. */
        case HCI_EVENT_LE_META:
        case HCI_EVENT_TRANSPORT_PACKET_SENT:
        case HCI_EVENT_COMMAND_COMPLETE:
        case HCI_EVENT_COMMAND_STATUS:
        case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS:
        case BTSTACK_EVENT_NR_CONNECTIONS_CHANGED:
        case HCI_EVENT_VENDOR_SPECIFIC:
        case SM_EVENT_JUST_WORKS_REQUEST:
        case SM_EVENT_PASSKEY_INPUT_NUMBER:
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
        case SM_EVENT_PAIRING_COMPLETE:
        case GATT_EVENT_SERVICE_QUERY_RESULT:
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
        case GATT_EVENT_ALL_CHARACTERISTIC_DESCRIPTORS_QUERY_RESULT:
        case GATT_EVENT_QUERY_COMPLETE:
        case GATT_EVENT_NOTIFICATION:
        case GATT_EVENT_INDICATION:
        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT:
        case GATT_EVENT_MTU:
        case ATT_EVENT_MTU_EXCHANGE_COMPLETE:
        case ATT_EVENT_CAN_SEND_NOW:
        case ATT_EVENT_HANDLE_VALUE_INDICATION_COMPLETE:
        case L2CAP_EVENT_CAN_SEND_NOW:
            return true;
        default:
            return false;
    }
}

/* Names for the ones worth recognising at a glance; everything else is
 * reported by code, which is still enough to look up. */
static const char *event_name(uint8_t evt) {
    switch (evt) {
        case HCI_EVENT_CONNECTION_COMPLETE:            return "connection_complete";
        case HCI_EVENT_CONNECTION_REQUEST:             return "connection_request";
        case HCI_EVENT_ENCRYPTION_CHANGE:              return "encryption_change";
        case HCI_EVENT_ENCRYPTION_KEY_REFRESH_COMPLETE:return "encryption_key_refresh";
        case HCI_EVENT_READ_REMOTE_VERSION_INFORMATION_COMPLETE: return "remote_version";
        case HCI_EVENT_INQUIRY_COMPLETE:               return "inquiry_complete";
        case HCI_EVENT_INQUIRY_RESULT:                 return "inquiry_result";
        case SM_EVENT_IDENTITY_RESOLVING_STARTED:      return "sm_irk_resolving_started";
        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED:    return "sm_irk_resolved";
        case SM_EVENT_IDENTITY_RESOLVING_FAILED:       return "sm_irk_unresolved";
        case SM_EVENT_REENCRYPTION_STARTED:            return "sm_reencryption_started";
        case SM_EVENT_REENCRYPTION_COMPLETE:           return "sm_reencryption_complete";
        case SM_EVENT_AUTHORIZATION_REQUEST:           return "sm_authorization_request";
        case ATT_EVENT_CONNECTED:                      return "att_connected";
        case ATT_EVENT_DISCONNECTED:                   return "att_disconnected";
        case L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE: return "l2cap_conn_param_response";
        case GAP_EVENT_INQUIRY_RESULT:                 return "gap_inquiry_result";
        default:                                       return "unknown";
    }
}

static void trace_unhandled(uint8_t evt, const uint8_t *pkt, uint16_t size) {
    s_unhandled_count++;
    if (!s_trace_unhandled) {
        return;
    }
    char hex[2 * 24 + 1];
    uint16_t n = size > 24 ? 24 : size;   /* a prefix is enough to identify it */
    for (uint16_t i = 0; i < n; i++) {
        snprintf(hex + i * 2, 3, "%02x", pkt[i]);
    }
    hex[n * 2] = 0;
    char json[200];
    snprintf(json, sizeof(json),
             "{\"code\":\"0x%02x\",\"name\":\"%s\",\"len\":%u,\"hex\":\"%s\"%s}",
             evt, event_name(evt), size, hex, size > 24 ? ",\"truncated\":true" : "");
    api_emit("bt.unhandled", json);
}

bool bt_ready(void) { return s_ready; }
bool bt_stack_up(void) { return s_stack_up; }

static void hci_handler(uint8_t type, uint16_t ch, uint8_t *pkt, uint16_t size) {
    (void)ch;
    if (type != HCI_EVENT_PACKET) {
        return;
    }
    switch (hci_event_packet_get_type(pkt)) {
        case BTSTACK_EVENT_STATE:
            s_ready = (btstack_event_state_get_state(pkt) == HCI_STATE_WORKING);
            if (s_ready) {
                bd_addr_t addr;
                gap_local_bd_addr(addr);
                printf("bt: up, local address %s\n", bd_addr_to_str(addr));
            } else {
                printf("bt: state %u\n", btstack_event_state_get_state(pkt));
            }
            break;
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(pkt) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                bd_addr_t peer;
                gap_subevent_le_connection_complete_get_peer_address(pkt, peer);
                le_on_connected(gap_subevent_le_connection_complete_get_connection_handle(pkt),
                                peer,
                                gap_subevent_le_connection_complete_get_peer_address_type(pkt));
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            le_on_disconnected(hci_event_disconnection_complete_get_connection_handle(pkt));
            break;
        default:
            /* Not acted on anywhere in this firmware -- say so rather than
             * dropping it silently. */
            if (!event_is_handled(hci_event_packet_get_type(pkt))) {
                trace_unhandled(hci_event_packet_get_type(pkt), pkt, size);
            }
            break;
    }
}

/* --- ATT MTU ----------------------------------------------------------------
 * The MTU a connection asks for. BTstack exchanges it once, automatically,
 * as each connection comes up, offering l2cap_max_le_mtu() -- and the spec
 * allows one exchange per connection, so a new value applies to the NEXT
 * connection, not to one already up.
 *
 * The ceiling is l2cap_max_mtu(): HCI_ACL_PAYLOAD_SIZE less the L2CAP
 * header, 255 on this LE-only build (include/btstack_config.h). Lowering is
 * immediate (l2cap_set_max_le_mtu). Raising back to the ceiling is not:
 * BTstack takes a value only BELOW the ceiling and never clears the override
 * it set, so that case is saved and applies from the next boot -- the reply
 * says so ("reboot_needed") rather than claiming a change it did not make.
 *
 * Kept in cfg/bt.json as {"max_mtu": n} and applied in bt_init(). */
#define BT_CFG_PATH "cfg/bt.json"
#define MTU_MIN 23
static uint16_t s_mtu_saved;           /* 0 = never set: BTstack's own default */

static bool mtu_save(uint16_t n) {
    char j[40];
    int len = snprintf(j, sizeof j, "{\"max_mtu\":%u}\n", n);
    if (fs_begin_write(BT_CFG_PATH) < 0) { return false; }
    if (fs_write((const uint8_t *)j, (size_t)len) < 0) { fs_abort_write(); return false; }
    return fs_end_write() >= 0;
}

static void mtu_load(void) {
    size_t size = 0;
    int h = fs_begin_read(BT_CFG_PATH, &size);
    if (h < 0) { return; }
    char buf[96];
    int n = fs_read(h, (uint8_t *)buf, sizeof buf - 1);
    fs_end_read(h);
    if (n <= 0) { return; }
    buf[n] = 0;
    long v = 0;
    if (!api_json_int(buf, "max_mtu", &v) || v < MTU_MIN || v > l2cap_max_mtu()) { return; }
    s_mtu_saved = (uint16_t)v;
    if (v < l2cap_max_mtu()) { l2cap_set_max_le_mtu((uint16_t)v); }
    printf("bt: ATT MTU %u requested (cfg/bt.json)\n", (unsigned)l2cap_max_le_mtu());
}

static void op_bt_mtu(const char *params, api_reply_fn reply, void *ctx, long id) {
    long want = 0;
    bool reboot_needed = false;
    if (api_json_int(params, "max_mtu", &want)) {
        if (want < MTU_MIN || want > l2cap_max_mtu()) {
            char e[80];
            snprintf(e, sizeof e, "max_mtu is %d..%u on this build", MTU_MIN, (unsigned)l2cap_max_mtu());
            api_reply_err(reply, ctx, id, "bad_request", e);
            return;
        }
        if (!mtu_save((uint16_t)want)) {
            api_reply_err(reply, ctx, id, "fs_error", "could not save " BT_CFG_PATH);
            return;
        }
        s_mtu_saved = (uint16_t)want;
        if (want < l2cap_max_mtu()) { l2cap_set_max_le_mtu((uint16_t)want); }
        else { reboot_needed = l2cap_max_le_mtu() != l2cap_max_mtu(); }
    }
    char *r = api_scratch();
    size_t cap = api_scratch_size();
    size_t n = (size_t)snprintf(r, cap,
        "{\"max_mtu\":%u,\"ceiling\":%u,\"saved\":%u,\"reboot_needed\":%s,\"links\":[",
        (unsigned)l2cap_max_le_mtu(), (unsigned)l2cap_max_mtu(), (unsigned)s_mtu_saved,
        reboot_needed ? "true" : "false");
    bool first = true;
    for (size_t i = 0; i < bt_registry_count() && n + 48 < cap; i++) {
        bt_device_t *d = bt_registry_at(i);
        if (!d || !d->connected) { continue; }
        uint16_t mtu = 0;
        gatt_client_get_mtu(d->con_handle, &mtu);
        n += (size_t)snprintf(r + n, cap - n, "%s{\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"mtu\":%u}",
                              first ? "" : ",", d->addr[0], d->addr[1], d->addr[2], d->addr[3],
                              d->addr[4], d->addr[5], (unsigned)mtu);
        first = false;
    }
    snprintf(r + n, cap - n, "]}");
    api_reply_ok(reply, ctx, id, r);
}

void bt_init(void) {
    l2cap_init();
    mtu_load();
    sm_init();
    /* SM configuration belongs to bond.c, which owns the policy. */
    gatt_client_init();
    /* A peer can query US over the same connection; without a local database
     * those reads fail. See src/bt/le/gattserver.c. */
    gatt_server_init(CONFIG_NET_HOSTNAME);

    bt_registry_reset();
    le_central_init();
    bond_init();

    hci_cb.callback = &hci_handler;
    hci_add_event_handler(&hci_cb);
    /* Only now is it safe for anything to call into BTstack. */
    s_stack_up = true;
    /* Controller power-on is deferred to bt_power_on_tick(); see below. */
}

/*
 * Power on the BT controller once WiFi has had the shared radio to itself for
 * its initial scan + join (wifi_bt_hold). Downloading the controller firmware
 * over the shared bus concurrently with a WiFi join was wedging the radio at
 * boot (task #37) -- the LED-confirmed boot wedge that scan/connect staggering
 * alone did not fix. Fires exactly once, from the main loop. On a no-WiFi
 * build there is nothing to wait for, so it powers on immediately.
 */
void bt_power_on_tick(void) {
    static bool powered;
    /* No BTstack this boot: hci_power_control() would call through a NULL
     * run loop. The radio failing is survivable; faulting over it is not. */
    if (!s_stack_up) { return; }
    if (powered) { return; }
#if CONFIG_NET_WIFI
    if (wifi_bt_hold()) { return; }
#endif
    powered = true;
    hci_power_control(HCI_POWER_ON);
    printf("bt: controller power-on (WiFi has had its turn)\n");
}

/* ---- ops ---------------------------------------------------------------- */

static void op_bt_state(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char result[160];
    bd_addr_t addr;
    gap_local_bd_addr(addr);
    snprintf(result, sizeof(result),
             "{\"ready\":%s,\"address\":\"%s\",\"max_links\":%d}",
             s_ready ? "true" : "false", bd_addr_to_str(addr), CONFIG_BT_MAX_LINKS);
    api_reply_ok(reply, ctx, id, result);
}

/* One scratch buffer, filled and consumed within the call -- core0 is
 * single-threaded and its stack is small. */
#define list_buf   api_scratch()
#define LIST_BUF_SZ api_scratch_size()

/* Parses the optional filter object. Note what is absent: there is no way to
 * filter on a GATT service UUID, because those are invisible until after a
 * connection and a discovery. Only advertised data can be matched here. */
static void parse_filter(const char *params, le_scan_filter_t *f) {
    memset(f, 0, sizeof(*f));
    f->active = true;
    (void)api_json_bool(params, "active", &f->active);

    long rssi = 0;
    if (api_json_int(params, "rssi_min", &rssi)) {
        f->rssi_min = (int8_t)rssi;
    }
    api_json_str(params, "name_prefix", f->name_prefix, sizeof(f->name_prefix));

    char hex[24];
    if (api_json_str(params, "mfg_prefix", hex, sizeof(hex))) {
        size_t n = strlen(hex) / 2;
        if (n > sizeof(f->mfg_prefix)) {
            n = sizeof(f->mfg_prefix);
        }
        for (size_t i = 0; i < n; i++) {
            unsigned b = 0;
            sscanf(hex + i * 2, "%2x", &b);
            f->mfg_prefix[i] = (uint8_t)b;
        }
        f->mfg_prefix_len = (uint8_t)n;
    }
    char u[8];
    if (api_json_str(params, "service_uuid", u, sizeof(u))) {
        unsigned v = 0;
        if (sscanf(u, "%4x", &v) == 1) {
            f->uuid16[0] = (uint16_t)v;
            f->n_uuid16 = 1;
        }
    }
}

static void op_scan_start(const char *params, api_reply_fn reply, void *ctx, long id) {
    char transport[12] = "le";
    api_json_str(params, "transport", transport, sizeof(transport));
    if (strcmp(transport, "le") != 0) {
        api_reply_err(reply, ctx, id, "unsupported_transport",
                      "only \"le\" is built; Classic is phase 9");
        return;
    }
    if (!s_ready) {
        api_reply_err(reply, ctx, id, "not_ready", "controller is not up");
        return;
    }
    long dur = 0;
    (void)api_json_int(params, "duration_ms", &dur);

    le_scan_filter_t f;
    parse_filter(params, &f);
    le_scan_start(&f, (uint32_t)dur);

    char r[96];
    snprintf(r, sizeof(r), "{\"scanning\":true,\"active\":%s,\"duration_ms\":%ld}",
             f.active ? "true" : "false", dur);
    api_reply_ok(reply, ctx, id, r);
}

static void op_scan_stop(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    le_scan_stop();
    api_reply_ok(reply, ctx, id, "{\"scanning\":false}");
}

/* scan.list {offset?, limit?} -> {devices, scanning, total, next}
 *
 * PAGED. One reply is one websocket frame (WS_TX_MAX, 4 KB), and every
 * device's JSON now carries its raw advertisement and scan response, so a
 * busy room never fit in one: the old reply stopped at the buffer edge and
 * said nothing (measured: 3751 B, 24 devices, with ~46 in range). Now it
 * stops there and says where -- `next` is the offset to ask for, or null at
 * the end -- and the client walks the pages. No parameters is the first page,
 * so an older client still gets what it always got. */
static void op_scan_list(const char *params, api_reply_fn reply, void *ctx, long id) {
    long off = 0, lim = 1000;
    (void)api_json_int(params, "offset", &off);
    (void)api_json_int(params, "limit", &lim);
    if (off < 0) { off = 0; }
    if (lim < 1) { lim = 1; }
    size_t n = (size_t)snprintf(list_buf, LIST_BUF_SZ, "{\"devices\":[");
    size_t count = bt_registry_count();
    size_t i = (size_t)off;
    long sent = 0;
    bool first = true;
    static char one[768];           /* static: core0's stack is small */
    for (; i < count && sent < lim; i++) {
        bt_device_t *d = bt_registry_at(i);
        if (!d) {
            continue;
        }
        bt_device_json(d, one, sizeof(one));
        /* Room for this device AND the closing fields (~80 bytes). */
        if (n + strlen(one) + 96 >= LIST_BUF_SZ) {
            break;
        }
        n += (size_t)snprintf(list_buf + n, LIST_BUF_SZ - n, "%s%s", first ? "" : ",", one);
        first = false;
        sent++;
    }
    char next[16];
    if (i < count) { snprintf(next, sizeof next, "%u", (unsigned)i); }
    else { snprintf(next, sizeof next, "null"); }
    snprintf(list_buf + n, LIST_BUF_SZ - n, "],\"scanning\":%s,\"total\":%u,\"next\":%s}",
             le_scanning() ? "true" : "false", (unsigned)count, next);
    api_reply_ok(reply, ctx, id, list_buf);
}

/* Resolves {"device":"AA:BB:.."} to a registry entry. */
static bt_device_t *dev_param(const char *params) {
    char addr[20];
    if (!api_json_str(params, "device", addr, sizeof(addr))) {
        return NULL;
    }
    return bt_registry_by_str(addr, BT_TRANSPORT_LE);
}

static void op_dev_connect(const char *params, api_reply_fn reply, void *ctx, long id) {
    bt_device_t *d = dev_param(params);
    if (!d) {
        api_reply_err(reply, ctx, id, "unknown_device",
                      "address not in the scan table -- scan first, so the "
                      "address type is known");
        return;
    }
    if (d->connected) {
        api_reply_ok(reply, ctx, id, "{\"connected\":true,\"already\":true}");
        return;
    }
    /* addr_type comes from the scan record. Assuming public produces a connect
     * that simply never completes against a random-address peripheral. */
    if (!le_connect(d->addr, d->addr_type)) {
        api_reply_err(reply, ctx, id, "busy", "another connect is already in flight");
        return;
    }
    api_reply_ok(reply, ctx, id, "{\"connecting\":true}");
}

static void op_dev_disconnect(const char *params, api_reply_fn reply, void *ctx, long id) {
    bt_device_t *d = dev_param(params);
    if (!d || !le_disconnect(d->addr)) {
        api_reply_err(reply, ctx, id, "not_connected", "no such open connection");
        return;
    }
    api_reply_ok(reply, ctx, id, "{\"disconnecting\":true}");
}

static void op_dev_list(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    size_t n = (size_t)snprintf(list_buf, LIST_BUF_SZ, "{\"devices\":[");
    bool first = true;
    for (size_t i = 0; i < bt_registry_count(); i++) {
        bt_device_t *d = bt_registry_at(i);
        if (!d || !d->connected) {
            continue;
        }
        static char one[768];       /* static: core0's stack is small */
        bt_device_json(d, one, sizeof(one));
        if (n + strlen(one) + 4 >= LIST_BUF_SZ) {
            break;
        }
        n += (size_t)snprintf(list_buf + n, LIST_BUF_SZ - n, "%s%s", first ? "" : ",", one);
        first = false;
    }
    snprintf(list_buf + n, LIST_BUF_SZ - n, "]}");
    api_reply_ok(reply, ctx, id, list_buf);
}

static void op_gatt_tree(const char *params, api_reply_fn reply, void *ctx, long id) {
    bt_device_t *d = dev_param(params);
    if (!d || !d->connected) {
        api_reply_err(reply, ctx, id, "not_connected", "device is not connected");
        return;
    }
    gattdb_t *db = le_db_for(d->con_handle);
    if (!db) {
        api_reply_err(reply, ctx, id, "no_database", "no GATT database for this link");
        return;
    }
    if (!db->ready) {
        /* Discovery spans many callbacks; answering from a half-built table is
         * how "characteristic not found" becomes intermittent. */
        api_reply_err(reply, ctx, id, "discovering", "discovery still in progress");
        return;
    }
    /* Paged. A device with a hundred-odd characteristics serialises to more
     * than lwIP's TCP send buffer takes in one write, and an oversized frame
     * is dropped whole rather than split -- so the summary is always small and
     * detail is fetched one service at a time. */
    char svc[8];
    if (api_json_str(params, "service", svc, sizeof(svc))) {
        long si = strtol(svc, NULL, 10);
        if (si < 0 || gattdb_service_json(db, (uint8_t)si, list_buf, LIST_BUF_SZ) == 0) {
            api_reply_err(reply, ctx, id, "no_such_service", "no service with that index");
            return;
        }
    } else {
        gattdb_summary_json(db, list_buf, LIST_BUF_SZ);
    }
    api_reply_ok(reply, ctx, id, list_buf);
}

/* Resolve a characteristic from either addressing form:
 *   {"device":..,"id":"2.1"}
 *   {"device":..,"service":"8c85...","characteristic":"8c85...","instance":0}
 * Both are accepted everywhere, and neither is an ATT handle. */
static bool resolve_chr(const char *params, bt_device_t **out_d,
                        uint8_t *svc_i, uint8_t *chr_i, const char **err) {
    bt_device_t *d = dev_param(params);
    if (!d || !d->connected) { *err = "device is not connected"; return false; }
    gattdb_t *db = le_db_for(d->con_handle);
    if (!db || !db->ready) { *err = "discovery still in progress"; return false; }

    char id[24];
    if (api_json_str(params, "id", id, sizeof(id))) {
        if (!gattdb_resolve_chr(db, id, svc_i, chr_i)) { *err = "no such path"; return false; }
    } else {
        char su[40] = "", cu[40] = "";
        api_json_str(params, "service", su, sizeof(su));
        if (!api_json_str(params, "characteristic", cu, sizeof(cu))) {
            *err = "need \"id\" or \"characteristic\""; return false;
        }
        long inst = 0;
        (void)api_json_int(params, "instance", &inst);
        if (!gattdb_find_chr(db, su, cu, (int)inst, svc_i, chr_i)) {
            *err = "no such characteristic"; return false;
        }
    }
    *out_d = d;
    return true;
}

static int unhex(const char *h, uint8_t *out, size_t cap) {
    size_t n = strlen(h) / 2;
    if (n > cap) { n = cap; }
    for (size_t i = 0; i < n; i++) {
        unsigned b = 0;
        if (sscanf(h + i * 2, "%2x", &b) != 1) { return -1; }
        out[i] = (uint8_t)b;
    }
    return (int)n;
}

static void op_gatt_read(const char *params, api_reply_fn reply, void *ctx, long id) {
    bt_device_t *d; uint8_t si, ci; const char *err = NULL;
    if (!resolve_chr(params, &d, &si, &ci, &err)) {
        api_reply_err(reply, ctx, id, "bad_target", err); return;
    }
    int rc = le_read(d->addr, si, ci);
    if (rc == -5) {
        /* Not "busy" any more: operations are queued, so the only way to be
         * refused is a queue that is genuinely full -- which means the
         * caller is issuing faster than the link can drain. */
        api_reply_err(reply, ctx, id, "queue_full",
                      "this link's GATT operation queue is full");
        return;
    }
    if (rc != 0) {
        api_reply_err(reply, ctx, id, "read_failed", "the stack refused the read"); return;
    }
    /* The value arrives as a char.value event with source "read": one shape for
     * a value however it was produced, rather than two. */
    api_reply_ok(reply, ctx, id, "{\"reading\":true}");
}

static void op_gatt_write(const char *params, api_reply_fn reply, void *ctx, long id) {
    bt_device_t *d; uint8_t si, ci; const char *err = NULL;
    if (!resolve_chr(params, &d, &si, &ci, &err)) {
        api_reply_err(reply, ctx, id, "bad_target", err); return;
    }
    char hex[1024];
    if (!api_json_str(params, "value", hex, sizeof(hex))) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"value\" as a hex string"); return;
    }
    static uint8_t buf[512];
    int n = unhex(hex, buf, sizeof(buf));
    if (n < 0) {
        api_reply_err(reply, ctx, id, "bad_request", "\"value\" is not valid hex"); return;
    }
    bool with_response = true;
    (void)api_json_bool(params, "response", &with_response);
    int rc = le_write(d->addr, si, ci, buf, (uint16_t)n, with_response);
    if (rc == -5) {
        /* Not "busy" any more: operations are queued, so the only way to be
         * refused is a queue that is genuinely full -- which means the
         * caller is issuing faster than the link can drain. */
        api_reply_err(reply, ctx, id, "queue_full",
                      "this link's GATT operation queue is full");
        return;
    }
    if (rc != 0) {
        api_reply_err(reply, ctx, id, "write_failed", "the stack refused the write"); return;
    }
    char r[64];
    snprintf(r, sizeof(r), "{\"written\":%d,\"response\":%s}", n, with_response ? "true" : "false");
    api_reply_ok(reply, ctx, id, r);
}

static void op_gatt_subscribe(const char *params, api_reply_fn reply, void *ctx, long id) {
    bt_device_t *d; uint8_t si, ci; const char *err = NULL;
    if (!resolve_chr(params, &d, &si, &ci, &err)) {
        api_reply_err(reply, ctx, id, "bad_target", err); return;
    }
    char mode[12] = "notify";
    api_json_str(params, "mode", mode, sizeof(mode));
    int m = strcmp(mode, "indicate") == 0 ? 2 : strcmp(mode, "off") == 0 ? 0 : 1;
    int rc = le_subscribe(d->addr, si, ci, m);
    if (rc == -5) {
        /* Not "busy" any more: operations are queued, so the only way to be
         * refused is a queue that is genuinely full -- which means the
         * caller is issuing faster than the link can drain. */
        api_reply_err(reply, ctx, id, "queue_full",
                      "this link's GATT operation queue is full");
        return;
    }
    if (rc != 0) {
        api_reply_err(reply, ctx, id, "subscribe_failed", "the stack refused the CCCD write"); return;
    }
    char r[64];
    snprintf(r, sizeof(r), "{\"mode\":\"%s\"}", mode);
    api_reply_ok(reply, ctx, id, r);
}

static void op_bond_policy(const char *params, api_reply_fn reply, void *ctx, long id) {
    char name[16];
    if (api_json_str(params, "policy", name, sizeof(name)) && !bond_set_policy(name)) {
        api_reply_err(reply, ctx, id, "bad_request", "policy is never|on_demand|always");
        return;
    }
    char r[64];
    snprintf(r, sizeof(r), "{\"policy\":\"%s\"}", bond_policy_name(bond_policy()));
    api_reply_ok(reply, ctx, id, r);
}

static void op_bond_pair(const char *params, api_reply_fn reply, void *ctx, long id) {
    bt_device_t *d = dev_param(params);
    if (!d || !d->connected) {
        api_reply_err(reply, ctx, id, "not_connected", "connect first"); return;
    }
    if (!bond_request(d->con_handle)) {
        api_reply_err(reply, ctx, id, "refused", "bond policy is \"never\""); return;
    }
    api_reply_ok(reply, ctx, id, "{\"pairing\":true}");
}

static void op_bond_passkey(const char *params, api_reply_fn reply, void *ctx, long id) {
    bt_device_t *d = dev_param(params);
    long pk = 0;
    if (!d || !d->connected || !api_json_int(params, "passkey", &pk)) {
        api_reply_err(reply, ctx, id, "bad_request", "need a connected device and \"passkey\"");
        return;
    }
    bond_supply_passkey(d->con_handle, (uint32_t)pk);
    api_reply_ok(reply, ctx, id, "{\"supplied\":true}");
}

static void op_bond_list(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char r[1024];
    size_t n = (size_t)snprintf(r, sizeof(r), "{\"policy\":\"%s\",\"bonds\":", bond_policy_name(bond_policy()));
    n += bond_list_json(r + n, sizeof(r) - n);
    snprintf(r + n, sizeof(r) - n, "}");
    api_reply_ok(reply, ctx, id, r);
}

static void op_bond_remove(const char *params, api_reply_fn reply, void *ctx, long id) {
    bt_device_t *d = dev_param(params);
    if (!d || !bond_remove(d->addr, d->addr_type)) {
        api_reply_err(reply, ctx, id, "not_found", "no stored bond for that device"); return;
    }
    api_reply_ok(reply, ctx, id, "{\"removed\":true}");
}

/* The local database over the API too -- the registry's rule is that nothing
 * is reachable from only one front-end. */
static void op_gattsrv_describe(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    gatt_server_json(list_buf, LIST_BUF_SZ);
    api_reply_ok(reply, ctx, id, list_buf);
}

static void op_bt_trace(const char *params, api_reply_fn reply, void *ctx, long id) {
    bool on;
    if (api_json_bool(params, "unhandled", &on)) { s_trace_unhandled = on; }
    char r[96];
    snprintf(r, sizeof(r), "{\"unhandled\":%s,\"seen\":%lu}",
             s_trace_unhandled ? "true" : "false", (unsigned long)s_unhandled_count);
    api_reply_ok(reply, ctx, id, r);
}

static const op_def_t bt_ops[] = {
    { "gatt_server.describe", op_gattsrv_describe },
    { "bt.trace",             op_bt_trace },
    { "bond.policy",  op_bond_policy },
    { "bond.pair",    op_bond_pair },
    { "bond.passkey", op_bond_passkey },
    { "bond.list",    op_bond_list },
    { "bond.remove",  op_bond_remove },
    { "gatt.read",      op_gatt_read },
    { "gatt.write",     op_gatt_write },
    { "gatt.subscribe", op_gatt_subscribe },
    { "dev.connect",    op_dev_connect },
    { "dev.disconnect", op_dev_disconnect },
    { "dev.list",       op_dev_list },
    { "gatt.tree",      op_gatt_tree },
    { "bt.state",   op_bt_state },
    { "bt.mtu",     op_bt_mtu },
    { "scan.start", op_scan_start },
    { "scan.stop",  op_scan_stop },
    { "scan.list",  op_scan_list },
    { NULL, NULL }
};

static const char *const bt_topics[] = {
    "dev.connected", "dev.disconnected", "scan.device", "scan.done",
    "gatt.discovered", "char.value",
    "security.required", "security.bonded", "security.failed",
    "security.passkey_request", "security.numeric_comparison",
    "bt.unhandled", NULL
};

const op_module_t op_module_bt = {
    .ns = "bt", .ops = bt_ops, .topics = bt_topics, .init = NULL
};
