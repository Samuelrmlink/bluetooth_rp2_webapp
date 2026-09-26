/*
 * LE central: scanning, and the advertisement bookkeeping behind it.
 *
 * Two things here are not obvious and both come from hard-won experience in
 * the ESP32 predecessor to this project:
 *
 * 1. ADV_IND and SCAN_RSP arrive as SEPARATE events and a device splits its
 *    data across them. Fields must therefore be merged only when the packet
 *    actually carried them. Overwriting unconditionally means a UUID-less scan
 *    response wipes the UUID list that the advertisement just supplied, and
 *    every device appears to advertise nothing.
 *
 * 2. A scan filter may NOT match on a GATT service UUID. GATT services are
 *    only visible after connecting and discovering. Only what is in the
 *    advertisement -- 16-bit service UUIDs and manufacturer data -- can be
 *    filtered on here. Filtering a scan on a device's GATT service UUID can
 *    never match, and silently finds nothing.
 */
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "pico/time.h"

#include "bt/le/central.h"
#include "bt/registry.h"
#include "api/api.h"

static btstack_packet_callback_registration_t adv_cb;
static le_scan_filter_t s_filter;
static bool s_scanning;
static uint32_t s_scan_until_ms;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

bool le_scanning(void) { return s_scanning; }

/* Applies the filter to a merged registry entry. Deliberately evaluated after
 * the merge, so a device whose name arrives only in the SCAN_RSP still matches
 * a name_prefix filter. */
static bool passes_filter(const bt_device_t *d) {
    if (s_filter.rssi_min && d->rssi < s_filter.rssi_min) {
        return false;
    }
    if (s_filter.name_prefix[0]) {
        if (!d->have_name || strncasecmp(d->name, s_filter.name_prefix,
                                         strlen(s_filter.name_prefix)) != 0) {
            return false;
        }
    }
    if (s_filter.mfg_prefix_len) {
        if (!d->have_mfg || d->mfg_len < s_filter.mfg_prefix_len ||
            memcmp(d->mfg, s_filter.mfg_prefix, s_filter.mfg_prefix_len) != 0) {
            return false;
        }
    }
    if (s_filter.n_uuid16) {
        if (!d->have_uuids) {
            return false;
        }
        bool hit = false;
        for (uint8_t i = 0; i < s_filter.n_uuid16 && !hit; i++) {
            for (uint8_t k = 0; k < d->n_uuid16; k++) {
                if (d->uuid16[k] == s_filter.uuid16[i]) { hit = true; break; }
            }
        }
        if (!hit) {
            return false;
        }
    }
    return true;
}

/* Merge one advertisement's AD structures into the registry entry. Every field
 * is written only if this packet carried it -- see the header comment. */
static void merge_ad(bt_device_t *d, const uint8_t *data, uint8_t len) {
    ad_context_t ctx;
    for (ad_iterator_init(&ctx, len, data); ad_iterator_has_more(&ctx); ad_iterator_next(&ctx)) {
        uint8_t type = ad_iterator_get_data_type(&ctx);
        uint8_t dlen = ad_iterator_get_data_len(&ctx);
        const uint8_t *d8 = ad_iterator_get_data(&ctx);

        switch (type) {
            case BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME:
            case BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME: {
                uint8_t n = dlen > BT_NAME_MAX ? BT_NAME_MAX : dlen;
                memcpy(d->name, d8, n);
                d->name[n] = 0;
                d->have_name = true;
                break;
            }
            case BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA: {
                uint8_t n = dlen > BT_MFG_MAX ? BT_MFG_MAX : dlen;
                memcpy(d->mfg, d8, n);
                d->mfg_len = n;
                d->have_mfg = true;
                break;
            }
            case BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS:
            case BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS: {
                uint8_t n = 0;
                for (uint8_t off = 0; off + 1 < dlen && n < BT_UUIDS_MAX; off += 2) {
                    d->uuid16[n++] = little_endian_read_16(d8, off);
                }
                d->n_uuid16 = n;
                d->have_uuids = true;
                break;
            }
            case BLUETOOTH_DATA_TYPE_TX_POWER_LEVEL:
                if (dlen >= 1) {
                    d->tx_power = (int8_t)d8[0];
                    d->have_tx_power = true;
                }
                break;
            default:
                break;
        }
    }
}

static void on_advertisement(uint8_t *pkt) {
    bd_addr_t addr;
    gap_event_advertising_report_get_address(pkt, addr);

    bt_device_t *d = bt_registry_get(addr, BT_TRANSPORT_LE);
    if (!d) {
        return;
    }
    d->addr_type = gap_event_advertising_report_get_address_type(pkt);
    d->rssi = (int8_t)gap_event_advertising_report_get_rssi(pkt);
    d->last_seen_ms = now_ms();

    uint8_t et = gap_event_advertising_report_get_advertising_event_type(pkt);
    /* 0 = ADV_IND, 1 = ADV_DIRECT_IND -- both connectable. 4 = SCAN_RSP, which
     * says nothing about connectability, so it must not clear the flag. */
    if (et == 0 || et == 1) {
        d->connectable = true;
    }

    merge_ad(d, gap_event_advertising_report_get_data(pkt),
             gap_event_advertising_report_get_data_length(pkt));

    if (!passes_filter(d)) {
        return;
    }
    char json[512];
    bt_device_json(d, json, sizeof(json));
    api_emit("scan.device", json);
}

static void adv_handler(uint8_t type, uint16_t ch, uint8_t *pkt, uint16_t size) {
    (void)ch; (void)size;
    if (type != HCI_EVENT_PACKET) {
        return;
    }
    switch (hci_event_packet_get_type(pkt)) {
        case GAP_EVENT_ADVERTISING_REPORT:
            on_advertisement(pkt);
            break;
        default:
            break;
    }
}

void le_central_init(void) {
    adv_cb.callback = &adv_handler;
    hci_add_event_handler(&adv_cb);
}

bool le_scan_start(const le_scan_filter_t *f, uint32_t duration_ms) {
    s_filter = *f;
    /* Active scanning solicits SCAN_RSP, which is where most peripherals put
     * the complete local name. A passive scan often sees no names at all. */
    gap_set_scan_parameters(f->active ? 1 : 0, 0x0030, 0x0030);
    gap_start_scan();
    s_scanning = true;
    s_scan_until_ms = duration_ms ? now_ms() + duration_ms : 0;
    return true;
}

void le_scan_stop(void) {
    if (!s_scanning) {
        return;
    }
    gap_stop_scan();
    s_scanning = false;
    s_scan_until_ms = 0;
    api_emit("scan.done", "{\"reason\":\"stopped\"}");
}

void le_central_tick(void) {
    if (s_scanning && s_scan_until_ms && (int32_t)(now_ms() - s_scan_until_ms) >= 0) {
        gap_stop_scan();
        s_scanning = false;
        s_scan_until_ms = 0;
        api_emit("scan.done", "{\"reason\":\"timeout\"}");
    }
}
