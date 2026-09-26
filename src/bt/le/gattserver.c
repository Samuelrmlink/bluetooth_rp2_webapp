/*
 * The LOCAL GATT database -- what a peer sees when it queries US.
 *
 * Easy to forget when writing a central, but a GATT connection is symmetric:
 * the peripheral can turn around and act as a client. Without an ATT database
 * those reads fail. BTstack's own client examples ship a .gatt with GAP
 * Service for exactly this reason (see example/gatt_device_information_query,
 * which calls att_server_init despite being a pure client).
 *
 * Built with att_db_util at RUNTIME rather than compile_gatt.py at build time.
 * That costs nothing here and buys the thing this project is for: a Lua script
 * will be able to declare services and characteristics without a reflash.
 */
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "bt/le/gattserver.h"
#include "autoconf.h"

static char s_device_name[32];

static uint16_t att_read_cb(hci_con_handle_t c, uint16_t handle, uint16_t off,
                            uint8_t *buf, uint16_t sz) {
    (void)c; (void)handle; (void)off; (void)buf; (void)sz;
    return 0;   /* everything served so far is a static value in the db */
}

static int att_write_cb(hci_con_handle_t c, uint16_t handle, uint16_t mode,
                        uint16_t off, uint8_t *buf, uint16_t sz) {
    (void)c; (void)handle; (void)mode; (void)off; (void)buf; (void)sz;
    return 0;
}

void gatt_server_init(const char *name) {
    snprintf(s_device_name, sizeof(s_device_name), "%s", name && *name ? name : "btgw");

    att_db_util_init();

    /* GAP Service: Device Name and Appearance. This is the minimum a peer
     * expects to be able to read from us. */
    att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ACCESS);
    att_db_util_add_characteristic_uuid16(
        ORG_BLUETOOTH_CHARACTERISTIC_GAP_DEVICE_NAME,
        ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE,
        (uint8_t *)s_device_name, (uint16_t)strlen(s_device_name));

    /* 0x0000 = Unknown. Honest: this is a gateway, not a category the spec
     * has a number for. */
    static uint8_t appearance[2] = { 0x00, 0x00 };
    att_db_util_add_characteristic_uuid16(
        ORG_BLUETOOTH_CHARACTERISTIC_GAP_APPEARANCE,
        ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE,
        appearance, sizeof(appearance));

    att_server_init(att_db_util_get_address(), att_read_cb, att_write_cb);
    printf("bt: local GATT database %u bytes (name \"%s\")\n",
           att_db_util_get_size(), s_device_name);
}

uint16_t gatt_server_db_size(void) { return att_db_util_get_size(); }
