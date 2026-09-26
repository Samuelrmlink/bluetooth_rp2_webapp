#ifndef BTGW_GATTDB_H
#define BTGW_GATTDB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "autoconf.h"

/*
 * A compact mirror of one peer's GATT database.
 *
 * BTstack hands us a full uuid128[16] in every service, characteristic and
 * descriptor -- 22 to 24 bytes each -- which is far too fat to keep for several
 * devices at once. Here the 128-bit UUIDs are interned per device and
 * everything else refers to them by one byte, which brings a typical
 * peripheral (8 services, 40 characteristics, 20 descriptors) to roughly 1 KB.
 *
 * ATT handles live in here but NEVER leave the firmware. Clients address
 * objects two ways, both stable for the life of the connection:
 *   - by path index: "3" service, "3.2" characteristic, "3.2.1" descriptor
 *   - by UUID, with an `instance` to disambiguate a repeated one
 */

#define UUID128_NONE 0xFF

typedef struct {
    uint16_t uuid16;    /* meaningful when u128 == UUID128_NONE */
    uint8_t  u128;      /* else an index into the device's uuid128 pool */
} uuid_ref_t;

typedef struct {
    uuid_ref_t uuid;
    uint16_t   start_handle, end_handle;   /* internal only */
    uint8_t    first_chr, n_chr;
} gdb_service_t;

typedef struct {
    uuid_ref_t uuid;
    uint16_t   value_handle, cccd_handle;  /* internal only */
    uint16_t   properties;
    uint8_t    first_dsc, n_dsc;
} gdb_char_t;

typedef struct {
    uuid_ref_t uuid;
    uint16_t   handle;                     /* internal only */
} gdb_desc_t;

typedef struct {
    bool     in_use;
    uint16_t con_handle;

    /* Cleared while a rebuild is in progress. Anything reading the tables must
     * check it first: discovery spans many async callbacks, and a reader that
     * races the rebuild sees a half-populated table, which presents as an
     * intermittent "characteristic not found". */
    bool     ready;

    gdb_service_t svc[CONFIG_GATT_MAX_SVCS];
    uint8_t       n_svc;
    gdb_char_t    chr[CONFIG_GATT_MAX_CHRS];
    uint8_t       n_chr;
    gdb_desc_t    dsc[CONFIG_GATT_MAX_DSCS];
    uint8_t       n_dsc;

    uint8_t  uuid128[CONFIG_GATT_MAX_UUID128][16];
    uint8_t  n_uuid128;

    uint16_t mtu;
    bool     truncated;   /* ran out of a table; the tree is incomplete */
} gattdb_t;

void gattdb_reset(gattdb_t *db, uint16_t con_handle);

/* Intern a UUID. `uuid16` is used when the 128-bit form is the Bluetooth base
 * UUID; otherwise the 16 bytes are stored (deduplicated). */
uuid_ref_t gattdb_intern(gattdb_t *db, uint16_t uuid16, const uint8_t *uuid128);

/* Render a uuid_ref as "180f" or the full dashed 128-bit form. */
size_t gattdb_uuid_str(const gattdb_t *db, uuid_ref_t u, char *dst, size_t cap);

/* The whole tree, handle-free. */
size_t gattdb_json(const gattdb_t *db, char *dst, size_t cap);

/* Resolve "3", "3.2" or "3.2.1". Returns false when the path does not exist. */
bool gattdb_resolve_chr(const gattdb_t *db, const char *path, uint8_t *svc_i, uint8_t *chr_i);

/* Resolve by UUID pair plus instance (0 = first match). */
bool gattdb_find_chr(const gattdb_t *db, const char *svc_uuid, const char *chr_uuid,
                     int instance, uint8_t *svc_i, uint8_t *chr_i);

#endif
