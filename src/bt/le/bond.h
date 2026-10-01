#ifndef BTGW_BOND_H
#define BTGW_BOND_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Bonding policy.
 *
 * A generic central must NOT demand pairing from everything it connects to.
 * Most peripherals need none, and asking anyway is intrusive, can trigger a
 * pairing prompt on the peer, and fails outright on devices with no input or
 * output. So the default is reactive: connect and read freely, and only pair
 * when the peripheral itself refuses an operation for want of security.
 *
 * ATT tells us exactly when that is -- insufficient authentication (0x05),
 * authorization (0x08), encryption key size (0x0C) or encryption (0x0F). Those
 * are the trigger, and the operation is retried once the link is secured.
 */
typedef enum {
    BOND_POLICY_NEVER = 0,   /* never pair; a refusal stays a refusal */
    BOND_POLICY_ON_DEMAND,   /* default: pair in response to an ATT refusal */
    BOND_POLICY_ALWAYS,      /* pair proactively as soon as a link is up */
} bond_policy_t;

void bond_init(void);
bond_policy_t bond_policy(void);
bool bond_set_policy(const char *name);
const char *bond_policy_name(bond_policy_t p);

/* True when this ATT error means "secure the link and try again". */
bool bond_att_error_wants_security(uint8_t att_error);

/* Ask for pairing on a link. Safe to call more than once. */
bool bond_request(uint16_t con_handle);

/* The SMP IO capability and MITM request (Settings -> Bluetooth).
 * name: no_input_no_output | display_only | display_yes_no | keyboard_only |
 * keyboard_display, or NULL to keep it; mitm: 0/1, or -1 to keep it. */
bool bond_set_io(const char *name, int mitm);
const char *bond_io_name(void);
bool bond_mitm(void);

/* Answers. `by` names who answered ("api", "script"); each is announced as
 * security.answered so an open prompt elsewhere can close. */
bool bond_supply_passkey(uint16_t con_handle, uint32_t passkey, const char *by);  /* 0..999999 */
bool bond_confirm(uint16_t con_handle, bool accept, const char *by);              /* numeric comparison */
bool bond_decline(uint16_t con_handle, const char *by);                           /* refuse to pair */

/* One listener for pairing requests besides the API events: the Lua runtime,
 * which queues them for scripts' bt.on_pairing handlers. Called from the SM
 * event handler, so it must only take a note. */
typedef enum {
    BOND_ASK_INPUT, BOND_ASK_COMPARE, BOND_ASK_DISPLAY,   /* questions */
    BOND_ANSWERED,                                         /* someone answered */
    BOND_DONE_OK, BOND_DONE_FAILED,                        /* pairing finished; number = reason */
} bond_ask_t;
typedef void (*bond_pairing_hook_t)(uint16_t con_handle, bond_ask_t ask, uint32_t number);
void bond_set_pairing_hook(bond_pairing_hook_t fn);

size_t bond_list_json(char *dst, size_t cap);
bool bond_remove(const uint8_t *addr, uint8_t addr_type);

#endif
