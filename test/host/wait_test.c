/*
 * Host tests for the wait/wake contract (src/lua/lwait.c).
 *
 * These are the cases the user called out as needing tests, and they are all
 * races: a disconnect that arrives while the coroutine is RUNNABLE rather
 * than blocked, an event from a previous connection arriving after a
 * reconnect, and an expected event that simply never comes. None of them is
 * reachable from a hardware test with any reliability -- you cannot ask a
 * charger to drop its link between two specific instructions -- so the state
 * machine is pure logic over an injected clock and is driven directly here.
 */
#include <stdio.h>
#include <string.h>

#include "lua/lwait.h"

static int failures;
static const char *current;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL [%s]: %s\n", current, (msg)); failures++; } \
} while (0)

#define CHECK_REASON(got, want) do { \
    lwait_reason_t g_ = (got), w_ = (want); \
    if (g_ != w_) { \
        printf("  FAIL [%s]: expected %s, got %s\n", current, \
               lwait_reason_name(w_), lwait_reason_name(g_)); \
        failures++; \
    } \
} while (0)

enum { OWNER = 0, OTHER = 1, LINK = 0, LINK2 = 1 };
#define TOK 0xABCDu

static void begin(const char *name) {
    current = name;
    lwait_reset();
    lwait_link_up(LINK);
    lwait_link_up(LINK2);
}

/* --- the happy path ------------------------------------------------------ */
static void t_delivered(void) {
    begin("delivered");
    CHECK_REASON(lwait_arm(OWNER, TOK, LINK, 1000, 0), LWAIT_PENDING);
    CHECK(lwait_is_armed(OWNER), "should be armed");
    int woke = lwait_deliver(TOK, LINK, lwait_link_generation(LINK));
    CHECK(woke == OWNER, "the right owner should wake");
    lwait_reason_t r;
    CHECK(lwait_take(OWNER, &r), "a resolved wait should be takeable");
    CHECK_REASON(r, LWAIT_DELIVERED);
    CHECK(!lwait_is_armed(OWNER), "taking should clear the slot");
}

/* --- the event that never comes ----------------------------------------- */
static void t_timeout(void) {
    begin("timeout: the event never arrives");
    lwait_arm(OWNER, TOK, LINK, 1000, 5000);
    lwait_expire(5999);
    CHECK_REASON(lwait_state(OWNER), LWAIT_PENDING);
    lwait_expire(6000);
    CHECK_REASON(lwait_state(OWNER), LWAIT_TIMEOUT);
    CHECK(lwait_waited_ms(OWNER, 6000) == 1000, "waited_ms should be the elapsed time");
}

/* --- waiting forever is not expressible --------------------------------- */
static void t_no_unbounded_wait(void) {
    begin("no unbounded wait");
    lwait_arm(OWNER, TOK, LINK, 0, 0);            /* caller named no timeout */
    lwait_expire(LWAIT_DEFAULT_MS - 1);
    CHECK_REASON(lwait_state(OWNER), LWAIT_PENDING);
    lwait_expire(LWAIT_DEFAULT_MS);
    CHECK_REASON(lwait_state(OWNER), LWAIT_TIMEOUT);

    begin("an over-long timeout is clamped");
    lwait_arm(OWNER, TOK, LINK, 10u * 60u * 1000u, 0);   /* asked for 10 min */
    lwait_expire(LWAIT_MAX_MS);
    CHECK_REASON(lwait_state(OWNER), LWAIT_TIMEOUT);
}

/* --- a disconnect while BLOCKED ------------------------------------------ */
static void t_link_lost_while_blocked(void) {
    begin("disconnect while blocked");
    lwait_arm(OWNER, TOK, LINK, 20000, 0);
    lwait_link_down(LINK);
    CHECK_REASON(lwait_state(OWNER), LWAIT_LINK_LOST);
    /* And it must NOT have had to wait out the 20 s deadline to notice. */
    lwait_reason_t r;
    lwait_take(OWNER, &r);
    CHECK_REASON(r, LWAIT_LINK_LOST);
}

/*
 * A disconnect while RUNNABLE. The coroutine was not blocked when the link
 * dropped, so nothing resolved a wait -- and then it arms one. Without sticky
 * link loss it would block on a link that can never answer and burn the whole
 * timeout, which is the failure mode the deadline alone cannot prevent.
 */
static void t_link_lost_while_runnable(void) {
    begin("disconnect while runnable, then arms a wait");
    lwait_link_down(LINK);
    lwait_reason_t r = lwait_arm(OWNER, TOK, LINK, 20000, 0);
    CHECK_REASON(r, LWAIT_LINK_LOST);
    CHECK(!lwait_is_armed(OWNER),
          "a wait that resolves on arming must not block at all");
}

