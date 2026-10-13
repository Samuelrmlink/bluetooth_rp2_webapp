#ifndef BTGW_PINS_H
#define BTGW_PINS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The pin broker. Build this before anything that touches a GPIO.
 *
 * Pins are a shared, conflicting resource, and getting it wrong here is not a
 * subtle bug: on a Pico 2 W, GPIO 23/24/25/29 are the CYW43439 interface, so a
 * script that claims one kills Wi-Fi AND Bluetooth -- including the websocket
 * you would use to undo it. Those are permanently unavailable.
 *
 * Every claim has an owner. When a script stops or reloads, everything it
 * claimed is released; without that, a reload-driven edit loop leaks pins until
 * nothing works, which would undermine the whole point of hot-reloading.
 */

#define PIN_COUNT 30

typedef enum {
    PIN_OWNER_NONE = 0,
    PIN_OWNER_SYSTEM,    /* the radio, flash, or a board function */
    PIN_OWNER_API,       /* claimed over the websocket */
    PIN_OWNER_SCRIPT,    /* claimed by a Lua script; released when it stops */
} pin_owner_kind_t;

typedef enum {
    PIN_FN_NONE = 0, PIN_FN_GPIO, PIN_FN_ADC, PIN_FN_PWM,
    PIN_FN_I2C, PIN_FN_SPI, PIN_FN_UART,
} pin_function_t;

void pins_init(void);

/* Claim a pin. owner_id names a script (or "" for the API). Fails if the pin
 * is reserved or already owned, and says which. */
bool pins_claim(uint8_t pin, pin_function_t fn, pin_owner_kind_t kind,
                const char *owner_id, const char **err);
bool pins_release(uint8_t pin, const char **err);

/* Release everything a given owner holds. Called when a script stops. */
int pins_release_owner(const char *owner_id);

bool pins_is_reserved(uint8_t pin);

/* Reserve a pin for a system driver (the W5500's SPI and control pins):
 * PIN_OWNER_SYSTEM, so neither the API nor a script can ever claim it. Safe
 * before pins_init(). False if the pin is already owned by someone else. */
bool pins_reserve(uint8_t pin, pin_function_t fn, const char *owner);
/* Who reserved it ("cyw43", "w5500", ...), or NULL. */
const char *pins_reserved_by(uint8_t pin);
bool pins_owned_by(uint8_t pin, const char *owner_id);

const char *pins_function_name(pin_function_t fn);
size_t pins_map_json(char *dst, size_t cap);

#endif
