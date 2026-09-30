#ifndef BTGW_LRT_H
#define BTGW_LRT_H

/* Before anything that tests a CONFIG_ symbol: this header is included by
 * translation units that pull in autoconf.h only later, and a guard evaluated
 * with the symbol undefined silently drops the declarations behind it. */
#include "autoconf.h"

#include "lua/lwait.h"
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

/* Drop one cached module so its memory can be reclaimed, and force a full
 * collection. A profile uses these to release its handshake once the session
 * is established -- see lrt.c for what the caller must also do. */
bool lrt_unload_module(const char *name);
size_t lrt_collect(void);

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
/* script.loaded: every running script and resident module, with the tier and
 * hash it was compiled from and the hash of its file now. */
size_t lrt_loaded_json(char *dst, size_t cap);

/* The same numbers as lrt_stats_json, as integers, for the MCUmgr stat
 * group; and one script's row for its taskstat. lrt_task_info returns -1
 * past the last slot, 0 for an empty slot, 1 with `out` filled. */
typedef struct { uint32_t arena_bytes, used, peak, hook_kills, slice_overruns, running; } lrt_stats_t;
void lrt_stats(lrt_stats_t *out);
typedef struct { const char *name; uint32_t state, resumes, runtime_ms; } lrt_task_info_t;
int lrt_task_info(int i, lrt_task_info_t *out);

/* --- blocking waits, for the bindings -------------------------------------
 * A binding that must wait for an event arms a wait and then yields with no
 * values. The scheduler will not resume the script until the wait resolves,
 * and the resume carries WHY it resolved (see lwait.h).
 *
 * The shape a binding uses:
 *
 *     lwait_reason_t r = lrt_block_current(token, link, timeout_ms);
 *     if (r != LWAIT_PENDING) { return fail(L, r); }   // resolved already
 *     return lua_yieldk(L, 0, 0, k);                   // k reads lrt_wake_reason()
 */
int            lrt_current_slot(void);

#if CONFIG_BT_LE
/* Record/forget a BLE link against the script that is running right now, so
 * teardown() can drop it. A script is the only thing that can open a link --
 * the JSON API has no connect op -- so if a dying script does not release
 * them, nothing does, and an orphaned link keeps le_radio_busy() true, which
 * makes the one-radio guard refuse every WiFi join until a reboot. */
void lrt_note_link(const uint8_t *addr);
void lrt_forget_link(const uint8_t *addr);
#endif
lwait_reason_t lrt_block_current(uint32_t token, uint8_t link, uint32_t timeout_ms);
lwait_reason_t lrt_wake_reason(void);

#endif
