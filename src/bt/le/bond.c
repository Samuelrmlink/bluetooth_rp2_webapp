#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "bt/le/bond.h"
#include "bt/le/central.h"
#include "bt/registry.h"
#include "api/api.h"

static btstack_packet_callback_registration_t sm_cb;
static bond_policy_t s_policy = BOND_POLICY_ON_DEMAND;

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

bool bond_supply_passkey(uint16_t con_handle, uint32_t passkey) {
    sm_passkey_input(con_handle, passkey);
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
            /* We cannot invent this: a human has to read it off the peripheral
             * and send it back with bond.passkey. */
            emit_sec("security.passkey_request", h, "\"action\":\"input\"");
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            h = sm_event_numeric_comparison_request_get_handle(pkt);
            {
                char extra[64];
                snprintf(extra, sizeof(extra), "\"action\":\"compare\",\"number\":%lu",
                         (unsigned long)sm_event_numeric_comparison_request_get_passkey(pkt));
                emit_sec("security.numeric_comparison", h, extra);
            }
            break;
        case SM_EVENT_PAIRING_COMPLETE: {
            h = sm_event_pairing_complete_get_handle(pkt);
            uint8_t status = sm_event_pairing_complete_get_status(pkt);
            /* Discovery may be parked waiting for exactly this. */
            le_on_link_secured(h, status == ERROR_CODE_SUCCESS);
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
    /* Bonding on, MITM off, and no requirement imposed on the peer: we accept
     * whatever security the peripheral asks for rather than demanding our own.
     * Keys persist in the BTstack flash bank, so a bond survives a reboot. */
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

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
