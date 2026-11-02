#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "autoconf.h"
#include "bt/le/bond.h"
#include "bt/le/central.h"
#include "bt/registry.h"
#include "api/api.h"

static btstack_packet_callback_registration_t sm_cb;
static bond_policy_t s_policy = BOND_POLICY_ON_DEMAND;

/* What this gateway says it can do for pairing (the SMP IO capability), and
 * whether it asks for MITM protection. With NoInputNoOutput, or with MITM off
 * on both sides, every pairing is Just Works; a passkey or a numeric
 * comparison only happens when someone asks for MITM and the two sides' IO
 * capabilities allow one. Settings -> Bluetooth picks both; cfg/bt.json keeps
 * them (bt.c). */
static uint8_t s_io = IO_CAPABILITY_NO_INPUT_NO_OUTPUT;
static bool s_mitm = false;
static bond_pairing_hook_t s_hook;

static const struct { const char *name; uint8_t cap; } IO_NAMES[] = {
    { "no_input_no_output", IO_CAPABILITY_NO_INPUT_NO_OUTPUT },
    { "display_only",       IO_CAPABILITY_DISPLAY_ONLY },
    { "display_yes_no",     IO_CAPABILITY_DISPLAY_YES_NO },
    { "keyboard_only",      IO_CAPABILITY_KEYBOARD_ONLY },
    { "keyboard_display",   IO_CAPABILITY_KEYBOARD_DISPLAY },
};

const char *bond_io_name(void) {
    for (size_t i = 0; i < sizeof IO_NAMES / sizeof IO_NAMES[0]; i++) {
        if (IO_NAMES[i].cap == s_io) { return IO_NAMES[i].name; }
    }
    return "no_input_no_output";
}

bool bond_mitm(void) { return s_mitm; }

static void apply_sm(void) {
    sm_set_io_capabilities((io_capability_t)s_io);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING | (s_mitm ? SM_AUTHREQ_MITM_PROTECTION : 0));
}

bool bond_set_io(const char *name, int mitm) {
    if (name) {
        size_t i = 0;
        for (; i < sizeof IO_NAMES / sizeof IO_NAMES[0]; i++) {
            if (strcmp(IO_NAMES[i].name, name) == 0) { break; }
        }
        if (i == sizeof IO_NAMES / sizeof IO_NAMES[0]) { return false; }
        s_io = IO_NAMES[i].cap;
    }
    if (mitm >= 0) { s_mitm = mitm != 0; }
    apply_sm();
    return true;
}

void bond_set_pairing_hook(bond_pairing_hook_t fn) { s_hook = fn; }

bond_policy_t bond_policy(void) { return s_policy; }

const char *bond_policy_name(bond_policy_t p) {
    switch (p) {
        case BOND_POLICY_NEVER:  return "never";
        case BOND_POLICY_ALWAYS: return "always";
        default:                 return "on_demand";
    }
}

bool bond_set_policy(const char *name) {
    if (strcmp(name, "never") == 0)     { s_policy = BOND_POLICY_NEVER;     return true; }
    if (strcmp(name, "on_demand") == 0) { s_policy = BOND_POLICY_ON_DEMAND; return true; }
    if (strcmp(name, "always") == 0)    { s_policy = BOND_POLICY_ALWAYS;    return true; }
    return false;
}

bool bond_att_error_wants_security(uint8_t e) {
    return e == ATT_ERROR_INSUFFICIENT_AUTHENTICATION
        || e == ATT_ERROR_INSUFFICIENT_AUTHORIZATION
        || e == ATT_ERROR_INSUFFICIENT_ENCRYPTION_KEY_SIZE
        || e == ATT_ERROR_INSUFFICIENT_ENCRYPTION;
}

bool bond_request(uint16_t con_handle) {
    if (s_policy == BOND_POLICY_NEVER) {
        return false;
    }
    sm_request_pairing(con_handle);
    return true;
}

static void emit_sec(const char *topic, uint16_t con_handle, const char *extra);

/* Every answer, from whoever gave it, is announced, so a prompt open in a
 * browser closes when a script (or another browser) answered first. */
static void answered(uint16_t con_handle, const char *by) {
    if (s_hook) { s_hook(con_handle, BOND_ANSWERED, 0); }
    char extra[48];
    snprintf(extra, sizeof extra, "\"by\":\"%s\"", by ? by : "api");
    emit_sec("security.answered", con_handle, extra);
}

bool bond_supply_passkey(uint16_t con_handle, uint32_t passkey, const char *by) {
    if (passkey > 999999) { return false; }
    sm_passkey_input(con_handle, passkey);
    answered(con_handle, by);
    return true;
}

bool bond_confirm(uint16_t con_handle, bool accept, const char *by) {
    if (accept) { sm_numeric_comparison_confirm(con_handle); }
    else        { sm_bonding_decline(con_handle); }
    answered(con_handle, by);
    return true;
}

bool bond_decline(uint16_t con_handle, const char *by) {
    sm_bonding_decline(con_handle);
    answered(con_handle, by);
    return true;
}

