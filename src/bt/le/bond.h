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

/* Answer a security.passkey_request. */
bool bond_supply_passkey(uint16_t con_handle, uint32_t passkey);

size_t bond_list_json(char *dst, size_t cap);
bool bond_remove(const uint8_t *addr, uint8_t addr_type);

#endif
