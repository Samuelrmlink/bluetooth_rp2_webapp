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
        default:
            break;
    }
}

void bt_init(void) {
    l2cap_init();
    sm_init();
    /* Central role, no bonding requirement by default: most peripherals want
     * none, and the ones that do ask for it during a procedure. Phase 4 makes
     * this configurable per connection. */
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gatt_client_init();

    bt_registry_reset();
    le_central_init();

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

/* One scratch buffer, filled and consumed within the call -- core0 is
 * single-threaded and its stack is small. */
static char list_buf[4096];

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

static const op_def_t bt_ops[] = {
    { "bt.state",   op_bt_state },
    { "scan.start", op_scan_start },
    { "scan.stop",  op_scan_stop },
    { "scan.list",  op_scan_list },
    { NULL, NULL }
};

static const char *const bt_topics[] = {
    "dev.connected", "dev.disconnected", "scan.device", "scan.done", NULL
};

const op_module_t op_module_bt = {
    .ns = "bt", .ops = bt_ops, .topics = bt_topics, .init = NULL
};
