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
#include "net/ws.h"
#include "bt/le/bond.h"
#if CONFIG_LUA_ENABLE
#include "lua/lwait.h"
#endif
#include "autoconf.h"

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

/* Defined after the cascade it re-issues. */
static void discovery_tick(void);

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

    discovery_tick();
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

    /* Retry state. Every gatt_client call in the cascade can be refused --
     * most often because that handle's client is not ready yet, which is
     * exactly what happens when a SECOND link is being discovered while a
     * first is mid-operation. The cascade used to ignore those returns, so a
     * single refusal stopped the state machine dead: db.ready never became
     * true, nothing retried, and nothing reported. The symptom was a link that
     * connected and then never finished discovering. */
    uint8_t  svc_issued;            /* services whose chr query has been issued */
    uint8_t  disc_stage;            /* DISC_* below; what to re-issue */
    uint8_t  disc_err;              /* last ATT error seen during the cascade */
    uint8_t  disc_inflight;         /* DISC_IN_*; which query is outstanding */
    uint32_t disc_deadline_ms;      /* 0 = not discovering */
} le_link_t;

/* What the tick should re-issue for a link whose last attempt was refused. */
#define DISC_IDLE    0
#define DISC_PRIMARY 1              /* gatt_client_discover_primary_services */
#define DISC_NEXT    2              /* resume the characteristic/descriptor walk */
#define DISC_SECURITY 3             /* parked until pairing completes */

/* Which query is outstanding. A GATT_EVENT_QUERY_COMPLETE carries a status but
 * no indication of WHAT it completed, and the right recovery differs per step:
 * a failed primary-service query leaves nothing usable, while a failed
 * descriptor query costs one characteristic's descriptors and no more. */
#define DISC_IN_NONE    0
#define DISC_IN_PRIMARY 1
#define DISC_IN_CHR     2
#define DISC_IN_DSC     3

/* A cascade that never progresses must fail loudly rather than hang. Generous
 * because a real peripheral with 65 characteristics takes a couple of seconds
 * and a busy radio can stretch that. */
#define DISC_TIMEOUT_MS 15000

static le_link_t links[CONFIG_BT_MAX_LINKS];

/* A link's SLOT index, which is what the wait contract keys on: it is stable
 * for the life of a connection and small enough to index a table, where a
 * con_handle is neither. */
static int link_index(const le_link_t *l) {
    return (int)(l - links);
}

#if CONFIG_LUA_ENABLE
/*
 * The token a script blocks on. Today a wait is "any frame from this device",
 * so the address alone identifies it; the token exists as a separate concept
 * because awaiting one CHARACTERISTIC is the obvious next step and should not
 * require changing the contract. FNV-1a, because it needs to be stable and
 * cheap, not cryptographic.
 */
