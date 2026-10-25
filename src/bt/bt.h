#ifndef BTGW_BT_H
#define BTGW_BT_H

#include <stdbool.h>

/* Powers on the controller and registers the transport-neutral device
 * registry. Safe to call once, after cyw43_arch_init(). */
void bt_init(void);

/* Powers the BT controller on once WiFi has had its boot-time turn on the
 * shared radio (see bt.c). Call every loop pass; one-shot internally. */
void bt_power_on_tick(void);

/* True once HCI reports the controller working. */
bool bt_ready(void);

/* True once bt_init() has run, i.e. BTstack exists. False for the whole of a
 * boot whose radio never came up. Callers reached from the main loop must
 * check this before entering BTstack, because the loop ticks them regardless
 * of whether the radio initialised. */
bool bt_stack_up(void);
uint32_t bt_unhandled_count(void);   /* HCI events nobody handled; for stat */

/* One Classic/BNEP line. Always printed on RTT. Also sent to the websocket
 * console as {"evt":"log","data":{"src":"bt",...}} while link logging is on
 * (the default). bt.trace {"link":false} silences the console copy.
 * level is "info", "warn", or "error". */
void bt_log(const char *level, const char *fmt, ...);

/* Short name for an HCI status byte. The caller still prints the hex. */
const char *bt_hci_reason(uint8_t status);

#endif
