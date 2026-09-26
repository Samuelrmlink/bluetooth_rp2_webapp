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

void le_on_connected(uint16_t con_handle, const uint8_t *addr, uint8_t addr_type);
void le_on_disconnected(uint16_t con_handle);

static btstack_packet_callback_registration_t adv_cb;
static le_scan_filter_t s_filter;
static bool s_scanning;
static uint32_t s_scan_until_ms;
static bool s_connecting;
static uint32_t s_connect_deadline_ms;

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
    if (s_connecting && (int32_t)(now_ms() - s_connect_deadline_ms) >= 0) {
        /* gap_connect has no timeout of its own; without this a peer that has
         * gone away leaves the stack believing a connect is still in flight and
         * every later connect is refused. */
        gap_connect_cancel();
        s_connecting = false;
        api_emit("dev.connect_failed", "{\"reason\":\"timeout\"}");
    }
    if (s_scanning && s_scan_until_ms && (int32_t)(now_ms() - s_scan_until_ms) >= 0) {
        gap_stop_scan();
        s_scanning = false;
        s_scan_until_ms = 0;
        api_emit("scan.done", "{\"reason\":\"timeout\"}");
    }
}

/* ===================== connect, and the discovery chain =====================
 *
 * Discovery is a cascade of async callbacks -- services, then characteristics
 * per service, then descriptors per characteristic -- and the tables are only
 * coherent once the whole cascade finishes. gattdb.ready is cleared for the
 * duration and set at the end; every reader checks it. Without that gate a
 * client that asks mid-rebuild sees a half-built tree, which shows up as an
 * intermittent "characteristic not found" rather than as an obvious error.
 */

typedef struct {
    bool     active;
    uint16_t con_handle;
    uint8_t  addr[BT_ADDR_LEN];
    gattdb_t db;

    /* Cursors through the cascade. */
    uint8_t  svc_i;                 /* service whose characteristics we want */
    uint8_t  chr_i;                 /* characteristic whose descriptors we want */
    gatt_client_service_t        bt_svc;
    gatt_client_characteristic_t bt_chr;
} le_link_t;

static le_link_t links[CONFIG_BT_MAX_LINKS];

static le_link_t *link_by_handle(uint16_t h) {
    for (int i = 0; i < CONFIG_BT_MAX_LINKS; i++) {
        if (links[i].active && links[i].con_handle == h) {
            return &links[i];
        }
    }
    return NULL;
}

static le_link_t *link_alloc(void) {
    for (int i = 0; i < CONFIG_BT_MAX_LINKS; i++) {
        if (!links[i].active) {
            return &links[i];
        }
    }
    return NULL;
}

gattdb_t *le_db_for(uint16_t h) {
    le_link_t *l = link_by_handle(h);
    return l ? &l->db : NULL;
}

gattdb_t *le_db_for_addr(const uint8_t *addr) {
    bt_device_t *d = bt_registry_find(addr, BT_TRANSPORT_LE);
    return (d && d->connected) ? le_db_for(d->con_handle) : NULL;
}

