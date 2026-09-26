#ifndef BTGW_GATTSERVER_H
#define BTGW_GATTSERVER_H

#include <stdint.h>

/* Build and install the local ATT database. Call after l2cap_init() and
 * before hci_power_control(). */
void gatt_server_init(const char *name);

uint16_t gatt_server_db_size(void);

#endif
