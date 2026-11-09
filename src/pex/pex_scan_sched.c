/*
 * The scan scheduler behind the PEX bt group's pex_bt_scan* and
 * pex_bt_quiet* calls. Apart from pex_gw_bt.c so a host test can drive it
 * against a fake radio and clock (test/host/scan_sched_test.c): it needs
 * only central.h's scan calls, pex_self/pex_info, and the time.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "pex_rt.h"
#include "bt/le/central.h"
#include "pico/time.h"
#include "pex/pex_scan_sched.h"

/* ---- scanning: one scan, shared by priority -------------------------------
 * There is one scan for the whole gateway. Each module holds at most one
 * request (a new call replaces its last one, so nobody queues twice):
 *
 *   high  pex_bt_scan(duration, active): scan now, until stopped or the
 *         duration ends.
 *   low   pex_bt_scan_periodic(burst, period, active): one burst of at most
 *         30 s per period. For a module that can wait a minute to find its
 *         device (anker), so the radio is free between bursts.
 *
 * A quiet slot (pex_bt_quiet) holds every other module's scans off for up
 * to 10 minutes, for a module that needs the radio's throughput (an OTA
 * transfer). Firmware policy grants it: one slot at a time, at most 10
 * minutes from the grant, and after a slot ends the low-priority requests
 * get QUIET_GUARD_MS of scanning before another slot is granted. The
 * holder's own requests still run during its slot.
 *
 * The requests outlive any one scan session: a scan the web page or Lua
 * started, a timeout, or WiFi taking the radio ends that session, and the
 * next poll starts a scan again for whoever still wants one. A scan the web
 * page or Lua started is never stopped from here. */
#define SREQS 8
#define LOW_BURST_MAX_MS 30000u
#define LOW_BURST_MIN_MS 1000u
#define LOW_PERIOD_MIN_MS 10000u
#define QUIET_MAX_MS 600000u
#define QUIET_GUARD_MS 25000u

enum { PRIO_HIGH = 1, PRIO_LOW = 2 };

typedef struct {
    bool used;
    pex_mod_t *mod;              /* NULL: the gateway's own (role console) */
    uint8_t prio, active;
    uint32_t until_ms;           /* high: 0 = until stopped */
    uint32_t burst_ms, period_ms;/* low */
    uint32_t next_ms;            /* low: when the next burst may start */
    uint32_t burst_end_ms;       /* low: 0 = not in a burst */
} sreq_t;

static sreq_t s_req[SREQS];
static uint32_t s_ours_gen;      /* le_scan_gen() of the scan started here; 0 none */
static uint8_t s_ours_active;

static struct {
    bool on;
    pex_mod_t *mod;
    int id;
    uint32_t start_ms, end_ms;
} s_quiet;
static int s_quiet_seq;
static uint32_t s_guard_until_ms;   /* no new quiet slot before this */

