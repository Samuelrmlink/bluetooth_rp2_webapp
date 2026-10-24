/* Connectable advertising beside the LE central. Starts only once the
 * controller reports HCI_STATE_WORKING, and only because BT_PERIPHERAL is
 * on. The central's scan path is a different handler. */
#include "bt/le/peripheral.h"

#include <stdio.h>
#include <string.h>

#include "api/ops.h"
#include "autoconf.h"
#include "btstack.h"
#include "bt/le/gattserver.h"

#define SMP_SVC "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
#define SMP_CHR "da2e7828-fbce-4e01-ae9e-261174997c48"

static btstack_packet_callback_registration_t s_hci;
static char s_name[30];
static uint8_t s_adv[31];
static uint8_t s_scan[31];
static int s_adv_on;

static void start_adv(void) {
    if (s_adv_on) { return; }
    size_t nlen = strlen(s_name);
    if (nlen > 29) { nlen = 29; }
    /* 0x02 LE General Discoverable. 0x04 BR/EDR Not Supported, only when
     * Classic is not in this image. */
#if CONFIG_BT_CLASSIC
    uint8_t flags = 0x02;
#else
    uint8_t flags = 0x06;
#endif
    s_adv[0] = 2;
    s_adv[1] = BLUETOOTH_DATA_TYPE_FLAGS;
    s_adv[2] = flags;
    static bd_addr_t none;
    gap_advertisements_set_params(0x00A0, 0x00F0, 0, 0, none, 0x07, 0);
    gap_advertisements_set_data(3, s_adv);
    s_scan[0] = (uint8_t)(nlen + 1);
    s_scan[1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(s_scan + 2, s_name, nlen);
    gap_scan_response_set_data((uint8_t)(nlen + 2), s_scan);
    gap_advertisements_enable(1);
    s_adv_on = 1;
    printf("bt: peripheral advertising \"%s\"\n", s_name);
}

static void hci_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET) { return; }
    if (hci_event_packet_get_type(packet) != BTSTACK_EVENT_STATE) { return; }
    if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
        start_adv();
    } else {
        s_adv_on = 0;
    }
}

void bt_peripheral_init(const char *name) {
    snprintf(s_name, sizeof s_name, "%s", name && *name ? name : "btgw");
    s_hci.callback = &hci_handler;
    hci_add_event_handler(&s_hci);
    printf("bt: peripheral armed (advertises when the controller is up)\n");
}

void op_bt_smp(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    gatt_server_begin();
    static uint8_t appearance[2];
    const char *err = NULL;
    if (!gatt_server_add_characteristic("2a01", ATT_PROPERTY_READ,
                                        appearance, sizeof appearance, NULL, &err)) {
        api_reply_err(reply, ctx, id, "failed", err ? err : "appearance");
        return;
    }
    if (!gatt_server_add_service(SMP_SVC, &err)) {
        api_reply_err(reply, ctx, id, "failed", err ? err : "service");
        return;
    }
    uint16_t props = ATT_PROPERTY_DYNAMIC | ATT_PROPERTY_WRITE
                   | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE | ATT_PROPERTY_NOTIFY;
    uint16_t handle = 0;
    if (!gatt_server_add_characteristic(SMP_CHR, props, NULL, 0, &handle, &err)) {
        api_reply_err(reply, ctx, id, "failed", err ? err : "characteristic");
        return;
    }
    if (!gatt_server_commit(&err)) {
        api_reply_err(reply, ctx, id, "failed", err ? err : "commit");
        return;
    }
    char b[48];
    snprintf(b, sizeof b, "{\"handle\":%u}", handle);
    api_reply_ok(reply, ctx, id, b);
}
