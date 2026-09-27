#include <string.h>

#include "lua/lwait.h"

typedef struct {
    bool           armed;
    uint32_t       token;
    uint8_t        link;
    uint32_t       gen;         /* link generation when the wait was armed */
    uint32_t       armed_at_ms;
    uint32_t       deadline_ms;
    lwait_reason_t reason;      /* LWAIT_PENDING until it resolves */
} waiter_t;

typedef struct {
    bool     up;
    uint32_t gen;
} link_t;

static waiter_t waits[LWAIT_MAX_OWNERS];
static link_t   links[LWAIT_MAX_LINKS];
static lwait_counters_t counters;

const char *lwait_reason_name(lwait_reason_t r) {
    switch (r) {
        case LWAIT_DELIVERED: return "delivered";
        case LWAIT_TIMEOUT:   return "timeout";
        case LWAIT_LINK_LOST: return "link_lost";
        case LWAIT_CANCELLED: return "cancelled";
        default:              return "pending";
    }
}

void lwait_reset(void) {
    memset(waits, 0, sizeof waits);
    memset(links, 0, sizeof links);
    memset(&counters, 0, sizeof counters);
}

static bool owner_ok(uint8_t owner) { return owner < LWAIT_MAX_OWNERS; }
static bool link_ok(uint8_t link)   { return link  < LWAIT_MAX_LINKS; }

void lwait_link_up(uint8_t link) {
    if (!link_ok(link)) { return; }
    /* A new generation per connection. Without this a frame queued by the
     * previous connection to the same slot is indistinguishable from a fresh
     * one, and a profile has no way to tell them apart. */
    links[link].gen++;
    links[link].up = true;
}

void lwait_link_down(uint8_t link) {
    if (!link_ok(link)) { return; }
    links[link].up = false;
    for (int i = 0; i < LWAIT_MAX_OWNERS; i++) {
        waiter_t *w = &waits[i];
        if (w->armed && w->reason == LWAIT_PENDING && w->link == link) {
            w->reason = LWAIT_LINK_LOST;
            counters.link_lost++;
        }
    }
}

uint32_t lwait_link_generation(uint8_t link) {
    return link_ok(link) ? links[link].gen : 0;
}

bool lwait_link_is_up(uint8_t link) {
    return link_ok(link) ? links[link].up : false;
}

lwait_reason_t lwait_arm(uint8_t owner, uint32_t token, uint8_t link,
                         uint32_t timeout_ms, uint32_t now_ms) {
    if (!owner_ok(owner)) { return LWAIT_CANCELLED; }

    /* The ceiling. A caller that names no timeout does not get to wait
     * forever; it gets a default, and one that asks for more than the maximum
     * is clamped rather than honoured. */
    if (timeout_ms == 0)           { timeout_ms = LWAIT_DEFAULT_MS; }
    if (timeout_ms > LWAIT_MAX_MS) { timeout_ms = LWAIT_MAX_MS; }

    /* Sticky link loss. The disconnect may already have happened while this
     * coroutine was runnable rather than blocked; blocking now would wait on
     * a link that can never answer, for the full timeout. */
    if (link != LWAIT_NO_LINK && (!link_ok(link) || !links[link].up)) {
        counters.link_lost++;
        return LWAIT_LINK_LOST;
    }

    waiter_t *w = &waits[owner];
    w->armed       = true;
    w->token       = token;
    w->link        = link;
    w->gen         = (link == LWAIT_NO_LINK) ? 0 : links[link].gen;
    w->armed_at_ms = now_ms;
    w->deadline_ms = now_ms + timeout_ms;
    w->reason      = LWAIT_PENDING;
    counters.armed++;
    return LWAIT_PENDING;
}

int lwait_deliver(uint32_t token, uint8_t link, uint32_t gen) {
    for (int i = 0; i < LWAIT_MAX_OWNERS; i++) {
        waiter_t *w = &waits[i];
        if (!w->armed || w->reason != LWAIT_PENDING) { continue; }
        if (w->token != token || w->link != link)    { continue; }
        if (w->link != LWAIT_NO_LINK && w->gen != gen) {
            /* Produced by a previous connection to this slot. Delivering it
             * would hand a profile data from a device session it never saw. */
            counters.stale_dropped++;
            continue;
        }
        w->reason = LWAIT_DELIVERED;
        counters.delivered++;
        return i;
    }
    return -1;
}

void lwait_expire(uint32_t now_ms) {
    for (int i = 0; i < LWAIT_MAX_OWNERS; i++) {
        waiter_t *w = &waits[i];
        if (!w->armed || w->reason != LWAIT_PENDING) { continue; }
        if ((int32_t)(now_ms - w->deadline_ms) >= 0) {
            w->reason = LWAIT_TIMEOUT;
            counters.timeout++;
        }
    }
}

void lwait_cancel(uint8_t owner) {
    if (!owner_ok(owner)) { return; }
    waiter_t *w = &waits[owner];
    if (!w->armed) { return; }
    if (w->reason == LWAIT_PENDING) {
        w->reason = LWAIT_CANCELLED;
        counters.cancelled++;
    }
    /* Cancellation is a teardown path: the slot goes away entirely rather
     * than waiting to be consumed by a coroutine that will not run again. */
    memset(w, 0, sizeof *w);
}

lwait_reason_t lwait_state(uint8_t owner) {
    if (!owner_ok(owner) || !waits[owner].armed) { return LWAIT_PENDING; }
    return waits[owner].reason;
}

bool lwait_is_armed(uint8_t owner) {
    return owner_ok(owner) && waits[owner].armed;
}

uint32_t lwait_waited_ms(uint8_t owner, uint32_t now_ms) {
    if (!owner_ok(owner) || !waits[owner].armed) { return 0; }
    return now_ms - waits[owner].armed_at_ms;
}

uint32_t lwait_token(uint8_t owner) {
    return (owner_ok(owner) && waits[owner].armed) ? waits[owner].token : 0;
}

uint8_t lwait_link(uint8_t owner) {
    return (owner_ok(owner) && waits[owner].armed) ? waits[owner].link : LWAIT_NO_LINK;
}

bool lwait_take(uint8_t owner, lwait_reason_t *out) {
    if (!owner_ok(owner)) { return false; }
    waiter_t *w = &waits[owner];
    if (!w->armed || w->reason == LWAIT_PENDING) { return false; }
    if (out) { *out = w->reason; }
    memset(w, 0, sizeof *w);
    return true;
}

void lwait_stats(lwait_counters_t *out) {
    if (out) { *out = counters; }
}
