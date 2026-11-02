#ifndef C5_BLE_PEX_H
#define C5_BLE_PEX_H

#include "pex_rt.h"

/* NimBLE when CONFIG_C5_BLE is on. The bt group still links when it is off. */
void ble_pex_start(void);
void ble_pex_poll(void);
void ble_pex_unload(pex_mod_t *m);

#endif
