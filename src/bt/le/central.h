#ifndef BTGW_LE_CENTRAL_H
#define BTGW_LE_CENTRAL_H

#include <stdbool.h>
#include <stdint.h>
#include "bt/registry.h"
#include "bt/le/gattdb.h"

typedef struct {
    bool     active;               /* active scan -> peers send SCAN_RSP, which
                                    * is usually where the name lives */
    int8_t   rssi_min;             /* 0 = no floor */
    char     name_prefix[16];
    uint8_t  mfg_prefix[8];
    uint8_t  mfg_prefix_len;
    uint16_t uuid16[BT_UUIDS_MAX]; /* ADVERTISED uuids only -- see the .c */
    uint8_t  n_uuid16;
} le_scan_filter_t;

void le_central_init(void);

bool le_scan_start(const le_scan_filter_t *f, uint32_t duration_ms);
void le_scan_stop(void);
bool le_scanning(void);

/* Connect by address. addr_type comes from the scan table -- you cannot assume
 * public, and guessing produces a connect that never completes. */
bool le_connect(const uint8_t *addr, uint8_t addr_type);
bool le_disconnect(const uint8_t *addr);

/* The database for a connected peer, or NULL. */
gattdb_t *le_db_for(uint16_t con_handle);
gattdb_t *le_db_for_addr(const uint8_t *addr);

/* Called once per main-loop pass; ends a timed scan and times out connects. */
void le_central_tick(void);

/* Called by the HCI event handler in bt.c. */
void le_on_connected(uint16_t con_handle, const uint8_t *addr, uint8_t addr_type);
void le_on_disconnected(uint16_t con_handle);



/* Value operations. svc_i is accepted for symmetry and future Classic
 * dispatch; chr_i is the index into the device's characteristic table. */
int le_read(const uint8_t *addr, uint8_t svc_i, uint8_t chr_i);
int le_write(const uint8_t *addr, uint8_t svc_i, uint8_t chr_i,
             const uint8_t *data, uint16_t len, bool with_response);
int le_subscribe(const uint8_t *addr, uint8_t svc_i, uint8_t chr_i, int mode);
void le_register_notifications(uint16_t con_handle);
#endif
