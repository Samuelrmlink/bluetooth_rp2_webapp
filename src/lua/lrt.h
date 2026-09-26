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

/* For an expensive C binding to call just before it returns, so the main loop
 * gets control back immediately instead of after the caller's next thousand
 * VM instructions. A C function cannot be preempted by the instruction hook
 * -- it is one instruction -- so without this a loop over a slow binding
 * stalls USB, lwIP and BTstack for as long as it runs. Measured at 3 seconds
 * for a loop over P-256, with the runaway killer never firing. */
void lrt_yield_soon(void);

/* --- script lifecycle ---------------------------------------------------- */
/* Source resolution, in order: RAM draft, staged/<name>.lua, lua/<name>.lua.
 * A draft that fails to load falls back to the tier beneath and reports, so a
 * bad edit never leaves nothing running. */
bool lrt_start(const char *name, const char **err);
bool lrt_stop(const char *name);
bool lrt_reload(const char *name, const char **err);

/* Drafts live in RAM and never touch flash -- that is the whole point of the
 * tier. Source arrives in chunks so the per-connection WebSocket buffer can
 * stay small (it is multiplied by the connection count) without capping how
 * large a draft may be: offset 0 starts a new one, later offsets append. */
bool lrt_draft_chunk(const char *name, size_t offset, const char *src, size_t len,
                     const char **err);
bool lrt_draft_set(const char *name, const char *src, size_t len, const char **err);
size_t lrt_draft_len(const char *name);

/* Read back the source currently IN EFFECT -- draft if there is one, else
 * staged, else committed. Paged, for the same reason the upload is: a frame
 * must stay small. Without this a draft is write-only, and pushing one then
 * losing the local copy leaves source on the device that nothing can retrieve.
 *
 * Returns the number of bytes copied; *tier names which tier answered and
 * *total the full length. */
size_t lrt_source_read(const char *name, size_t offset, char *dst, size_t cap,
                       const char **tier, size_t *total);
bool lrt_stage(const char *name, const char **err);   /* draft -> staged/ */
bool lrt_commit(const char *name, const char **err);  /* staged -> lua/ */
bool lrt_revert(const char *name);                    /* drop draft, else staged */

/* Run a chunk inside a script's environment; result is rendered into out. */
bool lrt_eval(const char *name, const char *chunk, char *out, size_t cap);

size_t lrt_list_json(char *dst, size_t cap);
size_t lrt_stats_json(char *dst, size_t cap);

#endif
