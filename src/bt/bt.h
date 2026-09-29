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
uint32_t bt_unhandled_count(void);   /* HCI events nobody handled; for stat */

#endif
