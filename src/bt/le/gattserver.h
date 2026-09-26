#ifndef BTGW_GATTSERVER_H
#define BTGW_GATTSERVER_H

#include <stdint.h>

/* Build and install the local ATT database. Call after l2cap_init() and
 * before hci_power_control(). */
void gatt_server_init(const char *name);

uint16_t gatt_server_db_size(void);

/* --- runtime definition --------------------------------------------------
 * att_db_util builds the database at runtime, so services can be declared
 * from a Lua script (or over the API) with no reflash. Rebuilding is a
 * begin -> add... -> commit cycle rather than incremental, because att_db_util
 * appends into one buffer and has no way to remove an entry.
 *
 * BTstack takes the database pointer once, at att_server_init, and reads it
 * live thereafter -- so a commit swaps the contents underneath it and tells
 * bonded peers via Service Changed, rather than re-initialising the server.
 */
void gatt_server_begin(void);
bool gatt_server_add_service(const char *uuid, const char **err);
bool gatt_server_add_characteristic(const char *uuid, uint16_t properties,
                                    const uint8_t *value, uint16_t len,
                                    uint16_t *out_handle, const char **err);
bool gatt_server_commit(const char **err);

/* The current database, as JSON, so a client can see what a peer would. */
size_t gatt_server_json(char *dst, size_t cap);

#endif
