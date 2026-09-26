#ifndef BTGW_OPS_H
#define BTGW_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include "api/api.h"

/*
 * The module registry: one C API, two front-ends.
 *
 * Every subsystem -- Bluetooth, and later gpio/i2c/spi/uart/wifi -- registers a
 * namespace, its operations and the event topics it may emit. The JSON
 * dispatcher and (from phase 6) the Lua bindings are both driven from this one
 * table, so adding a module makes it reachable from the browser, from
 * ws_client.py and from a script with no per-front-end work -- and neither
 * front-end can quietly drift ahead of the other.
 *
 * Nothing here knows about JSON or Lua, and nothing here is device-specific.
 */

typedef void (*op_fn)(const char *params_json, api_reply_fn reply, void *ctx, long id);

typedef struct {
    const char *name;      /* fully qualified, e.g. "scan.start" */
    op_fn       fn;
} op_def_t;

typedef struct {
    const char        *ns;
    const op_def_t    *ops;      /* NULL-terminated */
    const char *const *topics;   /* NULL-terminated */
    bool             (*init)(void);
} op_module_t;

/* Call every registered module's init(). Returns false if any refused. */
bool ops_init_all(void);

const op_def_t *ops_lookup(const char *name);

/* ["sys","bt",...] -- what actually linked, so clients discover capabilities
 * rather than assuming them. Every module is independently Kconfig-gated. */
size_t ops_modules_json(char *dst, size_t cap);

/* Helpers the op handlers share. */
void api_reply_ok(api_reply_fn reply, void *ctx, long id, const char *result_json);
void api_reply_err(api_reply_fn reply, void *ctx, long id, const char *code, const char *msg);
bool api_json_str(const char *json, const char *key, char *out, size_t cap);
bool api_json_int(const char *json, const char *key, long *out);
bool api_json_bool(const char *json, const char *key, bool *out);

#endif
