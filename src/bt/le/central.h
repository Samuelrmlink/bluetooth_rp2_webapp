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

/*
 * Is the BLE side using the radio right now -- scanning, or holding a link?
 *
 * Exists because WiFi has to ask. The CYW43439 carries WiFi and Bluetooth
 * over one interface, and a WiFi join issued while BLE is active wedges the
 * driver hard enough to need a reset (see src/net/wifi.c).
 */
bool le_radio_busy(void);

/* True while a connect is outstanding, or any live link is still walking
 * GATT. A link whose database is ready does not count, and neither does a
 * link whose discovery already failed: the application handshake after a
 * ready database is le_negotiation_pending(), and le_connect does not look
 * at that. Anker's second slot connects only after its own handshake
 * releases, which is after discovery and before the first telemetry report.
 * Holding le_connect across that window would refuse the slot and the
 * driver would count the refusal as a failed attempt. */
bool le_bringup_busy(void);

/* True for a few tens of seconds after a link's database becomes ready,
 * until that link drops, a profile telemetry report names it, or the hold
 * expires. The launcher uses this. le_connect does not. */
bool le_negotiation_pending(void);

/* api_emit calls this for every event. A profile.*.telemetry report ends
 * the negotiation hold for the device it names. */
void le_on_profile_telemetry(const char *topic, const char *json);

/*
 * Give the radio up for a WiFi join: stop a scan, cancel a pending connect,
 * disconnect every link. Disconnects complete asynchronously -- poll
 * le_radio_busy() for the result. Returns the number of links asked to close.
 * Profiles see an ordinary link loss and reconnect once WiFi lets them
 * (wifi_bt_hold() refuses new scans/connects meanwhile).
 */
int le_release_radio(const char *why);

/* Open links right now (for measurement; le_radio_busy() is the gate). */
int le_link_count(void);

/* Connect by address. addr_type comes from the scan table -- you cannot assume
 * public, and guessing produces a connect that never completes. Returns false
 * while another connect or a GATT discovery is still in progress. */
bool le_connect(const uint8_t *addr, uint8_t addr_type);
bool le_disconnect(const uint8_t *addr);

/* The database for a connected peer, or NULL. */
gattdb_t *le_db_for(uint16_t con_handle);
gattdb_t *le_db_for_addr(const uint8_t *addr);

/* Called once per main-loop pass; ends a timed scan and times out connects. */
void le_central_tick(void);

/* Called by the HCI event handler in bt.c. */
void le_on_connect_failed(uint8_t status, const uint8_t *addr);
void le_on_connected(uint16_t con_handle, const uint8_t *addr, uint8_t addr_type);
void le_on_disconnected(uint16_t con_handle);

/* Called from bond.c when pairing on a link finishes (ok = success). A
 * cascade parked by an ATT security refusal resumes here; nothing else did,
 * which is why such a peripheral was never discovered at all. */
void le_on_link_secured(uint16_t con_handle, bool ok);



/*
 * Value operations. svc_i is accepted for symmetry and future Classic
 * dispatch; chr_i is the index into the device's characteristic table.
 *
 * THESE ENQUEUE. They do not talk to the radio.
 *
 * BTstack runs one gatt_client procedure at a time per connection, and these
 * used to return LE_BUSY when one was already outstanding -- leaving every
 * caller to invent its own retry. They did so badly: the C1000 profile grew a
 * 2 s retry loop, and the A2687 profile papered over the same race with a
 * sleep. Now the link owns a short queue and the tick issues one operation at
 * a time, so "busy" is no longer a state a caller can observe.
 *
 * Returns 0 when queued, or:
 *   -1  the device is unknown or not connected
 *   -2  no link, discovery has not finished, or chr_i is out of range
 *   -3  bad arguments (a write longer than the ATT MTU)
 *   -5  the queue is full -- the caller is issuing faster than the link drains
 *
 * Completion is asynchronous. A read's value arrives as a char.value event
 * and in the notification inbox below; a failure of any kind now emits
 * gatt.op_failed, where previously a failed write was entirely silent. A
 * write WITHOUT response is the one exception: the ATT layer defines no
 * completion for it, so it is reported as issued and never confirmed.
 */
int le_read(const uint8_t *addr, uint8_t svc_i, uint8_t chr_i);
/* want_done: queue a source="write" inbox frame when the write completes
 * (with response: the ATT response arrives; without: handed to the
 * controller), fails, or times out -- see LE_WRITE_* for its status. */
int le_write(const uint8_t *addr, uint8_t svc_i, uint8_t chr_i,
             const uint8_t *data, uint16_t len, bool with_response, bool want_done);
/* A "write" frame's status: att_error | reason << 8. */
#define LE_WRITE_DONE    0
#define LE_WRITE_TIMEOUT 1
/* The negotiated ATT MTU of a connected peer, 0 before the exchange. */
uint16_t le_mtu(const uint8_t *addr);
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
                   uint8_t *val, size_t val_cap, int *status_out);
/* The link SLOT for an address (-1 if not connected), and the token a script
 * blocks on to await that device. Both feed the wait contract in lua/lwait.h:
 * the slot says which link a wait belongs to, so a disconnect can resolve it. */
int      le_link_slot(const char *addr);
uint32_t le_addr_token(const char *addr);

uint32_t le_notify_dropped(void);
void le_notify_flush(const char *addr_filter);
#endif
