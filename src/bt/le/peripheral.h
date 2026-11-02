#ifndef BTGW_BT_PERIPHERAL_H
#define BTGW_BT_PERIPHERAL_H

#include "api/api.h"

/* Advertise beside the central. The controller must already be up before
 * advertisements actually start; this only registers for that moment. */
void bt_peripheral_init(const char *name);

/* 0..31 byte payload for the next bt_adv_start. 0 ok, -1 invalid.
 * Does not change an advertisement that is already on. */
int bt_adv_data(const uint8_t *adv, uint16_t adv_len,
                const uint8_t *scan_rsp, uint16_t scan_len);
/* A zero bound is 0x00A0 / 0x00F0 (0.625 ms units). 0 ok, -1 illegal. */
int bt_adv_start(uint16_t interval_min, uint16_t interval_max);
void bt_adv_stop(void);
/* Static random address BTstack sends before advertising. 0 ok, -1 if addr is NULL. */
int bt_adv_set_addr(const uint8_t addr[6]);

/* GAP plus the MCUmgr SMP service, then commit. Replaces the local list. */
void op_bt_smp(const char *params, api_reply_fn reply, void *ctx, long id);

#endif