/* --- a stale event after a reconnect ------------------------------------- */
static void t_stale_event_after_reconnect(void) {
    begin("stale event from a previous connection");
    uint32_t old_gen = lwait_link_generation(LINK);
    lwait_arm(OWNER, TOK, LINK, 5000, 0);
    lwait_link_down(LINK);                 /* the wait resolves as link_lost */
    lwait_reason_t r;
    lwait_take(OWNER, &r);
    CHECK_REASON(r, LWAIT_LINK_LOST);

    lwait_link_up(LINK);                   /* reconnected: a new generation */
    CHECK(lwait_link_generation(LINK) != old_gen, "reconnect must bump the generation");
    lwait_arm(OWNER, TOK, LINK, 5000, 100);

    /* A frame queued by the OLD connection now arrives. */
    int woke = lwait_deliver(TOK, LINK, old_gen);
    CHECK(woke < 0, "a frame from the previous connection must not be delivered");
    CHECK_REASON(lwait_state(OWNER), LWAIT_PENDING);

    /* The real one does wake it. */
    woke = lwait_deliver(TOK, LINK, lwait_link_generation(LINK));
    CHECK(woke == OWNER, "a current frame should wake the waiter");
    CHECK_REASON(lwait_state(OWNER), LWAIT_DELIVERED);

    lwait_counters_t c;
    lwait_stats(&c);
    CHECK(c.stale_dropped == 1, "the stale frame should be counted");
}

/* --- cancellation -------------------------------------------------------- */
static void t_cancelled(void) {
    begin("cancelled by a script stop");
    lwait_arm(OWNER, TOK, LINK, 5000, 0);
    lwait_cancel(OWNER);
    CHECK(!lwait_is_armed(OWNER), "cancel must release the slot outright");
    lwait_counters_t c;
    lwait_stats(&c);
    CHECK(c.cancelled == 1, "cancel should be counted");
}

/* --- isolation between scripts ------------------------------------------- */
static void t_owners_are_independent(void) {
    begin("two scripts, two links");
    lwait_arm(OWNER, TOK, LINK,  5000, 0);
    lwait_arm(OTHER, TOK, LINK2, 5000, 0);

    /* Same token, different link: must not cross-wake. This is the shape of
     * the bug where two profiles drained each other's replies. */
    int woke = lwait_deliver(TOK, LINK2, lwait_link_generation(LINK2));
    CHECK(woke == OTHER, "the waiter on LINK2 should wake");
    CHECK_REASON(lwait_state(OWNER), LWAIT_PENDING);
    CHECK_REASON(lwait_state(OTHER), LWAIT_DELIVERED);

    /* Dropping one link must not disturb the other's waiter. */
    lwait_link_down(LINK2);
    CHECK_REASON(lwait_state(OWNER), LWAIT_PENDING);
}

/*
 * Once resolved, a wait keeps its reason. A delivery that lands after a
 * timeout must not silently upgrade it to success: the coroutine has already
 * been told it timed out, and its next resume must agree.
 */
static void t_first_resolution_wins(void) {
    begin("first resolution wins");
    lwait_arm(OWNER, TOK, LINK, 1000, 0);
    lwait_expire(1000);
    CHECK_REASON(lwait_state(OWNER), LWAIT_TIMEOUT);
    int woke = lwait_deliver(TOK, LINK, lwait_link_generation(LINK));
    CHECK(woke < 0, "a late delivery must not resolve an already-timed-out wait");
    CHECK_REASON(lwait_state(OWNER), LWAIT_TIMEOUT);

    begin("a disconnect after a delivery does not overwrite it");
    lwait_arm(OWNER, TOK, LINK, 1000, 0);
    lwait_deliver(TOK, LINK, lwait_link_generation(LINK));
    lwait_link_down(LINK);
    CHECK_REASON(lwait_state(OWNER), LWAIT_DELIVERED);
}

/* --- the clock wraps ----------------------------------------------------- */
static void t_clock_wrap(void) {
    begin("deadline across a 32-bit ms wrap");
    uint32_t near_end = 0xFFFFFF00u;
    lwait_arm(OWNER, TOK, LINK, 1000, near_end);
    lwait_expire(near_end + 999);          /* wraps past 0 */
    CHECK_REASON(lwait_state(OWNER), LWAIT_PENDING);
    lwait_expire(near_end + 1000);
    CHECK_REASON(lwait_state(OWNER), LWAIT_TIMEOUT);
}

/* --- nothing resolves a wait that was never armed ------------------------ */
static void t_unarmed(void) {
    begin("unarmed owners are inert");
    CHECK(!lwait_is_armed(OWNER), "nothing armed yet");
    lwait_expire(100000);
    lwait_link_down(LINK);
    lwait_reason_t r;
    CHECK(!lwait_take(OWNER, &r), "an unarmed owner has nothing to take");
    int woke = lwait_deliver(TOK, LINK, 1);
    CHECK(woke < 0, "delivering to nobody should match nothing");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    t_delivered();
    t_timeout();
    t_no_unbounded_wait();
    t_link_lost_while_blocked();
    t_link_lost_while_runnable();
    t_stale_event_after_reconnect();
    t_cancelled();
    t_owners_are_independent();
    t_first_resolution_wins();
    t_clock_wrap();
    t_unarmed();
    if (failures) {
        printf("wait_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("wait_test: all checks passed\n");
    return 0;
}
