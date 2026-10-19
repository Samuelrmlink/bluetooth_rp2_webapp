/* The storage worker: the one execution context that ever touches the SD
 * card (FatFs, the card driver), so a slow card never stalls the rest of the
 * main loop -- USB-NCM, WiFi, Bluetooth, HTTP, OTA. Ported from
 * km003c_rp2_webapp (src/log/sd_worker.h, docs/SD_WORKER.md there).
 *
 * Two backends behind this header:
 *   sd_worker_coro.c          firmware: a stackful coroutine. The main loop
 *                             resumes it once a pass (sd_poll()); it runs
 *                             until it yields (card busy, a few ms of work
 *                             done, idle). Cooperative and single-core: no
 *                             locks needed.
 *   test/host/sd_worker_uc.c  host tests: the same semantics on ucontext,
 *                             so the tests are deterministic.
 *
 * Rules: the worker body never returns. Code in the worker may call
 * sdw_yield() anywhere; code in the main context never touches the card.
 */
#ifndef BTGW_SD_WORKER_H
#define BTGW_SD_WORKER_H

#include <stdbool.h>
#include <stdint.h>

/* Starts the worker running `body` (main context, once). */
void sdw_start(void (*body)(void));
/* Main context, once per main-loop pass: let the worker run until it yields.
 * A no-op where the worker is a thread. */
void sdw_service(void);
/* Main context: work was posted for the worker. */
void sdw_kick(void);

/* Worker context: give the main loop a turn (cooperative backends), then
 * carry on. Cheap to call often. */
void sdw_yield(void);
/* Worker context: nothing to do; come back after at most max_us, or sooner
 * when kicked. */
void sdw_idle(uint32_t max_us);
bool sdw_in_worker(void);
/* Worker context: yield only if this slice has already run budget_us. For
 * places that are called often and usually quick (each card read/write),
 * so a long run of them still gives the main loop a turn every few ms. */
void sdw_maybe_yield(uint32_t budget_us);

/* Shared-state lock: a no-op on the cooperative backends. Never held across
 * storage I/O or a yield. */
void sdw_lock(void);
void sdw_unlock(void);

/* Diagnostics: the worker stack's size and high-water mark (0 where it is
 * a thread with an OS stack), and whether its guard word is intact. */
uint32_t sdw_stack_size(void);
uint32_t sdw_stack_used(void);
bool sdw_stack_ok(void);

/* How long the worker kept the main loop waiting: each resume runs it until
 * it yields, and the main loop (USB, WiFi, the live stream) waits that long.
 * Zero where the worker is a thread. `reset` clears the maxima and counts
 * after reading them. */
struct sdw_stats {
    uint32_t slices;
    uint32_t slice_max_us;
    uint32_t over_2ms, over_10ms, over_50ms;
};
void sdw_get_stats(struct sdw_stats *out, bool reset);

#endif
