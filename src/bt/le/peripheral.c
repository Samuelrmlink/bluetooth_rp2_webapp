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
static uint8_t s_custom_adv[31];
static uint8_t s_custom_scan[31];
static uint8_t s_custom_adv_len;
static uint8_t s_custom_scan_len;
static int s_have_custom;
static int s_adv_on;
static int s_hci_up;
static int s_hold;          /* pex_adv_stop until the next pex_adv_start */
static int s_user_start;    /* the next enable uses the stored payload */
static uint16_t s_imin;
static uint16_t s_imax;

#define ADV_INT_LO 0x0020
#define ADV_INT_HI 0x4000
#define ADV_INT_DEF_MIN 0x00A0
#define ADV_INT_DEF_MAX 0x00F0

static void load_default(uint8_t *adv_len, uint8_t *scan_len) {
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
    *adv_len = 3;
    s_scan[0] = (uint8_t)(nlen + 1);
    s_scan[1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(s_scan + 2, s_name, nlen);
    *scan_len = (uint8_t)(nlen + 2);
}

/* gap_advertisements_* is not the scan path; the central scanner stays up. */
static void enable_adv(int use_custom) {
    uint8_t adv_len, scan_len;
    uint16_t imin = s_imin ? s_imin : ADV_INT_DEF_MIN;
    uint16_t imax = s_imax ? s_imax : ADV_INT_DEF_MAX;
    int custom = use_custom && s_have_custom;
    if (custom) {
        if (s_custom_adv_len) { memcpy(s_adv, s_custom_adv, s_custom_adv_len); }
        adv_len = s_custom_adv_len;
        if (s_custom_scan_len) { memcpy(s_scan, s_custom_scan, s_custom_scan_len); }
        scan_len = s_custom_scan_len;
    } else {
        load_default(&adv_len, &scan_len);
    }
    static bd_addr_t none;
    gap_advertisements_set_params(imin, imax, 0, 0, none, 0x07, 0);
    gap_advertisements_set_data(adv_len, s_adv);
    gap_scan_response_set_data(scan_len, s_scan);
    gap_advertisements_enable(1);
    s_adv_on = 1;
    if (custom) {
        printf("bt: peripheral advertising %u+%u bytes\n", adv_len, scan_len);
    } else {
        printf("bt: peripheral advertising \"%s\"\n", s_name);
    }
}

static void hci_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET) { return; }
    if (hci_event_packet_get_type(packet) != BTSTACK_EVENT_STATE) { return; }
    if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
        s_hci_up = 1;
        if (!s_adv_on && !s_hold) { enable_adv(s_user_start); }
    } else {
        s_hci_up = 0;
        s_adv_on = 0;
    }
}

int bt_adv_data(const uint8_t *adv, uint16_t adv_len, const uint8_t *scan_rsp, uint16_t scan_len) {
    if (adv_len > 31 || scan_len > 31) { return -1; }
    if ((adv_len && !adv) || (scan_len && !scan_rsp)) { return -1; }
    if (adv_len) { memcpy(s_custom_adv, adv, adv_len); }
    if (scan_len) { memcpy(s_custom_scan, scan_rsp, scan_len); }
    s_custom_adv_len = (uint8_t)adv_len;
    s_custom_scan_len = (uint8_t)scan_len;
    s_have_custom = 1;
    return 0;
}

int bt_adv_start(uint16_t interval_min, uint16_t interval_max) {
    uint16_t imin = interval_min ? interval_min : ADV_INT_DEF_MIN;
    uint16_t imax = interval_max ? interval_max : ADV_INT_DEF_MAX;
    if (imin < ADV_INT_LO || imax > ADV_INT_HI || imin > imax) { return -1; }
    s_imin = imin;
    s_imax = imax;
    s_hold = 0;
    s_user_start = 1;
    if (s_hci_up) { enable_adv(1); }
    return 0;
}

void bt_adv_stop(void) {
    s_hold = 1;
    s_user_start = 0;
    if (s_hci_up) { gap_advertisements_enable(0); }
    s_adv_on = 0;
}

int bt_adv_set_addr(const uint8_t addr[6]) {
    if (!addr) { return -1; }
    gap_random_address_set(addr);
    return 0;
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
