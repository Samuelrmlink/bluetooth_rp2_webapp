#include <stdio.h>
#include <string.h>

#include "io/pins.h"
#include "app_config.h"
#include "autoconf.h"

typedef struct {
    pin_owner_kind_t kind;
    pin_function_t   fn;
    char             owner[24];
} pin_slot_t;

static pin_slot_t slots[PIN_COUNT];

/* Permanently unavailable on this board. Claiming one of these takes the radio
 * down, and with it both transports and the only way back in. */
static const uint8_t reserved[] = APP_RADIO_PINS;

/* Idempotent: a system driver (the W5500, through spibus) may reserve its
 * pins during network init, before the io module's init runs this from
 * ops_init_all(), and a second call must not wipe those reservations. */
static bool inited;

void pins_init(void) {
    if (inited) { return; }
    inited = true;
    memset(slots, 0, sizeof(slots));
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        uint8_t p = reserved[i];
        if (p < PIN_COUNT) {
            slots[p].kind = PIN_OWNER_SYSTEM;
            slots[p].fn = PIN_FN_NONE;
            snprintf(slots[p].owner, sizeof(slots[p].owner), "cyw43");
        }
    }
    printf("io: pin broker up, %u reserved for the radio\n",
           (unsigned)(sizeof(reserved) / sizeof(reserved[0])));
}

bool pins_reserve(uint8_t pin, pin_function_t fn, const char *owner) {
    pins_init();
    if (pin >= PIN_COUNT) { return false; }
    if (slots[pin].kind == PIN_OWNER_SYSTEM) {
        /* The same driver asking again (a re-init) is fine; anyone else is not. */
        return strcmp(slots[pin].owner, owner ? owner : "") == 0;
    }
    if (slots[pin].kind != PIN_OWNER_NONE) { return false; }
    slots[pin].kind = PIN_OWNER_SYSTEM;
    slots[pin].fn = fn;
    snprintf(slots[pin].owner, sizeof(slots[pin].owner), "%s", owner ? owner : "system");
    printf("io: pin %u reserved for %s\n", pin, slots[pin].owner);
    return true;
}

void pins_unreserve(uint8_t pin, const char *owner) {
    if (pin >= PIN_COUNT || slots[pin].kind != PIN_OWNER_SYSTEM) { return; }
    if (strcmp(slots[pin].owner, owner ? owner : "") != 0) { return; }   /* never the radio's */
    memset(&slots[pin], 0, sizeof(slots[pin]));
}

const char *pins_reserved_by(uint8_t pin) {
    return (pin < PIN_COUNT && slots[pin].kind == PIN_OWNER_SYSTEM) ? slots[pin].owner : NULL;
}

const char *pins_owner(uint8_t pin) {
    pins_init();
    if (pin >= PIN_COUNT || slots[pin].kind == PIN_OWNER_NONE) { return NULL; }
    return slots[pin].owner;
}

bool pins_is_reserved(uint8_t pin) {
    return pin < PIN_COUNT && slots[pin].kind == PIN_OWNER_SYSTEM;
}

const char *pins_function_name(pin_function_t fn) {
    switch (fn) {
        case PIN_FN_GPIO: return "gpio";
        case PIN_FN_ADC:  return "adc";
        case PIN_FN_PWM:  return "pwm";
        case PIN_FN_I2C:  return "i2c";
        case PIN_FN_SPI:  return "spi";
        case PIN_FN_UART: return "uart";
        default:          return "none";
    }
}

bool pins_claim(uint8_t pin, pin_function_t fn, pin_owner_kind_t kind,
                const char *owner_id, const char **err) {
    if (pin >= PIN_COUNT) { *err = "no such pin"; return false; }
    if (slots[pin].kind == PIN_OWNER_SYSTEM) {
        static char rmsg[80];
        snprintf(rmsg, sizeof(rmsg), "pin is reserved by %s and can never be claimed",
                 strcmp(slots[pin].owner, "cyw43") == 0 ? "the radio" : slots[pin].owner);
        *err = rmsg;
        return false;
    }
    if (slots[pin].kind != PIN_OWNER_NONE) {
        static char msg[80];
        snprintf(msg, sizeof(msg), "pin already claimed by %s for %s",
                 slots[pin].owner[0] ? slots[pin].owner : "the api",
                 pins_function_name(slots[pin].fn));
        *err = msg;
        return false;
    }
    slots[pin].kind = kind;
    slots[pin].fn = fn;
    snprintf(slots[pin].owner, sizeof(slots[pin].owner), "%s", owner_id ? owner_id : "");
    return true;
}

bool pins_release(uint8_t pin, const char **err) {
    if (pin >= PIN_COUNT) { *err = "no such pin"; return false; }
    if (slots[pin].kind == PIN_OWNER_SYSTEM) { *err = "pin is reserved"; return false; }
    if (slots[pin].kind == PIN_OWNER_NONE) { *err = "pin is not claimed"; return false; }
    memset(&slots[pin], 0, sizeof(slots[pin]));
    return true;
}

int pins_release_owner(const char *owner_id) {
    int n = 0;
    for (uint8_t p = 0; p < PIN_COUNT; p++) {
        if (slots[p].kind == PIN_OWNER_SCRIPT && strcmp(slots[p].owner, owner_id) == 0) {
            memset(&slots[p], 0, sizeof(slots[p]));
            n++;
        }
    }
    return n;
}

bool pins_owned_by(uint8_t pin, const char *owner_id) {
    return pin < PIN_COUNT && slots[pin].kind != PIN_OWNER_NONE &&
           strcmp(slots[pin].owner, owner_id ? owner_id : "") == 0;
}

size_t pins_map_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "[");
    for (uint8_t p = 0; p < PIN_COUNT && n + 80 < cap; p++) {
        const char *kind = slots[p].kind == PIN_OWNER_SYSTEM ? "reserved"
                         : slots[p].kind == PIN_OWNER_SCRIPT ? "script"
                         : slots[p].kind == PIN_OWNER_API    ? "api" : "free";
        n += (size_t)snprintf(dst + n, cap - n,
                              "%s{\"pin\":%u,\"state\":\"%s\",\"function\":\"%s\",\"owner\":\"%s\"}",
                              p ? "," : "", p, kind, pins_function_name(slots[p].fn),
                              slots[p].owner);
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]");
}