/* Same weak hook central.c uses. Kinds match pex/host_api.json. */
void le_pex_event(int kind, const uint8_t *addr, int chr, const uint8_t *data, uint16_t len, int status)
    __attribute__((weak));

static void pex_kind(uint16_t con_handle, int kind) {
    if (!le_pex_event) { return; }
    bt_device_t *d = bt_registry_by_handle(con_handle);
    static const uint8_t zero[6];
    le_pex_event(kind, d ? d->addr : zero, -1, NULL, 0, 0);
}

static void emit_sec(const char *topic, uint16_t con_handle, const char *extra) {
    bt_device_t *d = bt_registry_by_handle(con_handle);
    char addr[24] = "unknown";
    if (d) {
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 d->addr[0], d->addr[1], d->addr[2], d->addr[3], d->addr[4], d->addr[5]);
    }
    char json[256];
    snprintf(json, sizeof(json), "{\"addr\":\"%s\"%s%s}", addr, extra ? "," : "", extra ? extra : "");
    api_emit(topic, json);
}

static void sm_handler(uint8_t type, uint16_t ch, uint8_t *pkt, uint16_t size) {
    (void)ch; (void)size;
    if (type != HCI_EVENT_PACKET) {
        return;
    }
    uint16_t h;
    switch (hci_event_packet_get_type(pkt)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            h = sm_event_just_works_request_get_handle(pkt);
            /* Just Works needs no human input. The GATT client and a peripheral
             * Security Request start the procedure inside BTstack, so this is
             * the gate for policy "never": decline instead of confirming. */
            if (s_policy == BOND_POLICY_NEVER) {
                emit_sec("security.required", h, "\"policy\":\"never\"");
                pex_kind(h, 24); /* PEX_BT_PAIR_FAIL */
                sm_bonding_decline(h);
                break;
            }
            sm_just_works_confirm(h);
            emit_sec("security.just_works", h, NULL);
            pex_kind(h, 20); /* PEX_BT_JUST_WORKS */
            break;
        case SM_EVENT_PASSKEY_INPUT_NUMBER:
            h = sm_event_passkey_input_number_get_handle(pkt);
            /* We cannot invent this: it is read off the peripheral (or known
             * to a script) and sent back -- bond.passkey, or bt.passkey/a
             * bt.on_pairing handler in Lua. */
            emit_sec("security.passkey_request", h, "\"action\":\"input\"");
            pex_kind(h, 21); /* PEX_BT_PASSKEY */
            if (s_hook) { s_hook(h, BOND_ASK_INPUT, 0); }
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            h = sm_event_numeric_comparison_request_get_handle(pkt);
            {
                uint32_t num = sm_event_numeric_comparison_request_get_passkey(pkt);
                char extra[64];
                snprintf(extra, sizeof(extra), "\"action\":\"compare\",\"number\":%lu", (unsigned long)num);
                emit_sec("security.numeric_comparison", h, extra);
                pex_kind(h, 22); /* PEX_BT_CONFIRM */
                if (s_hook) { s_hook(h, BOND_ASK_COMPARE, num); }
            }
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            h = sm_event_passkey_display_number_get_handle(pkt);
            {
                /* We show, the peripheral's user types it in. Nothing to answer. */
                uint32_t num = sm_event_passkey_display_number_get_passkey(pkt);
                char extra[64];
                snprintf(extra, sizeof(extra), "\"action\":\"display\",\"number\":%lu", (unsigned long)num);
                emit_sec("security.passkey_display", h, extra);
                pex_kind(h, 25); /* PEX_BT_PASSKEY_DISP */
                if (s_hook) { s_hook(h, BOND_ASK_DISPLAY, num); }
            }
            break;
        case SM_EVENT_PASSKEY_DISPLAY_CANCEL:
            h = sm_event_passkey_display_cancel_get_handle(pkt);
            emit_sec("security.passkey_cancel", h, NULL);
            break;
        case SM_EVENT_PAIRING_COMPLETE: {
            h = sm_event_pairing_complete_get_handle(pkt);
            uint8_t status = sm_event_pairing_complete_get_status(pkt);
            /* Discovery may be parked waiting for exactly this. */
            le_on_link_secured(h, status == ERROR_CODE_SUCCESS);
            if (s_hook) {
                s_hook(h, status == ERROR_CODE_SUCCESS ? BOND_DONE_OK : BOND_DONE_FAILED,
                       sm_event_pairing_complete_get_reason(pkt));
            }
            if (status == ERROR_CODE_SUCCESS) {
                emit_sec("security.bonded", h, NULL);
                pex_kind(h, 23); /* PEX_BT_BONDED */
                printf("bt: bonded\n");
            } else {
                char extra[64];
                snprintf(extra, sizeof(extra), "\"status\":%u,\"reason\":%u", status,
                         sm_event_pairing_complete_get_reason(pkt));
                emit_sec("security.failed", h, extra);
                printf("bt: pairing failed status %u reason %u\n", status,
                       sm_event_pairing_complete_get_reason(pkt));
                pex_kind(h, 24); /* PEX_BT_PAIR_FAIL */
            }
            break;
        }
        default:
            break;
    }
}

