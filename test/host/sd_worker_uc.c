/* sd_worker.h for test/host: the firmware's coroutine semantics on
 * ucontext, so a test drives the worker step by step and deterministically
 * (sdw_service() runs it until it yields). */
#define _XOPEN_SOURCE 700
#include "sd/sd_worker.h"

#include <string.h>

#include <stdlib.h>
#include <ucontext.h>

#define STACK (256u * 1024u)

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
    swapcontext(&s_main, &s_worker);
    s_in_worker = false;
}

void sdw_kick(void) {
}

void sdw_yield(void) {
    if (s_in_worker) {
        swapcontext(&s_worker, &s_main);
    }
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
    (void)budget_us; /* deterministic tests / a real thread: nothing to share */
}
