#ifndef BTGW_LJSON_H
#define BTGW_LJSON_H

#include <stdbool.h>
#include <stddef.h>

#include "lua.h"

/* Encode the Lua value at `idx` as a JSON value into dst[0..cap).
 *
 * Returns the number of bytes written, which is always less than cap, and
 * always leaves dst NUL-terminated. `*truncated` is set when anything had to
 * be dropped -- a clamped string, a skipped key, an array cut short, a table
 * nested past the depth limit.
 *
 * Callers must check it. What a truncated encode leaves behind is not valid
 * JSON (a string can be cut mid-escape, a brace left unclosed), and putting
 * that on the wire reaches the browser as a parse error that points at
 * nothing. Say the event was too large instead.
 *
 * This lives apart from lbind.c so a host test can drive it against the real
 * vendored Lua without the Bluetooth stack, which is how the out-of-bounds
 * write it used to contain is kept fixed. */
size_t ljson_encode(lua_State *L, int idx, char *dst, size_t cap, bool *truncated);

#endif
