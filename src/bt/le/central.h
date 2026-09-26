#ifndef BTGW_LE_CENTRAL_H
#define BTGW_LE_CENTRAL_H

#include <stdbool.h>
#include <stdint.h>
#include "bt/registry.h"

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

/* Called once per main-loop pass; ends a timed scan. */
void le_central_tick(void);

#endif
