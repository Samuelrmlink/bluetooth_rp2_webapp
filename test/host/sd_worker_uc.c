/* sd_worker.h for test/host: the firmware's coroutine semantics on
 * ucontext, so a test drives the worker step by step and deterministically
 * (sdw_service() runs it until it yields). */
#define _XOPEN_SOURCE 700
#include "sd/sd_worker.h"

#include <string.h>

#include <stdlib.h>
#include <ucontext.h>

#include "sdcard_sim.h"

#define STACK (256u * 1024u)

/* The work meter: how much the worker does between two yield opportunities
 * (a yield, or an sdw_maybe_yield()/sd_port_maybe_yield() check, where the
 * board yields once its slice has run ~2 ms). Two measures, both
 * deterministic:
 *   calls  firmware functions entered (the firmware sources are built with
 *          -finstrument-functions; test/host is excluded) -- CPU work, e.g.
 *          a FatFs loop that runs 65536 times without touching the card;
 *   us     simulated time that passed -- waits and wire time, e.g. a poll
 *          loop or a delay that never yields.
 * The longest stretch of each is kept, with a call site inside it. */
static uint64_t m_calls, m_t0;
static struct sdw_meter { uint64_t calls_max, us_max; void *calls_site, *us_site; } m_max;
static void *m_site, *m_last_site;

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site);
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *site);
void __cyg_profile_func_enter(void *fn, void *site) {
    (void)fn;
    m_last_site = site;
    if (++m_calls == 4096) { m_site = site; }   /* deep in a long stretch: a site in its loop */
}
void __cyg_profile_func_exit(void *fn, void *site) { (void)fn; (void)site; }

__attribute__((no_instrument_function)) static void meter_checkpoint(void) {
    uint64_t us = sdsim_now_us() - m_t0;
    if (m_calls > m_max.calls_max) { m_max.calls_max = m_calls; m_max.calls_site = m_site ? m_site : m_last_site; }
    if (us > m_max.us_max) { m_max.us_max = us; m_max.us_site = m_last_site; }
    m_calls = 0;
    m_site = NULL;
    m_t0 = sdsim_now_us();
}

/* Test API: the longest stretches since the last reset. */
void sdw_host_meter(uint64_t *calls_max, void **calls_site, uint64_t *us_max, void **us_site, bool reset) {
    *calls_max = m_max.calls_max; *calls_site = m_max.calls_site;
    *us_max = m_max.us_max; *us_site = m_max.us_site;
    if (reset) { memset(&m_max, 0, sizeof m_max); }
}

static ucontext_t s_main, s_worker;
static void (*s_body)(void);
static bool s_in_worker, s_started;
static char *s_stack;

static void entry(void) {
    s_body();
    for (;;) {
        sdw_yield();
    }
}

void sdw_start(void (*body)(void)) {
    if (s_started) {
        return;
    }
    s_body = body;
    s_stack = malloc(STACK);
    getcontext(&s_worker);
    s_worker.uc_stack.ss_sp = s_stack;
    s_worker.uc_stack.ss_size = STACK;
    s_worker.uc_link = NULL;
    makecontext(&s_worker, entry, 0);
    s_started = true;
}

void sdw_service(void) {
    if (!s_started || s_in_worker) {
        return;
    }
    s_in_worker = true;
    m_calls = 0;
    m_t0 = sdsim_now_us();
    swapcontext(&s_main, &s_worker);
    s_in_worker = false;
}

void sdw_kick(void) {
}

void sdw_yield(void) {
    if (s_in_worker) {
        meter_checkpoint();
        swapcontext(&s_worker, &s_main);
    }
}

uint32_t sdw_slice_us(void) {
    return 0;   /* deterministic: never "over budget" (the meter checks instead) */
}

static const char *s_phase = "idle";
const char *sdw_set_phase(const char *phase) {
    const char *was = s_phase;
    s_phase = phase;
    return was;
}

void sdw_idle(uint32_t max_us) {
    (void)max_us;
    sdw_yield();
}

bool sdw_in_worker(void) {
    return s_in_worker;
}

void sdw_lock(void) {
}

void sdw_unlock(void) {
}

uint32_t sdw_stack_size(void) {
    return STACK;
}

uint32_t sdw_stack_used(void) {
    return 0;
}

bool sdw_stack_ok(void) {
    return true;
}

void sdw_get_stats(struct sdw_stats *out, bool reset) {
    (void)reset;
    memset(out, 0, sizeof(*out)); /* not measured here */
}

void sdw_maybe_yield(uint32_t budget_us) {
    /* Deterministic tests: no yield here, but it is where the board would. */
    (void)budget_us;
    if (s_in_worker) { meter_checkpoint(); }
}
