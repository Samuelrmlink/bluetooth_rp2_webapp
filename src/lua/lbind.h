#ifndef BTGW_LBIND_H
#define BTGW_LBIND_H

#include <stdbool.h>
#include <stddef.h>
struct lua_State;

/* Install every binding into the shared globals. */
void lbind_open_all(struct lua_State *L);

/* The byte-level protocol primitives (frame/tlv/crypto/bytes). Separate from
 * lbind_open_all because it is compile-gated on CONFIG_PROTO_ANKER: a build
 * that will never talk to Anker hardware should not carry the codec. */
void lbind_proto_open(struct lua_State *L);

/* Dispatch to an op a script registered with ops.register(). Returns false if
 * no script has claimed that name. */
struct api_reply_ctx;
bool lbind_dispatch(const char *name, const char *params_json,
                    void (*reply)(const char *, void *), void *ctx, long id);

/* Names registered from Lua, for sys.info. */
size_t lbind_ops_json(char *dst, size_t cap);
/* Drop a stopping script's claim on its registered ops (lbind.c). */
int lbind_release_ops(int slot);
/* Deliver queued pairing questions to scripts' bt.on_pairing handlers
 * (lrt_tick; a no-op without BLE). */
void lbind_pairing_pump(void);

#endif
