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

static const op_def_t bt_ops[] = {
    { "bt.state", op_bt_state },
    { NULL, NULL }
};

static const char *const bt_topics[] = {
    "dev.connected", "dev.disconnected", "scan.device", "scan.done", NULL
};

const op_module_t op_module_bt = {
    .ns = "bt", .ops = bt_ops, .topics = bt_topics, .init = NULL
};