uint32_t le_addr_token(const char *addr) {
    uint32_t h = 2166136261u;
    for (const char *p = addr; p && *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h;
}

int le_link_slot(const char *addr) {
    for (int i = 0; i < CONFIG_BT_MAX_LINKS; i++) {
        if (!links[i].active) { continue; }
        char a[18];
        snprintf(a, sizeof a, "%02X:%02X:%02X:%02X:%02X:%02X",
                 links[i].addr[0], links[i].addr[1], links[i].addr[2],
                 links[i].addr[3], links[i].addr[4], links[i].addr[5]);
        if (strcmp(a, addr) == 0) { return i; }
    }
    return -1;
}
#endif

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

/* Undo whatever the outstanding query had already recorded, and rewind the
 * cursor to it, so that re-issuing the same step cannot append a second copy
 * of a partial result. Every assignment is a no-op when nothing was recorded,
 * which is the case when the stack refused the call outright. */
static void disc_drop_partial(le_link_t *l) {
    gattdb_t *db = &l->db;
    switch (l->disc_inflight) {
        case DISC_IN_PRIMARY:
            db->n_svc = 0; db->n_chr = 0; db->n_dsc = 0;
            l->svc_i = 0; l->chr_i = 0; l->svc_issued = 0;
            break;
        case DISC_IN_CHR: {
            if (l->svc_i >= db->n_svc) { break; }
            gdb_service_t *s = &db->svc[l->svc_i];
            db->n_chr = s->first_chr;
            s->n_chr  = 0;
            l->svc_issued = l->svc_i;      /* ask for this service again */
            break;
        }
        case DISC_IN_DSC: {
            /* discover_next advances chr_i BEFORE issuing, so the in-flight
             * characteristic is the one behind the cursor. */
            if (l->chr_i == 0 || l->svc_i >= db->n_svc) { break; }
            gdb_service_t *s = &db->svc[l->svc_i];
            l->chr_i--;
            gdb_char_t *ch = &db->chr[s->first_chr + l->chr_i];
            db->n_dsc = ch->first_dsc;
            ch->n_dsc = 0;
            ch->cccd_handle = 0;
            break;
        }
        default: break;
    }
    l->disc_inflight = DISC_IN_NONE;
}

/* A cascade that cannot finish must say so. Leaving db.ready false with no
 * event is what made the original bug invisible: the script waited out its
 * own timeout with nothing in the log to say why. */
static void discovery_failed(le_link_t *l, const char *reason, uint8_t att_status) {
    printf("bt: discovery failed on %s: %s (att 0x%02x)\n",
           bd_addr_to_str(l->addr), reason, att_status);
    char ev[144];
    snprintf(ev, sizeof ev,
             "{\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
             "\"reason\":\"%s\",\"att_error\":%u}",
             l->addr[0], l->addr[1], l->addr[2], l->addr[3], l->addr[4], l->addr[5],
             reason, att_status);
    api_emit("gatt.failed", ev);
    l->disc_stage    = DISC_IDLE;
    l->disc_inflight = DISC_IN_NONE;
    l->disc_deadline_ms = 0;
}

/* Characteristics of svc_i, then descriptors of each, then the next service.
 *
 * NOTE on svc_issued: every service arrives with n_chr == 0, so the old loop
 * tested "chr_i < s->n_chr" as its first act, found 0 < 0 false, and advanced
 * svc_i -- meaning service 0's characteristics were NEVER requested. It went
 * unnoticed only because index 0 is usually GAP. On a device with a single
 * primary service the cascade "completed" in one round trip with 0 chr, which
 * is indistinguishable from a stall from Lua's side. svc_issued records how
 * many services have had their characteristic query issued, so service 0 is
 * asked for like any other.
 */
static void discover_next(le_link_t *l) {
    gattdb_t *db = &l->db;

    while (l->svc_i < db->n_svc) {
        gdb_service_t *s = &db->svc[l->svc_i];

        if (l->svc_issued <= l->svc_i) {
            l->bt_svc.start_group_handle = s->start_handle;
            l->bt_svc.end_group_handle   = s->end_handle;
            s->first_chr = db->n_chr;
            s->n_chr = 0;
            l->svc_issued = (uint8_t)(l->svc_i + 1);
            l->disc_inflight = DISC_IN_CHR;
            uint8_t st = gatt_client_discover_characteristics_for_service(
                             gatt_handler, l->con_handle, &l->bt_svc);
            if (st != ERROR_CODE_SUCCESS) {
                disc_drop_partial(l);        /* rewinds svc_issued to svc_i */
                l->disc_stage = DISC_NEXT;
            }
            return;
        }

        if (l->chr_i < s->n_chr) {
            uint8_t ci = (uint8_t)(s->first_chr + l->chr_i);
            gdb_char_t *ch = &db->chr[ci];
            l->bt_chr.start_handle = ch->value_handle;
            l->bt_chr.value_handle = ch->value_handle;
            /* Descriptors of characteristic k run from its value handle + 1 up
             * to just before the NEXT characteristic's DECLARATION, which sits
             * one handle below that characteristic's value handle. Stopping at
             * next.value_handle - 1 instead swallows that declaration and
             * reports 0x2803 as though it were a descriptor. */
            l->bt_chr.end_handle   = (uint8_t)(l->chr_i + 1) < s->n_chr
                                   ? (uint16_t)(db->chr[ci + 1].value_handle - 2)
                                   : s->end_handle;
            l->bt_chr.properties   = ch->properties;
            ch->first_dsc = db->n_dsc;
            l->chr_i++;
            if (l->bt_chr.end_handle > l->bt_chr.value_handle) {
                l->disc_inflight = DISC_IN_DSC;
                uint8_t st = gatt_client_discover_characteristic_descriptors(
                                 gatt_handler, l->con_handle, &l->bt_chr);
                if (st != ERROR_CODE_SUCCESS) {
                    disc_drop_partial(l);        /* rewinds chr_i to this one */
                    l->disc_stage = DISC_NEXT;
                }
                return;
            }
            l->disc_inflight = DISC_IN_NONE;
            continue;   /* no room for descriptors between this and the next */
        }

        l->svc_i++;
        l->chr_i = 0;
    }

    /* Whole cascade complete. A real peripheral always exposes at least GAP
     * (0x1800), so zero services means the cascade fell through rather than
     * finished -- the exact false success this bug produced. Refuse to mark
     * an empty database ready. */
    if (db->n_svc == 0) {
        discovery_failed(l, "no services", l->disc_err);
        return;
    }
    l->disc_stage = DISC_IDLE;
    l->disc_inflight = DISC_IN_NONE;
    l->disc_deadline_ms = 0;
    db->ready = true;
    gatt_client_get_mtu(l->con_handle, &db->mtu);

    char *tree = api_scratch();
    const size_t tree_sz = api_scratch_size();
    size_t n = (size_t)snprintf(tree, tree_sz,
        "{\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"gatt\":",
        l->addr[0], l->addr[1], l->addr[2], l->addr[3], l->addr[4], l->addr[5]);
    n += gattdb_summary_json(db, tree + n, tree_sz - n);
    snprintf(tree + n, tree_sz - n, "}");
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
            db->svc[l->svc_issued - 1].n_chr++;   /* the service we asked about */
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
            uint16_t h = gatt_event_query_complete_get_handle(pkt);
            uint8_t att_status = gatt_event_query_complete_get_att_status(pkt);
            l = link_by_handle(h);
            if (!l) { return; }
            /* The peripheral refusing for want of security is the ONLY thing
             * that triggers pairing under the default policy -- we do not ask
             * to bond with everything we connect to. */
            if (bond_att_error_wants_security(att_status)) {
                char extra[48];
                snprintf(extra, sizeof(extra), "{\"att_error\":%u,\"policy\":\"%s\"}",
                         att_status, bond_policy_name(bond_policy()));
                api_emit("security.required", extra);
                if (bond_policy() != BOND_POLICY_NEVER) {
                    printf("bt: att 0x%02x wants security; requesting pairing\n", att_status);
                    bond_request(h);
                }
                /* Park the cascade rather than abandon it. The old comment
                 * here said "the caller retries once the link is secured" and
                 * nothing did: the cascade simply stopped. le_on_link_secured
                 * (called from bond.c) resumes it, and the deadline still
                 * runs, so a pairing that never completes fails loudly. */
                disc_drop_partial(l);
                l->disc_err   = att_status;
                l->disc_stage = DISC_SECURITY;
                return;
            }
            if (att_status != ATT_ERROR_SUCCESS) {
                /* Any other error used to fall straight through to
                 * discover_next(), which is how a failed primary-service query
                 * produced an EMPTY database that was then marked ready:
                 * 0 services, 0 characteristics, ready = true, and bt.find
                 * failing forever with nothing logged. An error is not
                 * progress. Note BTstack maps ATTRIBUTE_NOT_FOUND -- the normal
                 * end of a discovery walk -- to SUCCESS for exactly these
                 * queries (gatt_client.c), so anything non-zero here is real.
                 *
                 * How bad it is depends on the step. Losing the service list
                 * leaves nothing usable, and an ATT timeout means the link is
                 * gone; either is fatal. Losing one characteristic's
                 * descriptors costs a CCCD at worst, so skip that unit and
                 * keep the rest of the database. */
                uint8_t step = l->disc_inflight;
                l->disc_err = att_status;
                printf("bt: att error 0x%02x on discovery step %u for %s\n",
                       att_status, step, bd_addr_to_str(l->addr));
                if (step == DISC_IN_PRIMARY || att_status == ATT_ERROR_TIMEOUT) {
                    discovery_failed(l,
                        step == DISC_IN_PRIMARY ? "primary services" : "att timeout",
                        att_status);
                    return;
                }
                disc_drop_partial(l);
                if (step == DISC_IN_CHR) {
                    l->svc_i++;                        /* skip the whole service */
                    l->chr_i = 0;
                    l->svc_issued = l->svc_i;
                } else {
                    l->chr_i++;                        /* skip just this one */
                }
                discover_next(l);
                return;
            }
            l->disc_inflight = DISC_IN_NONE;
            if (!l->db.ready) {
                discover_next(l);
            }
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
    l->svc_issued = 0;
    l->disc_err = 0;
    l->disc_inflight = DISC_IN_PRIMARY;
    l->disc_deadline_ms = now_ms() + DISC_TIMEOUT_MS;

    /* Guarded like the value operations are (see le_read/le_write), because
     * this is called straight out of le_on_connected and that handle's client
     * is not necessarily ready yet. Refusal is an ordinary outcome, not an
     * error: the tick re-issues it. */
    if (!gatt_client_is_ready(l->con_handle) ||
        gatt_client_discover_primary_services(gatt_handler, l->con_handle)
            != ERROR_CODE_SUCCESS) {
        l->disc_stage = DISC_PRIMARY;
        return;
    }
    /* IDLE means "nothing to re-issue": from here the packet handler drives
     * the cascade. Only a REFUSAL arms a retry stage -- setting one here would
     * have the tick call discover_next behind the callbacks' backs and walk
     * the cursor twice. */
    l->disc_stage = DISC_IDLE;
}

static void discovery_tick(void) {
    /* Re-issue any discovery step that was refused, and fail loudly if a
     * cascade stops progressing. Both halves matter: without the retry a
     * second link never finishes discovering, and without the deadline a
     * cascade that cannot proceed hangs silently until a script gives up. */
    for (int i = 0; i < CONFIG_BT_MAX_LINKS; i++) {
        le_link_t *l = &links[i];
        if (!l->active || l->db.ready) { continue; }
        if (!l->disc_deadline_ms) { continue; }

        if ((int32_t)(now_ms() - l->disc_deadline_ms) >= 0) {
            discovery_failed(l,
                             l->disc_stage == DISC_SECURITY ? "pairing timeout"
                                                            : "stalled",
                             l->disc_err);
            continue;
        }
        /* DISC_SECURITY is waiting on pairing, not on the client being free:
         * re-issuing would just earn the same refusal. le_on_link_secured
         * moves it on, and the deadline above still applies if that never
         * comes. */
        if (l->disc_stage == DISC_IDLE || l->disc_stage == DISC_SECURITY ||
            !gatt_client_is_ready(l->con_handle)) {
            continue;
        }
        if (l->disc_stage == DISC_PRIMARY) {
            l->disc_inflight = DISC_IN_PRIMARY;
            if (gatt_client_discover_primary_services(gatt_handler, l->con_handle)
                    == ERROR_CODE_SUCCESS) {
                l->disc_stage = DISC_IDLE;   /* callbacks take it from here */
            }
        } else {
            /* DISC_NEXT with a pending retry: discover_next re-arms the stage
             * itself if it is refused again. */
            l->disc_stage = DISC_IDLE;
            discover_next(l);
        }
    }
}

/* Called from bond.c when pairing finishes, either way. This is the signal
 * the parked cascade waits for: without it a peripheral that refuses
 * discovery until the link is encrypted is discovered exactly never. */
void le_on_link_secured(uint16_t con_handle, bool ok) {
    le_link_t *l = link_by_handle(con_handle);
    if (!l || l->disc_stage != DISC_SECURITY) { return; }
    if (!ok) {
        discovery_failed(l, "pairing failed", l->disc_err);
        return;
    }
    /* Re-issue the step the peripheral refused. disc_drop_partial already
     * rewound the cursor to it, so DISC_NEXT resumes exactly there -- unless
     * it was the service list, which has to start over. */
    l->disc_stage = (l->db.n_svc == 0) ? DISC_PRIMARY : DISC_NEXT;
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
#if CONFIG_LUA_ENABLE
    /* A new generation for this slot, so a frame queued by the PREVIOUS
     * connection can never be delivered to this one. */
    lwait_link_up((uint8_t)link_index(l));
#endif
    emit_device_event("dev.connected", addr);

    le_register_notifications(con_handle);

    /* Discovery starts immediately; the MTU is read once it completes, because
     * BTstack negotiates it as part of the first GATT procedure. */
    start_discovery(l);
}

void le_on_disconnected(uint16_t con_handle) {
    le_link_t *l = link_by_handle(con_handle);
    uint8_t addr[BT_ADDR_LEN] = {0};
    if (l) {
        memcpy(addr, l->addr, BT_ADDR_LEN);
#if CONFIG_LUA_ENABLE
        /* Before the slot is cleared, and before anything else: this is what
         * unblocks a coroutine waiting on an event that is now never coming.
         * A deadline alone would leave it blocked for the rest of its
         * timeout, which for a handshake step is the difference between
         * failing in 200 ms and failing in 20 s. */
        lwait_link_down((uint8_t)link_index(l));
#endif
        memset(l, 0, sizeof(*l));
    }
    bt_device_t *d = bt_registry_by_handle(con_handle);
    if (d) {
        d->connected = false;
        d->con_handle = 0;
        memcpy(addr, d->addr, BT_ADDR_LEN);
    }
#if CONFIG_LUA_ENABLE
    /* Drop anything still queued for this device. Without it a reconnect to
     * the same address is handed frames from the previous connection, which a
     * profile has no way to tell apart from fresh ones. (A generation counter
     * is the fuller answer; this closes the common case now.) */
    {
        char a[18];
        snprintf(a, sizeof a, "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        le_notify_flush(a);
    }
#endif
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

/* ===================== read / write / subscribe ============================
 *
 * Every operation here addresses a characteristic by its index in our own
 * database, never by an ATT handle from the client. The handle is looked up on
 * this side at the last possible moment.
 */

/* Notifications are delivered through one registration per link, made when the
 * link is established rather than per subscription: BTstack routes every
 * notification on a connection to the same handler, and the value handle in the
 * event is what identifies the characteristic. */
static gatt_client_notification_t notif_reg[CONFIG_BT_MAX_LINKS];


/* --- the notification inbox ------------------------------------------------
 * See central.h for why this exists alongside the char.value event.
 *
 * Sized for the job rather than generously: a handshake exchanges one frame at
 * a time, and the largest thing that arrives unsolicited is a telemetry report
 * of ~208 bytes, fragmented across notifications that are each MTU-bounded.
 * Eight slots of 256 bytes is 2 KB and covers a whole fragmented report
 * arriving while a script is between resumes.
 *
 * Overflow drops the OLDEST. A script that has stopped draining is not coming
 * back for the backlog, and holding it would only delay the fresh frame it
 * will actually want. The drop is counted so the condition is visible rather
 * than silent. */
#if CONFIG_LUA_ENABLE
#define NOTIFY_SLOTS 8
#define NOTIFY_VAL_MAX 256

/* Entries are addressed by DEVICE, not taken blindly off the front.
 *
 * They used to be a plain FIFO that any script could drain. With one profile
 * running that is fine; with two it is not, and the failure was ugly in both
 * directions -- the charger's profile discarded the iron's read results, so
 * those timed out, while the iron's profile called a global flush before every
 * read and so wiped the charger's pending key exchange. The handshake failed
 * at "no reply to 0x0021" with nothing obviously wrong at either end.
 *
 * So a slot is a slot, not a ring position: take() finds the oldest entry for
 * the address asked for and frees only that one, leaving everyone else's
 * alone. Ordering comes from a monotonic sequence rather than from position. */
typedef struct {
    bool     used;
    uint32_t seq;
    char     addr[18];
    char     id[8];
    char     source[10];   /* notify | indicate | read */
    uint16_t len;
    uint8_t  val[NOTIFY_VAL_MAX];
} notify_slot_t;

static notify_slot_t s_inbox[NOTIFY_SLOTS];
static uint32_t s_inbox_seq;
static uint32_t s_inbox_dropped;

static int oldest_slot(const char *addr) {
    int best = -1;
    for (int i = 0; i < NOTIFY_SLOTS; i++) {
        if (!s_inbox[i].used) { continue; }
        if (addr && strcmp(s_inbox[i].addr, addr) != 0) { continue; }
        if (best < 0 || s_inbox[i].seq < s_inbox[best].seq) { best = i; }
    }
    return best;
}

static void notify_push(const char *addr, const char *id, const char *source,
                        const uint8_t *val, uint16_t len) {
    if (len > NOTIFY_VAL_MAX) {
        /* Truncating would hand a script a frame whose checksum cannot pass,
         * which is a worse failure than never seeing it. */
        s_inbox_dropped++;
        return;
    }
    int slot = -1;
    for (int i = 0; i < NOTIFY_SLOTS; i++) {
        if (!s_inbox[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        /* Full: drop the oldest of ANY device. A script that has stopped
         * draining is not coming back for the backlog, and holding it would
         * only delay the fresh frame someone does want. */
        slot = oldest_slot(NULL);
        if (slot < 0) { return; }
        s_inbox_dropped++;
    }
    notify_slot_t *s = &s_inbox[slot];
    s->used = true;
    s->seq = ++s_inbox_seq;
    snprintf(s->addr, sizeof s->addr, "%s", addr);
    snprintf(s->id, sizeof s->id, "%s", id);
    snprintf(s->source, sizeof s->source, "%s", source);
    s->len = len;
    memcpy(s->val, val, len);

    /* Wake whoever is blocked on this device. The inbox still holds the
     * frame -- the waiter drains it on its next resume -- so a script that
     * polls and a script that blocks see exactly the same data. */
    int wslot = le_link_slot(addr);
    if (wslot >= 0) {
        lwait_deliver(le_addr_token(addr), (uint8_t)wslot,
                      lwait_link_generation((uint8_t)wslot));
    }
}

int le_notify_take(const char *addr_filter, char *addr_out, size_t addr_cap,
                   char *id_out, size_t id_cap, char *src_out, size_t src_cap,
                   uint8_t *val, size_t val_cap) {
    int i = oldest_slot(addr_filter && addr_filter[0] ? addr_filter : NULL);
    if (i < 0) { return -1; }
    notify_slot_t *s = &s_inbox[i];
    if (s->len > val_cap) { s->used = false; return -1; }
    if (addr_out) { snprintf(addr_out, addr_cap, "%s", s->addr); }
    if (id_out) { snprintf(id_out, id_cap, "%s", s->id); }
    if (src_out) { snprintf(src_out, src_cap, "%s", s->source); }
    memcpy(val, s->val, s->len);
    s->used = false;
    return (int)s->len;
}

uint32_t le_notify_dropped(void) { return s_inbox_dropped; }

/* NULL or "" flushes everything; an address flushes only that device's, which
 * is what a profile wants before it starts an exchange. */
void le_notify_flush(const char *addr_filter) {
    for (int i = 0; i < NOTIFY_SLOTS; i++) {
        if (!s_inbox[i].used) { continue; }
        if (addr_filter && addr_filter[0] && strcmp(s_inbox[i].addr, addr_filter) != 0) { continue; }
        s_inbox[i].used = false;
    }
}
#endif /* CONFIG_LUA_ENABLE */

static void emit_char_value(le_link_t *l, uint16_t value_handle,
                            const uint8_t *data, uint16_t len, const char *source) {
    gattdb_t *db = &l->db;
    /* Map the value handle back to a path index so no handle escapes. */
    for (uint8_t si = 0; si < db->n_svc; si++) {
        for (uint8_t k = 0; k < db->svc[si].n_chr; k++) {
            uint8_t ci = (uint8_t)(db->svc[si].first_chr + k);
            if (db->chr[ci].value_handle != value_handle) {
                continue;
            }
            char uuid[40];
            gattdb_uuid_str(db, db->chr[ci].uuid, uuid, sizeof(uuid));
            static char ev[1600];
            size_t n = (size_t)snprintf(ev, sizeof(ev),
                "{\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"id\":\"%u.%u\",\"uuid\":\"%s\","
                "\"source\":\"%s\",\"len\":%u,\"value\":\"",
                l->addr[0], l->addr[1], l->addr[2], l->addr[3], l->addr[4], l->addr[5],
                si, k, uuid, source, len);
            for (uint16_t i = 0; i < len && n + 4 < sizeof(ev); i++) {
                n += (size_t)snprintf(ev + n, sizeof(ev) - n, "%02x", data[i]);
            }
            snprintf(ev + n, sizeof(ev) - n, "\"}");
            api_emit("char.value", ev);
#if CONFIG_LUA_ENABLE
            /* Reads are queued alongside notifications, tagged with which
             * they were. The websocket caller of a read already has its
             * reply, but a SCRIPT has no other way to see one -- and a device
             * like the Pinecil exposes no notify characteristic at all, so
             * polling reads is the only way to talk to it. The consumer
             * filters on `source`. */
            {
                char a[18], pid[8];
                snprintf(a, sizeof a, "%02X:%02X:%02X:%02X:%02X:%02X",
                         l->addr[0], l->addr[1], l->addr[2], l->addr[3], l->addr[4],
                         l->addr[5]);
                snprintf(pid, sizeof pid, "%u.%u", si, k);
                notify_push(a, pid, source, data, len);
            }
#endif
            return;
        }
    }
}

static void value_handler(uint8_t type, uint16_t ch, uint8_t *pkt, uint16_t size) {
    (void)type; (void)size;
    switch (hci_event_packet_get_type(pkt)) {
        case GATT_EVENT_NOTIFICATION: {
            le_link_t *l = link_by_handle(gatt_event_notification_get_handle(pkt));
            if (l) {
                emit_char_value(l, gatt_event_notification_get_value_handle(pkt),
                                gatt_event_notification_get_value(pkt),
                                gatt_event_notification_get_value_length(pkt), "notify");
            }
            break;
        }
        case GATT_EVENT_INDICATION: {
            le_link_t *l = link_by_handle(gatt_event_indication_get_handle(pkt));
            if (l) {
                emit_char_value(l, gatt_event_indication_get_value_handle(pkt),
                                gatt_event_indication_get_value(pkt),
                                gatt_event_indication_get_value_length(pkt), "indicate");
            }
            break;
        }
        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
            le_link_t *l = link_by_handle(gatt_event_characteristic_value_query_result_get_handle(pkt));
            if (l) {
                emit_char_value(l, gatt_event_characteristic_value_query_result_get_value_handle(pkt),
                                gatt_event_characteristic_value_query_result_get_value(pkt),
                                gatt_event_characteristic_value_query_result_get_value_length(pkt), "read");
            }
            break;
        }
        default:
            break;
    }
}

void le_register_notifications(uint16_t con_handle) {
    for (int i = 0; i < CONFIG_BT_MAX_LINKS; i++) {
        if (links[i].active && links[i].con_handle == con_handle) {
            gatt_client_listen_for_characteristic_value_updates(
                &notif_reg[i], value_handler, con_handle, NULL);
            return;
        }
    }
}

/* BTstack runs ONE gatt_client procedure at a time per connection. Issuing a
 * second while the first is outstanding fails in a way that looks like the
 * peripheral refused it, so check first and say plainly that we are busy. */
#define LE_BUSY (-4)

int le_read(const uint8_t *addr, uint8_t svc_i, uint8_t chr_i) {
    bt_device_t *d = bt_registry_find(addr, BT_TRANSPORT_LE);
    if (!d || !d->connected) { return -1; }
    le_link_t *l = link_by_handle(d->con_handle);
    if (!l || !l->db.ready) { return -2; }
    if (!gatt_client_is_ready(l->con_handle)) { return LE_BUSY; }
    (void)svc_i;
    return gatt_client_read_value_of_characteristic_using_value_handle(
               value_handler, l->con_handle, l->db.chr[chr_i].value_handle) == 0 ? 0 : -3;
}

int le_write(const uint8_t *addr, uint8_t svc_i, uint8_t chr_i,
             const uint8_t *data, uint16_t len, bool with_response) {
    bt_device_t *d = bt_registry_find(addr, BT_TRANSPORT_LE);
    if (!d || !d->connected) { return -1; }
    le_link_t *l = link_by_handle(d->con_handle);
    if (!l || !l->db.ready) { return -2; }
    if (!gatt_client_is_ready(l->con_handle)) { return LE_BUSY; }
    (void)svc_i;
    uint16_t vh = l->db.chr[chr_i].value_handle;
    uint8_t rc = with_response
        ? gatt_client_write_value_of_characteristic(value_handler, l->con_handle, vh, len, (uint8_t *)data)
        : gatt_client_write_value_of_characteristic_without_response(l->con_handle, vh, len, (uint8_t *)data);
    return rc == 0 ? 0 : -3;
}

int le_subscribe(const uint8_t *addr, uint8_t svc_i, uint8_t chr_i, int mode) {
    bt_device_t *d = bt_registry_find(addr, BT_TRANSPORT_LE);
    if (!d || !d->connected) { return -1; }
    le_link_t *l = link_by_handle(d->con_handle);
    if (!l || !l->db.ready) { return -2; }
    if (!gatt_client_is_ready(l->con_handle)) { return LE_BUSY; }
    (void)svc_i;
    gdb_char_t *c = &l->db.chr[chr_i];

    uint16_t cccd = c->cccd_handle;
    if (cccd == 0) {
        /* Discovery does not always report the CCC descriptor even when the
         * characteristic is notifiable. The declaration, value and CCCD are
         * consecutive in every implementation that gets this wrong, so the
         * next handle is the right guess -- and a wrong guess fails as an ATT
         * error rather than silently doing nothing. */
        cccd = (uint16_t)(c->value_handle + 1);
        printf("bt: no CCCD discovered for chr %u, trying value_handle+1 (0x%04x)\n", chr_i, cccd);
    }
    uint8_t v[2] = { (uint8_t)mode, 0x00 };   /* 1 notify, 2 indicate, 0 off */
    return gatt_client_write_characteristic_descriptor_using_descriptor_handle(
               value_handler, l->con_handle, cccd, sizeof(v), v) == 0 ? 0 : -3;
}
