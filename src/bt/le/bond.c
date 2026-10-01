#include <stdio.h>
#include <string.h>

#include "btstack.h"
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
            /* Just Works needs no human input, so auto-accept: the decision
             * about whether to pair at all was already made by the policy. */
            sm_just_works_confirm(h);
            emit_sec("security.just_works", h, NULL);
            break;
        case SM_EVENT_PASSKEY_INPUT_NUMBER:
            h = sm_event_passkey_input_number_get_handle(pkt);
            /* We cannot invent this: it is read off the peripheral (or known
             * to a script) and sent back -- bond.passkey, or bt.passkey/a
             * bt.on_pairing handler in Lua. */
            emit_sec("security.passkey_request", h, "\"action\":\"input\"");
            if (s_hook) { s_hook(h, BOND_ASK_INPUT, 0); }
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            h = sm_event_numeric_comparison_request_get_handle(pkt);
            {
                uint32_t num = sm_event_numeric_comparison_request_get_passkey(pkt);
                char extra[64];
                snprintf(extra, sizeof(extra), "\"action\":\"compare\",\"number\":%lu", (unsigned long)num);
                emit_sec("security.numeric_comparison", h, extra);
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
                printf("bt: bonded\n");
            } else {
                char extra[64];
                snprintf(extra, sizeof(extra), "\"status\":%u,\"reason\":%u", status,
                         sm_event_pairing_complete_get_reason(pkt));
                emit_sec("security.failed", h, extra);
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

size_t bond_list_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "[");
    int count = le_device_db_count();
    bool first = true;
    for (int i = 0; i < count && n + 64 < cap; i++) {
        int type = 0;
        bd_addr_t addr;
        sm_key_t irk;
        le_device_db_info(i, &type, addr, irk);
        if (type < 0) {
            continue;
        }
        n += (size_t)snprintf(dst + n, cap - n,
                              "%s{\"index\":%d,\"addr\":\"%s\",\"addr_type\":%d}",
                              first ? "" : ",", i, bd_addr_to_str(addr), type);
        first = false;
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]");
}

bool bond_remove(const uint8_t *addr, uint8_t addr_type) {
    int count = le_device_db_count();
    for (int i = 0; i < count; i++) {
        int type = 0;
        bd_addr_t a;
        sm_key_t irk;
        le_device_db_info(i, &type, a, irk);
        if (type == addr_type && memcmp(a, addr, 6) == 0) {
            le_device_db_remove(i);
            return true;
        }
    }
    return false;
}
