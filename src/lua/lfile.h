#ifndef BTGW_LFILE_H
#define BTGW_LFILE_H

/* The Lua `file` module (lfile.c): LittleFS and the SD card. */
struct lua_State;
void lfile_open(struct lua_State *L);
/* A script is stopping: close its files, let go of a job it waits for. */
void lfile_release(int slot);
/* The Lua state is going away (VM restart): close every handle. */
void lfile_reset(void);

#endif
