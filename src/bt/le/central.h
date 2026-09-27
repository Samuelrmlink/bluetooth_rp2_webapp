#ifndef BTGW_LE_CENTRAL_H
#define BTGW_LE_CENTRAL_H

#include <stdbool.h>
#include <stddef.h>
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

/* Called from bond.c when pairing on a link finishes (ok = success). A
 * cascade parked by an ATT security refusal resumes here; nothing else did,
 * which is why such a peripheral was never discovered at all. */
void le_on_link_secured(uint16_t con_handle, bool ok);



/* Value operations. svc_i is accepted for symmetry and future Classic
 * dispatch; chr_i is the index into the device's characteristic table. */
int le_read(const uint8_t *addr, uint8_t svc_i, uint8_t chr_i);
int le_write(const uint8_t *addr, uint8_t svc_i, uint8_t chr_i,
             const uint8_t *data, uint16_t len, bool with_response);
int le_subscribe(const uint8_t *addr, uint8_t svc_i, uint8_t chr_i, int mode);
void le_register_notifications(uint16_t con_handle);

/* --- the notification inbox ----------------------------------------------
 * Notifications already go out as char.value events, which is right for a
 * browser watching a device but useless to a script that has just written a
 * request and wants the answer. Events are fire-and-forget: a reply that
 * arrives between the write and the wait is simply gone, and a handshake made
 * of request/response pairs cannot be written against that.
 *
 * So arriving values are ALSO queued here, briefly, for a script to drain.
 * The queue is small and drops oldest-first: it is a rendezvous buffer for a
 * script that is actively listening, not a log. A build with no Lua does not
 * have it.
 *
 * `addr_filter` scopes both take and flush to ONE device. Two profiles ran
 * against one global FIFO exactly once: each drained or flushed the other's
 * replies and both failed in ways that pointed nowhere near the cause.
 *
 * `addr_out` gets "AA:BB:..", `id_out` the "svc.chr" path -- no handles, same
 * rule as everywhere else -- and `src_out` "notify", "indicate" or "read".
 * Read results are queued too: a script has no other way to see one, and a
 * peripheral with no notify characteristic (the Pinecil has none) can only be
 * polled. Returns the value length, or -1 when empty. */
int le_notify_take(const char *addr_filter, char *addr_out, size_t addr_cap,
                   char *id_out, size_t id_cap, char *src_out, size_t src_cap,
                   uint8_t *val, size_t val_cap);
/* The link SLOT for an address (-1 if not connected), and the token a script
 * blocks on to await that device. Both feed the wait contract in lua/lwait.h:
 * the slot says which link a wait belongs to, so a disconnect can resolve it. */
int      le_link_slot(const char *addr);
uint32_t le_addr_token(const char *addr);

uint32_t le_notify_dropped(void);
void le_notify_flush(const char *addr_filter);
#endif