static void emit_device_event(const char *topic, const uint8_t *addr) {
    bt_device_t *d = bt_registry_find(addr, BT_TRANSPORT_LE);
    char json[512];
    if (d) {
        bt_device_json(d, json, sizeof(json));
    } else {
        snprintf(json, sizeof(json), "{\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    }
    api_emit(topic, json);
}

static void discover_next(le_link_t *l);
static void gatt_handler(uint8_t type, uint16_t ch, uint8_t *pkt, uint16_t size);

/* Characteristics of svc_i, then descriptors of each, then the next service. */
static void discover_next(le_link_t *l) {
    gattdb_t *db = &l->db;

    /* Descriptors pending for the current characteristic? */
    while (l->svc_i < db->n_svc) {
        gdb_service_t *s = &db->svc[l->svc_i];
        if (l->chr_i < s->n_chr) {
            uint8_t ci = (uint8_t)(s->first_chr + l->chr_i);
            gdb_char_t *c = &db->chr[ci];
            l->bt_chr.start_handle = c->value_handle;   /* set below at capture */
            l->bt_chr.value_handle = c->value_handle;
            /* Descriptors of characteristic k run from its value handle + 1 up
             * to just before the NEXT characteristic's DECLARATION, which sits
             * one handle below that characteristic's value handle. Stopping at
             * next.value_handle - 1 instead swallows that declaration and
             * reports 0x2803 as though it were a descriptor. */
            l->bt_chr.end_handle   = (uint8_t)(l->chr_i + 1) < s->n_chr
                                   ? (uint16_t)(db->chr[ci + 1].value_handle - 2)
                                   : s->end_handle;
            l->bt_chr.properties   = c->properties;
            c->first_dsc = db->n_dsc;
            l->chr_i++;
            if (l->bt_chr.end_handle > l->bt_chr.value_handle) {
                gatt_client_discover_characteristic_descriptors(gatt_handler, l->con_handle, &l->bt_chr);
                return;
            }
            continue;   /* no room for descriptors between this and the next */
        }
        /* Service done; move to the next one's characteristics. */
        l->svc_i++;
        l->chr_i = 0;
        if (l->svc_i < db->n_svc) {
            gdb_service_t *ns = &db->svc[l->svc_i];
            l->bt_svc.start_group_handle = ns->start_handle;
            l->bt_svc.end_group_handle   = ns->end_handle;
            ns->first_chr = db->n_chr;
            gatt_client_discover_characteristics_for_service(gatt_handler, l->con_handle, &l->bt_svc);
            return;
        }
    }

    /* Whole cascade complete. */
    db->ready = true;
    gatt_client_get_mtu(l->con_handle, &db->mtu);

    static char tree[3072];
    size_t n = (size_t)snprintf(tree, sizeof(tree),
        "{\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"gatt\":",
        l->addr[0], l->addr[1], l->addr[2], l->addr[3], l->addr[4], l->addr[5]);
    n += gattdb_json(db, tree + n, sizeof(tree) - n);
    snprintf(tree + n, sizeof(tree) - n, "}");
    api_emit("gatt.discovered", tree);
    printf("bt: discovery done, %u svc / %u chr / %u dsc, mtu %u%s\n",
           db->n_svc, db->n_chr, db->n_dsc, db->mtu, db->truncated ? " (TRUNCATED)" : "");
}

static void gatt_handler(uint8_t type, uint16_t ch, uint8_t *pkt, uint16_t size) {
    (void)type; (void)size;
    le_link_t *l = link_by_handle(ch ? ch : 0);
    if (!l) {
        /* The event carries the handle in the payload for these. */
        switch (hci_event_packet_get_type(pkt)) {
            case GATT_EVENT_SERVICE_QUERY_RESULT:
            case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            case GATT_EVENT_ALL_CHARACTERISTIC_DESCRIPTORS_QUERY_RESULT:
            case GATT_EVENT_QUERY_COMPLETE:
                break;
            default:
                return;
        }
    }

    gattdb_t *db;
    switch (hci_event_packet_get_type(pkt)) {
        case GATT_EVENT_SERVICE_QUERY_RESULT: {
            l = link_by_handle(gatt_event_service_query_result_get_handle(pkt));
            if (!l) { return; }
            db = &l->db;
            gatt_client_service_t s;
            gatt_event_service_query_result_get_service(pkt, &s);
            if (db->n_svc >= CONFIG_GATT_MAX_SVCS) { db->truncated = true; return; }
            gdb_service_t *g = &db->svc[db->n_svc++];
            g->uuid = gattdb_intern(db, s.uuid16, s.uuid128);
            g->start_handle = s.start_group_handle;
            g->end_handle   = s.end_group_handle;
            g->first_chr = db->n_chr;
            g->n_chr = 0;
            break;
        }
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            l = link_by_handle(gatt_event_characteristic_query_result_get_handle(pkt));
            if (!l) { return; }
            db = &l->db;
            gatt_client_characteristic_t c;
            gatt_event_characteristic_query_result_get_characteristic(pkt, &c);
            if (db->n_chr >= CONFIG_GATT_MAX_CHRS) { db->truncated = true; return; }
            gdb_char_t *g = &db->chr[db->n_chr++];
            g->uuid = gattdb_intern(db, c.uuid16, c.uuid128);
            g->value_handle = c.value_handle;
            g->properties   = c.properties;
            g->cccd_handle  = 0;
            g->first_dsc = db->n_dsc;
            g->n_dsc = 0;
            db->svc[l->svc_i].n_chr++;
            break;
        }
        case GATT_EVENT_ALL_CHARACTERISTIC_DESCRIPTORS_QUERY_RESULT: {
            l = link_by_handle(gatt_event_all_characteristic_descriptors_query_result_get_handle(pkt));
            if (!l) { return; }
            db = &l->db;
            gatt_client_characteristic_descriptor_t d;
            gatt_event_all_characteristic_descriptors_query_result_get_characteristic_descriptor(pkt, &d);
            if (db->n_dsc >= CONFIG_GATT_MAX_DSCS) { db->truncated = true; return; }
            gdb_desc_t *g = &db->dsc[db->n_dsc++];
            g->uuid = gattdb_intern(db, d.uuid16, d.uuid128);
            g->handle = d.handle;
            /* chr_i was already advanced past this characteristic. */
            uint8_t ci = (uint8_t)(db->svc[l->svc_i].first_chr + l->chr_i - 1);
            db->chr[ci].n_dsc++;
            if (d.uuid16 == ORG_BLUETOOTH_DESCRIPTOR_GATT_CLIENT_CHARACTERISTIC_CONFIGURATION) {
                db->chr[ci].cccd_handle = d.handle;
            }
            break;
        }
        case GATT_EVENT_QUERY_COMPLETE: {
            l = link_by_handle(gatt_event_query_complete_get_handle(pkt));
            if (!l) { return; }
            discover_next(l);
            break;
        }
        default:
            break;
    }
}

static void start_discovery(le_link_t *l) {
    gattdb_reset(&l->db, l->con_handle);
    l->svc_i = 0;
    l->chr_i = 0;
    gatt_client_discover_primary_services(gatt_handler, l->con_handle);
}

/* Called from bt.c's HCI handler. */
void le_on_connected(uint16_t con_handle, const uint8_t *addr, uint8_t addr_type) {
    s_connecting = false;
    le_link_t *l = link_alloc();
    if (!l) {
        printf("bt: no free link slot; disconnecting %s\n", bd_addr_to_str((uint8_t *)addr));
        gap_disconnect(con_handle);
        return;
    }
    l->active = true;
    l->con_handle = con_handle;
    memcpy(l->addr, addr, BT_ADDR_LEN);

    bt_device_t *d = bt_registry_get(addr, BT_TRANSPORT_LE);
    if (d) {
        d->connected = true;
        d->con_handle = con_handle;
        d->addr_type = addr_type;
    }
    emit_device_event("dev.connected", addr);

    /* Discovery starts immediately; the MTU is read once it completes, because
     * BTstack negotiates it as part of the first GATT procedure. */
    start_discovery(l);
}

void le_on_disconnected(uint16_t con_handle) {
    le_link_t *l = link_by_handle(con_handle);
    uint8_t addr[BT_ADDR_LEN] = {0};
    if (l) {
        memcpy(addr, l->addr, BT_ADDR_LEN);
        memset(l, 0, sizeof(*l));
    }
    bt_device_t *d = bt_registry_by_handle(con_handle);
    if (d) {
        d->connected = false;
        d->con_handle = 0;
        memcpy(addr, d->addr, BT_ADDR_LEN);
    }
    emit_device_event("dev.disconnected", addr);
}

bool le_connect(const uint8_t *addr, uint8_t addr_type) {
    if (s_connecting) {
        return false;
    }
    /* Scanning and connecting at once is asking the controller to do two
     * things with one radio; stop first. */
    if (s_scanning) {
        gap_stop_scan();
        s_scanning = false;
    }
    s_connecting = true;
    s_connect_deadline_ms = now_ms() + 15000;
    gap_connect((uint8_t *)addr, (bd_addr_type_t)addr_type);
    return true;
}

bool le_disconnect(const uint8_t *addr) {
    bt_device_t *d = bt_registry_find(addr, BT_TRANSPORT_LE);
    if (!d || !d->connected) {
        return false;
    }
    gap_disconnect(d->con_handle);
    return true;
}
