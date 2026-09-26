#ifndef BTGW_LRT_H
#define BTGW_LRT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The Lua runtime: one state, a fixed arena, and a cooperative scheduler.
 *
 * The main loop is single-threaded and drives cyw43, lwIP and BTstack, so a
 * script that spins stops the radio and the network together. Two mechanisms
 * keep that from happening, and both must exist before the first script runs:
 * scripts are coroutines resumed in bounded slices, and an instruction-count
 * hook forces a yield (and eventually kills) anything that will not yield on
 * its own. */

void lrt_init(void);
bool lrt_ready(void);

/* Resume whatever is runnable, for at most one slice. Called once per pass. */
void lrt_tick(void);

/* --- script lifecycle ---------------------------------------------------- */
/* Source resolution, in order: RAM draft, staged/<name>.lua, lua/<name>.lua.
 * A draft that fails to load falls back to the tier beneath and reports, so a
 * bad edit never leaves nothing running. */
bool lrt_start(const char *name, const char **err);
bool lrt_stop(const char *name);
bool lrt_reload(const char *name, const char **err);

bool lrt_draft_set(const char *name, const char *src, size_t len, const char **err);
bool lrt_stage(const char *name, const char **err);   /* draft -> staged/ */
bool lrt_commit(const char *name, const char **err);  /* staged -> lua/ */
bool lrt_revert(const char *name);                    /* drop draft, else staged */

/* Run a chunk inside a script's environment; result is rendered into out. */
bool lrt_eval(const char *name, const char *chunk, char *out, size_t cap);

size_t lrt_list_json(char *dst, size_t cap);
size_t lrt_stats_json(char *dst, size_t cap);

#endif
