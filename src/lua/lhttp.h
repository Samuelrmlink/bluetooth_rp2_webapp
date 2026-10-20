#ifndef BTGW_LHTTP_H
#define BTGW_LHTTP_H

/* The Lua `http` module (lhttp.c) over src/net/httpc.c. */
struct lua_State;
void lhttp_open(struct lua_State *L);
/* Per pass (lrt_tick): drives the client and the to_file writes. */
void lhttp_tick(void);
/* A script is stopping: cancel its requests. */
void lhttp_release(int slot);
/* The Lua VM is restarting: every request and TLS session goes (their memory
 * is in the arena that is about to be reset). */
void lhttp_reset(void);

#endif
