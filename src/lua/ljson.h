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

/* JSON text -> one Lua value, pushed on the stack.
 *
 * Objects become tables with string keys, arrays tables with 1..n, JSON null
 * json.null (a light userdata NULL, which ljson_encode writes back as null --
 * a nil would silently drop the key or end the array). Numbers that are
 * integral and fit lua_Integer stay integers; anything else is a float, which
 * on the firmware (LUA_32BITS) means single precision. \uXXXX escapes,
 * surrogate pairs included, become UTF-8.
 *
 * Nesting is bounded (LJSON_DECODE_DEPTH) because the parser recurses on
 * core0's 4 KB stack. Returns true with the value pushed, or false with
 * nothing pushed and *err pointing at a static message that names the byte
 * offset ("expected ':' at byte 17"). Trailing whitespace is allowed,
 * trailing anything else is an error. */
#define LJSON_DECODE_DEPTH 32
bool ljson_decode(lua_State *L, const char *s, size_t len, const char **err);

/* Push the json.null sentinel. */
void ljson_push_null(lua_State *L);

#endif