static uint32_t sched_now(void) { return to_ms_since_boot(get_absolute_time()); }
static bool after_or_at(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

static const char *mod_name(pex_mod_t *m) {
    if (!m) { return "gateway"; }
    pex_info_t in;
    return (pex_info(m, &in) == PEX_OK && in.name) ? in.name : "?";
}

static sreq_t *req_of(pex_mod_t *m) {
    for (int i = 0; i < SREQS; i++) { if (s_req[i].used && s_req[i].mod == m) { return &s_req[i]; } }
    return NULL;
}

static sreq_t *req_new(pex_mod_t *m) {
    sreq_t *r = req_of(m);
    if (r) { return r; }
    for (int i = 0; i < SREQS; i++) {
        if (!s_req[i].used) { memset(&s_req[i], 0, sizeof s_req[i]); s_req[i].used = true; s_req[i].mod = m; return &s_req[i]; }
    }
    return NULL;
}

/* Does a module other than m still want a scan? The guard after a slot is
 * owed to those modules only: with none waiting, a new slot is granted at
 * once, so back-to-back jobs of one module are not each delayed 25 s. */
static bool others_want(const pex_mod_t *m) {
    for (int i = 0; i < SREQS; i++) {
        if (s_req[i].used && s_req[i].mod != m) { return true; }
    }
    return false;
}

static bool guarding(uint32_t now, const pex_mod_t *m) {
    if (!s_guard_until_ms || after_or_at(now, s_guard_until_ms)) { return false; }
    return others_want(m);
}

static void quiet_end(uint32_t now) {
    if (!s_quiet.on) { return; }
    s_quiet.on = false;
    s_guard_until_ms = others_want(s_quiet.mod) ? now + QUIET_GUARD_MS : 0;
    /* Low-priority requests waited out the slot: each gets a burst of at
     * least the guard length now, before another slot can be granted. */
    for (int i = 0; i < SREQS; i++) {
        sreq_t *r = &s_req[i];
        if (!r->used || r->prio != PRIO_LOW) { continue; }
        uint32_t len = r->burst_ms > QUIET_GUARD_MS ? r->burst_ms : QUIET_GUARD_MS;
        r->burst_end_ms = now + len;
        r->next_ms = now + (r->period_ms > len ? r->period_ms : len);
    }
    printf("pex: quiet slot %d ended; scans resume (no new slot for %u s)\n",
           s_quiet.id, s_guard_until_ms ? (unsigned)(QUIET_GUARD_MS / 1000u) : 0u);
}

/* Is r held off by another module's quiet slot? */
static bool held_off(const sreq_t *r) { return s_quiet.on && r->mod != s_quiet.mod; }

/* Bring the radio in line with the requests. Called from the poll and from
 * every API call, so a request takes effect at once. */
void pex_scan_sched_apply(void) {
    uint32_t now = sched_now();
    if (s_quiet.on && after_or_at(now, s_quiet.end_ms)) { quiet_end(now); }
    bool want = false;
    uint8_t active = 0;
    for (int i = 0; i < SREQS; i++) {
        sreq_t *r = &s_req[i];
        if (!r->used) { continue; }
        if (r->prio == PRIO_HIGH) {
            if (r->until_ms && after_or_at(now, r->until_ms)) { r->used = false; continue; }
            if (held_off(r)) { continue; }
            want = true;
            active |= r->active;
            continue;
        }
        if (held_off(r)) {
            r->burst_end_ms = 0;      /* quiet_end hands it a burst afterwards */
            continue;
        }
        if (r->burst_end_ms && after_or_at(now, r->burst_end_ms)) { r->burst_end_ms = 0; }
        if (!r->burst_end_ms && after_or_at(now, r->next_ms)) {
            r->burst_end_ms = now + r->burst_ms;
            r->next_ms = now + r->period_ms;
        }
        if (r->burst_end_ms) { want = true; active |= r->active; }
    }
    bool ours = s_ours_gen && le_scanning() && le_scan_gen() == s_ours_gen;
    if (want) {
        if (!le_scanning() || (ours && active != s_ours_active)) {
            le_scan_filter_t f;
            memset(&f, 0, sizeof f);
            f.active = active != 0;
            f.rssi_min = -127;
            /* Refused while WiFi holds the radio: the next poll asks again. */
            if (le_scan_start(&f, 0)) { s_ours_gen = le_scan_gen(); s_ours_active = active; }
        }
    } else if (ours) {
        le_scan_stop();
        s_ours_gen = 0;
    }
}

static void req_drop(pex_mod_t *m) {
    sreq_t *r = req_of(m);
    if (r) { r->used = false; }
}

int pex_bt_scan(uint32_t duration_ms, int active) {
    sreq_t *r = req_new(pex_self());
    if (!r) { return PEX_ENOMEM; }
    uint32_t until = duration_ms ? sched_now() + duration_ms : 0;
    /* The same call again (a module's tick re-asserting) keeps its deadline. */
    if (!(r->prio == PRIO_HIGH && r->active == (active != 0) && duration_ms == 0 && r->until_ms == 0)) {
        r->until_ms = until;
    }
    r->prio = PRIO_HIGH;
    r->active = active != 0;
    pex_scan_sched_apply();
    return PEX_OK;
}

int pex_bt_scan_periodic(uint32_t burst_ms, uint32_t period_ms, int active) {
    if (burst_ms < LOW_BURST_MIN_MS) { burst_ms = LOW_BURST_MIN_MS; }
    if (burst_ms > LOW_BURST_MAX_MS) { burst_ms = LOW_BURST_MAX_MS; }
    if (period_ms < LOW_PERIOD_MIN_MS) { period_ms = LOW_PERIOD_MIN_MS; }
    if (period_ms < burst_ms + 5000u) { period_ms = burst_ms + 5000u; }
    sreq_t *r = req_new(pex_self());
    if (!r) { return PEX_ENOMEM; }
    /* The same request again keeps its schedule. A new one bursts at once. */
    if (!(r->prio == PRIO_LOW && r->burst_ms == burst_ms && r->period_ms == period_ms && r->active == (active != 0))) {
        r->prio = PRIO_LOW;
        r->burst_ms = burst_ms;
        r->period_ms = period_ms;
        r->active = active != 0;
        r->burst_end_ms = 0;
        r->next_ms = sched_now();
    }
    pex_scan_sched_apply();
    return PEX_OK;
}

void pex_bt_scan_stop(void) {
    req_drop(pex_self());
    pex_scan_sched_apply();
}

int pex_bt_scanning(void) { return le_scanning(); }

int pex_bt_scan_state(void) {
    sreq_t *r = req_of(pex_self());
    if (!r) { return PEX_BT_SCAN_NONE; }
    if (held_off(r)) { return PEX_BT_SCAN_QUIET; }
    if (r->prio == PRIO_LOW && !r->burst_end_ms) { return PEX_BT_SCAN_WAITING; }
    if (!le_scanning()) { return PEX_BT_SCAN_BLOCKED; }
    /* Wanted, but a connect, GATT discovery, or WiFi holds the radio. */
    if (le_bringup_busy()) { return PEX_BT_SCAN_BLOCKED; }
    return PEX_BT_SCAN_ON;
}

int pex_bt_quiet(uint32_t ms) {
    if (!ms) { return PEX_EINVAL; }
    if (ms > QUIET_MAX_MS) { ms = QUIET_MAX_MS; }
    pex_mod_t *m = pex_self();
    uint32_t now = sched_now();
    if (s_quiet.on && after_or_at(now, s_quiet.end_ms)) { quiet_end(now); }
    if (s_quiet.on) {
        if (s_quiet.mod != m) { return PEX_EBUSY; }
        /* The holder asking again extends its own slot. */
        uint32_t cap = s_quiet.start_ms + QUIET_MAX_MS;
        uint32_t end = now + ms;
        s_quiet.end_ms = after_or_at(end, cap) ? cap : end;
        return s_quiet.id;
    }
    if (guarding(now, m)) { return PEX_EAGAIN; }
    if (++s_quiet_seq <= 0) { s_quiet_seq = 1; }
    s_quiet.on = true;
    s_quiet.mod = m;
    s_quiet.id = s_quiet_seq;
    s_quiet.start_ms = now;
    s_quiet.end_ms = now + ms;
    printf("pex: quiet slot %d granted for %u s\n", s_quiet.id, (unsigned)(ms / 1000u));
    pex_scan_sched_apply();
    return s_quiet.id;
}

static bool quiet_mine(int id) {
    return s_quiet.on && s_quiet.id == id && s_quiet.mod == pex_self();
}

int pex_bt_quiet_extend(int id, uint32_t ms) {
    uint32_t now = sched_now();
    if (s_quiet.on && after_or_at(now, s_quiet.end_ms)) { quiet_end(now); pex_scan_sched_apply(); }
    if (!quiet_mine(id)) { return PEX_ENOENT; }
    uint32_t cap = s_quiet.start_ms + QUIET_MAX_MS;
    uint32_t end = now + (ms > QUIET_MAX_MS ? QUIET_MAX_MS : ms);
    s_quiet.end_ms = after_or_at(end, cap) ? cap : end;
    return (int)(s_quiet.end_ms - now);
}

int pex_bt_quiet_release(int id) {
    if (!quiet_mine(id)) { return PEX_ENOENT; }
    quiet_end(sched_now());
    pex_scan_sched_apply();
    return PEX_OK;
}

int pex_bt_quiet_left(int id) {
    uint32_t now = sched_now();
    if (s_quiet.on && after_or_at(now, s_quiet.end_ms)) { quiet_end(now); pex_scan_sched_apply(); }
    if (!s_quiet.on || s_quiet.id != id) { return PEX_ENOENT; }
    return (int)(s_quiet.end_ms - now);
}

/* The schedule, for pex.radio (and the web UI). */
int pex_gw_bt_sched_json(char *buf, size_t cap) {
    uint32_t now = sched_now();
    size_t n = 0;
    int w = snprintf(buf, cap, "{\"scanning\":%s,\"ours\":%s,\"quiet\":",
                     le_scanning() ? "true" : "false",
                     (s_ours_gen && le_scan_gen() == s_ours_gen) ? "true" : "false");
    if (w < 0 || (size_t)w >= cap) { return -1; }
    n = (size_t)w;
    if (s_quiet.on) {
        w = snprintf(buf + n, cap - n, "{\"id\":%d,\"module\":\"%s\",\"left_ms\":%lu}", s_quiet.id,
                     mod_name(s_quiet.mod),
                     (unsigned long)(s_quiet.end_ms - now));
    } else {
        w = snprintf(buf + n, cap - n, "null");
    }
    if (w < 0 || (size_t)w >= cap - n) { return -1; }
    n += (size_t)w;
    uint32_t guard = guarding(now, NULL) ? s_guard_until_ms - now : 0;
    w = snprintf(buf + n, cap - n, ",\"guard_ms\":%lu,\"requests\":[", (unsigned long)guard);
    if (w < 0 || (size_t)w >= cap - n) { return -1; }
    n += (size_t)w;
    bool first = true;
    for (int i = 0; i < SREQS; i++) {
        sreq_t *r = &s_req[i];
        if (!r->used) { continue; }
        w = snprintf(buf + n, cap - n,
                     "%s{\"module\":\"%s\",\"prio\":\"%s\",\"active\":%s,\"burst_ms\":%lu,\"period_ms\":%lu,"
                     "\"in_burst\":%s,\"next_in_ms\":%ld,\"held\":%s}",
                     first ? "" : ",", mod_name(r->mod),
                     r->prio == PRIO_HIGH ? "high" : "low", r->active ? "true" : "false",
                     (unsigned long)r->burst_ms, (unsigned long)r->period_ms,
                     r->burst_end_ms ? "true" : "false",
                     r->prio == PRIO_LOW ? (long)(int32_t)(r->next_ms - now) : 0L,
                     held_off(r) ? "true" : "false");
        if (w < 0 || (size_t)w >= cap - n) { return -1; }
        n += (size_t)w;
        first = false;
    }
    w = snprintf(buf + n, cap - n, "]}");
    if (w < 0 || (size_t)w >= cap - n) { return -1; }
    return (int)(n + (size_t)w);
}

/* Revoke the quiet slot (pex.radio_revoke, the web UI). */
int pex_gw_bt_quiet_revoke(void) {
    if (!s_quiet.on) { return PEX_ENOENT; }
    printf("pex: quiet slot %d revoked\n", s_quiet.id);
    quiet_end(sched_now());
    pex_scan_sched_apply();
    return PEX_OK;
}

void pex_scan_sched_unload(pex_mod_t *m) {
    req_drop(m);
    if (s_quiet.on && s_quiet.mod == m) { quiet_end(sched_now()); }
    pex_scan_sched_apply();
}

