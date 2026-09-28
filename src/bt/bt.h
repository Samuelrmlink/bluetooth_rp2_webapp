#ifndef BTGW_BT_H
#define BTGW_BT_H

#include <stdbool.h>

/* Powers on the controller and registers the transport-neutral device
 * registry. Safe to call once, after cyw43_arch_init(). */
void bt_init(void);

/* True once HCI reports the controller working. */
bool bt_ready(void);
uint32_t bt_unhandled_count(void);   /* HCI events nobody handled; for stat */

#endif
