#ifndef BTGW_LBIND_H
#define BTGW_LBIND_H

#include <stdbool.h>
#include <stddef.h>
struct lua_State;

/* Install every binding into the shared globals. */
void lbind_open_all(struct lua_State *L);

/* Dispatch to an op a script registered with ops.register(). Returns false if
 * no script has claimed that name. */
struct api_reply_ctx;
bool lbind_dispatch(const char *name, const char *params_json,
                    void (*reply)(const char *, void *), void *ctx, long id);

/* Names registered from Lua, for sys.info. */
size_t lbind_ops_json(char *dst, size_t cap);

#endif
