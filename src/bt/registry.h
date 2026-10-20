#ifndef BTGW_REGISTRY_H
#define BTGW_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The device registry: everything the firmware has seen or is talking to,
 * keyed by (transport, address).
 *
 * Transport-neutral on purpose. Only BLE exists today, but the whole point of
 * carrying a discriminator now is that adding Classic later is additive --
 * retrofitting one into a registry, a JSON schema and a Lua API after the fact
 * is the expensive change. Nothing in this file may assume LE.
 */

#define BT_ADDR_LEN 6

typedef enum {
    BT_TRANSPORT_LE = 0,
    BT_TRANSPORT_CLASSIC = 1,      /* reserved; phase 9 */
} bt_transport_t;

#define BT_NAME_MAX 30
#define BT_MFG_MAX  31
#define BT_UUIDS_MAX 4
#define BT_ADV_MAX  31             /* a legacy advertising / scan response payload */

typedef struct {
    bool     used;
    uint8_t  addr[BT_ADDR_LEN];
    bt_transport_t transport;
    uint8_t  addr_type;            /* LE only; meaningless for Classic */

    /* Advertisement data, merged across ADV_IND and SCAN_RSP. Each `have_*`
     * says whether we have ever actually been told, which is what stops a
     * scan response that carries no UUIDs from wiping the ones we had. */
    char     name[BT_NAME_MAX + 1];
    bool     have_name;
    uint8_t  mfg[BT_MFG_MAX];
    uint8_t  mfg_len;
    bool     have_mfg;
    uint16_t uuid16[BT_UUIDS_MAX]; /* advertised service UUIDs, NOT GATT ones */
    uint8_t  n_uuid16;
    bool     have_uuids;
    int8_t   rssi;
    int8_t   tx_power;
    bool     have_tx_power;
    bool     connectable;

    /* The raw payloads as last received, kept separately because they say
     * different things: the advertisement (ADV_IND & co) and the scan
     * response. The browser parses every AD type from these (js/adparse.js);
     * the fields above are what the firmware itself needs. */
    uint8_t  adv[BT_ADV_MAX];
    uint8_t  adv_len;
    uint8_t  rsp[BT_ADV_MAX];
    uint8_t  rsp_len;

    uint32_t last_seen_ms;
    bool     connected;
    uint16_t con_handle;           /* internal only; never serialised */
} bt_device_t;

void bt_registry_reset(void);

/* Find, or allocate, the entry for this peer. NULL when the table is full. */
bt_device_t *bt_registry_get(const uint8_t *addr, bt_transport_t transport);
bt_device_t *bt_registry_find(const uint8_t *addr, bt_transport_t transport);

/* Look a peer up by "AA:BB:CC:DD:EE:FF". */
bt_device_t *bt_registry_by_str(const char *addr_str, bt_transport_t transport);

bt_device_t *bt_registry_by_handle(uint16_t con_handle);

size_t bt_registry_count(void);
bt_device_t *bt_registry_at(size_t i);

/* {"addr":..,"transport":..,"rssi":..,...} -- one device, escaped. */
size_t bt_device_json(const bt_device_t *d, char *dst, size_t cap);

const char *bt_transport_name(bt_transport_t t);

/* Every 128-bit service UUID in the stored advertisement and scan response
 * (complete and incomplete lists, AD types 0x06/0x07), as canonical
 * upper-case 8-4-4-4-12 strings, de-duplicated. Returns how many (<= max). */
#define BT_UUID128_STR 37
int bt_device_uuids128(const bt_device_t *d, char (*out)[BT_UUID128_STR], int max);

#endif
