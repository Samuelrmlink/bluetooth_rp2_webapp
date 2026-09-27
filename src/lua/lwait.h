#ifndef BTGW_LWAIT_H
#define BTGW_LWAIT_H

/*
 * The wait/wake contract.
 *
 * Going event-driven removes the liveness guarantee that polling gave away
 * for free: a script that polls bt.notify_take() every 250 ms notices a dead
 * link on its next lap whether or not anything tells it. A script that BLOCKS
 * notices nothing unless something wakes it. So the rules come first, and the
 * dispatch is built on top of them.
 *
 * A wait ends in exactly one of four ways, and THE RESUME ALWAYS CARRIES
 * WHICH. A coroutine is never woken without being told why:
 *
 *   delivered   the expected event arrived
 *   timeout     the deadline passed
 *   link_lost   the owning link dropped
 *   cancelled   the script was stopped or reloaded
 *
 * Both a deadline and a link check are required and neither suffices alone.
 * A timeout alone means a link that dies at t=0 still burns the full budget
 * before anyone notices -- fatal for a handshake with a 6 s allowance. A
 * disconnect alone never fires for a device that stays connected but goes
 * silent, which is exactly what the A2687 does when its auth burst is
 * incomplete.
 *
 * WAITING FOREVER IS NOT EXPRESSIBLE. A wait with no timeout is a bug that
 * hides; a capped wait is a bug that logs. Callers that pass 0 get
 * LWAIT_DEFAULT_MS, and nothing may exceed LWAIT_MAX_MS.
 *
 * LINK LOSS IS STICKY, AND EVENTS CARRY A GENERATION. A disconnect can arrive
 * while the coroutine is runnable rather than blocked; arming a wait on a
 * link that is already down therefore resolves immediately instead of
 * blocking on a link that will never answer. And each connection to a slot
 * gets a new generation, so a frame queued by a previous connection is never
 * delivered to its replacement.
 *
 * This file is pure logic over an injected clock, with no Lua and no BTstack
 * in it, so test/host/wait_test.c can drive deliver / expire / link-lost /
 * cancel and the races between them. That is deliberate: the races are the
 * whole difficulty, and they are unreachable from a hardware test.
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LWAIT_PENDING = 0,
    LWAIT_DELIVERED,
    LWAIT_TIMEOUT,
    LWAIT_LINK_LOST,
    LWAIT_CANCELLED,
} lwait_reason_t;

const char *lwait_reason_name(lwait_reason_t r);

/* The ceiling, and what a caller who names no timeout gets. */
#define LWAIT_MAX_MS      30000u
#define LWAIT_DEFAULT_MS   5000u

#define LWAIT_MAX_OWNERS  8      /* one wait per script; MAX_SCRIPTS in lrt.c */
#define LWAIT_MAX_LINKS   4      /* CONFIG_BT_MAX_LINKS */
#define LWAIT_NO_LINK     0xFFu  /* a wait not tied to any link (a bare timer) */

void lwait_reset(void);

/* --- links --------------------------------------------------------------
 * A link slot's generation increments on every connect, so "which
 * connection" is answerable. lwait_link_down resolves every waiter on that
 * link as link_lost, which is the whole point of the exercise. */
void     lwait_link_up(uint8_t link);
void     lwait_link_down(uint8_t link);
uint32_t lwait_link_generation(uint8_t link);
bool     lwait_link_is_up(uint8_t link);

/* --- waits ---------------------------------------------------------------
 * Arm a wait for `owner` (a script slot). `token` is an opaque match key --
 * in practice a hash of the address and characteristic being awaited.
 *
 * Returns the reason if the wait resolved IMMEDIATELY (the link is already
 * down), in which case nothing is armed and the caller must not block.
 * Otherwise returns LWAIT_PENDING. */
lwait_reason_t lwait_arm(uint8_t owner, uint32_t token, uint8_t link,
                         uint32_t timeout_ms, uint32_t now_ms);

/* Resolve the waiter matching `token` on `link`, if any. `gen` is the link
 * generation the EVENT was produced under: a mismatch means the event belongs
 * to a previous connection and is dropped rather than delivered.
 * Returns the owner woken, or -1 if nothing matched. */
int lwait_deliver(uint32_t token, uint8_t link, uint32_t gen);

/* Resolve every waiter whose deadline has passed. */
void lwait_expire(uint32_t now_ms);

/* Resolve one owner's wait as cancelled (script stopped or reloaded). */
void lwait_cancel(uint8_t owner);

/* Where an owner's wait stands. LWAIT_PENDING covers "no wait armed" too:
 * callers ask lwait_is_armed when the distinction matters. */
lwait_reason_t lwait_state(uint8_t owner);
bool           lwait_is_armed(uint8_t owner);

/* How long this owner has been waiting, for the log line. */
uint32_t lwait_waited_ms(uint8_t owner, uint32_t now_ms);
uint32_t lwait_token(uint8_t owner);
uint8_t  lwait_link(uint8_t owner);

/* Consume a resolved wait, clearing the slot. Returns false while it is still
 * pending or nothing is armed. */
bool lwait_take(uint8_t owner, lwait_reason_t *out);

/* Counters, for sys stats and for tests. */
typedef struct {
    uint32_t armed, delivered, timeout, link_lost, cancelled, stale_dropped;
} lwait_counters_t;
void lwait_stats(lwait_counters_t *out);

#endif