void bond_init(void) {
    /* Bonding on, and by default MITM off and no requirement imposed on the
     * peer: we accept whatever security the peripheral asks for rather than
     * demanding our own. bond_set_io changes that.
     * Keys persist in the BTstack flash bank, so a bond survives a reboot. */
    apply_sm();     /* NoInputNoOutput, no MITM until cfg/bt.json says otherwise */

    sm_cb.callback = &sm_handler;
    sm_add_event_handler(&sm_cb);
}

/* le_device_db_count() is how many entries are valid, not the last index.
 * A removed bond leaves a hole, and reading only the first N indices prints
 * those holes (addr_type 0xfe, address all zeros) and skips the real ones. */
static bool bond_addr_real(int type, const uint8_t *addr) {
    static const uint8_t zero[6] = {0};
    return type >= 0 && type != BD_ADDR_TYPE_UNKNOWN && memcmp(addr, zero, 6) != 0;
}

static size_t bond_json_add(char *dst, size_t n, size_t cap, bool *first,
                            int index, const char *addr, int addr_type,
                            const char *transport) {
    int w;
    if (n + 96 >= cap) return n;
    w = snprintf(dst + n, cap - n,
                 "%s{\"index\":%d,\"addr\":\"%s\",\"addr_type\":%d,\"transport\":\"%s\"}",
                 *first ? "" : ",", index, addr, addr_type, transport);
    if (w < 0 || (size_t)w >= cap - n) return n;
    *first = false;
    return n + (size_t)w;
}

size_t bond_list_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "[");
    int max = le_device_db_max_count();
    bool first = true;
    for (int i = 0; i < max && n + 96 < cap; i++) {
        int type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t addr;
        sm_key_t irk;
        le_device_db_info(i, &type, addr, irk);
        if (!bond_addr_real(type, addr)) continue;
        n = bond_json_add(dst, n, cap, &first, i, bd_addr_to_str(addr), type, "le");
    }
#if CONFIG_BT_CLASSIC
    /* Classic keys live in a different TLV database from the LE device DB. */
    {
        btstack_link_key_iterator_t it;
        if (gap_link_key_iterator_init(&it)) {
            bd_addr_t addr;
            link_key_t key;
            link_key_type_t ktype;
            int idx = 0;
            memset(key, 0, sizeof key);
            while (n + 96 < cap &&
                   gap_link_key_iterator_get_next(&it, addr, key, &ktype)) {
                memset(key, 0, sizeof key);
                if (bond_addr_real(0, addr)) {
                    n = bond_json_add(dst, n, cap, &first, idx, bd_addr_to_str(addr), 0, "classic");
                }
                idx++;
            }
            memset(key, 0, sizeof key);
            gap_link_key_iterator_done(&it);
        }
    }
#endif
    return n + (size_t)snprintf(dst + n, cap - n, "]");
}

bool bond_remove(const uint8_t *addr, uint8_t addr_type) {
    bool removed = false;
    int max;
    if (!addr) return false;
    max = le_device_db_max_count();
    for (int i = 0; i < max; i++) {
        int type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t a;
        sm_key_t irk;
        le_device_db_info(i, &type, a, irk);
        if (!bond_addr_real(type, a)) continue;
        if (type == (int)addr_type && memcmp(a, addr, 6) == 0) {
            le_device_db_remove(i);
            removed = true;
        }
    }
    return removed;
}

bool bond_forget(const char *addr_str) {
    bd_addr_t addr;
    bool removed = false;
    int max;
    if (!addr_str || !sscanf_bd_addr(addr_str, addr)) return false;
    max = le_device_db_max_count();
    for (int i = 0; i < max; i++) {
        int type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t a;
        sm_key_t irk;
        le_device_db_info(i, &type, a, irk);
        if (!bond_addr_real(type, a)) continue;
        if (memcmp(a, addr, 6) == 0) {
            le_device_db_remove(i);
            removed = true;
        }
    }
#if CONFIG_BT_CLASSIC
    {
        link_key_t key;
        link_key_type_t ktype = (link_key_type_t)0;
        memset(key, 0, sizeof key);
        if (gap_get_link_key_for_bd_addr(addr, key, &ktype)) {
            gap_drop_link_key_for_bd_addr(addr);
            removed = true;
        }
        memset(key, 0, sizeof key);
    }
#endif
    return removed;
}

void bond_forget_all(void) {
    int max = le_device_db_max_count();
    for (int i = 0; i < max; i++) {
        int type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t a;
        sm_key_t irk;
        le_device_db_info(i, &type, a, irk);
        if (bond_addr_real(type, a)) { le_device_db_remove(i); }
    }
#if CONFIG_BT_CLASSIC
    gap_delete_all_link_keys();
#endif
}
