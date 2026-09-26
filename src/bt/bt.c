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
#include "autoconf.h"

static btstack_packet_callback_registration_t hci_cb;
static bool s_ready;

bool bt_ready(void) { return s_ready; }

static void hci_handler(uint8_t type, uint16_t ch, uint8_t *pkt, uint16_t size) {
    (void)ch; (void)size;
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
            break;
    }
}

void bt_init(void) {
    l2cap_init();
    sm_init();
    /* SM configuration belongs to bond.c, which owns the policy. */
    gatt_client_init();

    bt_registry_reset();
    le_central_init();
    bond_init();

    hci_cb.callback = &hci_handler;
    hci_add_event_handler(&hci_cb);
    hci_power_control(HCI_POWER_ON);
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
static char list_buf[3072];

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

static void op_scan_list(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    size_t n = (size_t)snprintf(list_buf, sizeof(list_buf), "{\"devices\":[");
    size_t count = bt_registry_count();
    bool first = true;
    for (size_t i = 0; i < count; i++) {
        bt_device_t *d = bt_registry_at(i);
        if (!d) {
            continue;
        }
        char one[512];
        bt_device_json(d, one, sizeof(one));
        if (n + strlen(one) + 4 >= sizeof(list_buf)) {
            break;                    /* truncate rather than overflow */
        }
        n += (size_t)snprintf(list_buf + n, sizeof(list_buf) - n, "%s%s", first ? "" : ",", one);
        first = false;
    }
    snprintf(list_buf + n, sizeof(list_buf) - n, "],\"scanning\":%s}",
             le_scanning() ? "true" : "false");
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
    size_t n = (size_t)snprintf(list_buf, sizeof(list_buf), "{\"devices\":[");
    bool first = true;
    for (size_t i = 0; i < bt_registry_count(); i++) {
        bt_device_t *d = bt_registry_at(i);
        if (!d || !d->connected) {
            continue;
        }
        char one[512];
        bt_device_json(d, one, sizeof(one));
        if (n + strlen(one) + 4 >= sizeof(list_buf)) {
            break;
        }
        n += (size_t)snprintf(list_buf + n, sizeof(list_buf) - n, "%s%s", first ? "" : ",", one);
        first = false;
    }
    snprintf(list_buf + n, sizeof(list_buf) - n, "]}");
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
    gattdb_json(db, list_buf, sizeof(list_buf));
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
    if (rc == -4) {
        api_reply_err(reply, ctx, id, "busy",
                      "another GATT procedure is in flight on this link; retry");
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
    if (rc == -4) {
        api_reply_err(reply, ctx, id, "busy",
                      "another GATT procedure is in flight on this link; retry");
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
    if (rc == -4) {
        api_reply_err(reply, ctx, id, "busy",
                      "another GATT procedure is in flight on this link; retry");
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

static const op_def_t bt_ops[] = {
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
    { "scan.start", op_scan_start },
    { "scan.stop",  op_scan_stop },
    { "scan.list",  op_scan_list },
    { NULL, NULL }
};

static const char *const bt_topics[] = {
    "dev.connected", "dev.disconnected", "scan.device", "scan.done",
    "gatt.discovered", "char.value",
    "security.required", "security.bonded", "security.failed",
    "security.passkey_request", "security.numeric_comparison", NULL
};

const op_module_t op_module_bt = {
    .ns = "bt", .ops = bt_ops, .topics = bt_topics, .init = NULL
};
