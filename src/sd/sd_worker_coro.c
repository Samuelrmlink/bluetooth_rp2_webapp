/* sd_worker.h on the firmware: a stackful coroutine (from km003c_rp2_webapp).
 *
 * Context switch: callee-saved registers (r4-r11, and s16-s31 where the
 * core has a hardware FPU in use) are pushed on the current stack, the
 * stack pointer is swapped, and the other side's are popped. Written in
 * Thumb-1 so the same code runs on the RP2040 (Cortex-M0+) and the RP2350
 * (Cortex-M33). Interrupts keep running on whichever stack is current, so
 * the worker stack carries headroom for core0's IRQ frames.
 *
 * Stack safety: the stack is painted at start, its high-water mark is
 * reported by /api/debug/crash, and a guard word at its bottom is checked
 * on every return to the main loop — a hit panics with a clear message
 * instead of silently corrupting the .bss below it. */
#include "sd/sd_worker.h"

#include "autoconf.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#ifndef CONFIG_SD_WORKER_STACK
#define CONFIG_SD_WORKER_STACK 4096
#endif

#define GUARD 0xC0FFEE5Du
#define PAINT 0xA5A5A5A5u

static uint32_t s_stack[CONFIG_SD_WORKER_STACK / 4] __attribute__((aligned(8)));
static uint32_t *s_main_sp;   /* saved main-loop stack pointer while the worker runs */
static uint32_t *s_worker_sp; /* saved worker stack pointer while the main loop runs */
static void (*s_body)(void);
static bool s_in_worker;
static bool s_started;
static struct sdw_stats s_stats;
static uint32_t s_slice_t0; /* when the worker last got the CPU */
static const char *s_phase = "idle";

/* void sdw_switch(uint32_t **save_sp, uint32_t *load_sp) */
__attribute__((naked, noinline)) static void sdw_switch(uint32_t **save_sp __attribute__((unused)), uint32_t *load_sp __attribute__((unused))) {
    /* r0 = save_sp, r1 = load_sp (AAPCS). Naked: asm only, no C here. */
    __asm volatile(
        "push {r4-r7, lr}\n"
        "mov r2, r8\n"
        "mov r3, r9\n"
        "push {r2, r3}\n"
        "mov r2, r10\n"
        "mov r3, r11\n"
        "push {r2, r3}\n"
#if defined(__ARM_FP) && !defined(__SOFTFP__)
        "vpush {s16-s31}\n"
#endif
        "mov r2, sp\n"
        "str r2, [r0]\n"
        "mov sp, r1\n"
#if defined(__ARM_FP) && !defined(__SOFTFP__)
        "vpop {s16-s31}\n"
#endif
        "pop {r2, r3}\n"
        "mov r10, r2\n"
        "mov r11, r3\n"
        "pop {r2, r3}\n"
        "mov r8, r2\n"
        "mov r9, r3\n"
        "pop {r4-r7, pc}\n");
}

static void trampoline(void) {
    s_body(); /* never returns */
    for (;;) {
        sdw_yield();
    }
}

void sdw_start(void (*body)(void)) {
    if (s_started) {
        return;
    }
    s_body = body;
    for (size_t i = 0; i < sizeof(s_stack) / 4; i++) {
        s_stack[i] = PAINT;
    }
    s_stack[0] = GUARD;
    /* An initial frame that sdw_switch's pops turn into a call of
     * trampoline(): r10,r11,r8,r9 then r4-r7,pc (and s16-s31 first, where
     * the FPU registers are switched). */
    uint32_t *top = &s_stack[sizeof(s_stack) / 4];
    uint32_t *sp = top;
    *--sp = (uint32_t)trampoline | 1u; /* pc */
    for (int i = 0; i < 4; i++) {
        *--sp = 0; /* r7..r4 */
    }
    for (int i = 0; i < 4; i++) {
        *--sp = 0; /* r9,r8,r11,r10 */
    }
#if defined(__ARM_FP) && !defined(__SOFTFP__)
    for (int i = 0; i < 16; i++) {
        *--sp = 0; /* s16-s31 */
    }
#endif
    s_worker_sp = sp;
    s_started = true;
}

void sdw_service(void) {
    if (!s_started || s_in_worker) {
        return;
    }
    s_in_worker = true;
    uint32_t t0 = time_us_32();
    s_slice_t0 = t0;
    sdw_switch(&s_main_sp, s_worker_sp);
    uint32_t dt = time_us_32() - t0;
    s_in_worker = false;
    s_stats.slices++;
    if (dt > s_stats.slice_max_us) {
        s_stats.slice_max_us = dt;
        s_stats.slice_max_phase = s_phase;
    }
    s_stats.over_2ms += dt > 2000u;
    s_stats.over_10ms += dt > 10000u;
    s_stats.over_50ms += dt > 50000u;
    if (s_stack[0] != GUARD) {
        panic("sd worker stack overflow (%u bytes is not enough)", (unsigned)sizeof(s_stack));
    }
}

void sdw_kick(void) {
    /* The main loop resumes the worker every pass anyway. */
}

void sdw_yield(void) {
    if (!s_in_worker) {
        return; /* called from the main context (early boot): nothing to yield to */
    }
    sdw_switch(&s_worker_sp, s_main_sp);
}

void sdw_maybe_yield(uint32_t budget_us) {
    if (s_in_worker && time_us_32() - s_slice_t0 >= budget_us) {
        sdw_switch(&s_worker_sp, s_main_sp);
    }
}

uint32_t sdw_slice_us(void) {
    return s_in_worker ? time_us_32() - s_slice_t0 : 0;
}

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
    return (uint32_t)sizeof(s_stack);
}

uint32_t sdw_stack_used(void) {
    size_t i = 1; /* [0] is the guard */
    while (i < sizeof(s_stack) / 4 && s_stack[i] == PAINT) {
        i++;
    }
    return (uint32_t)(sizeof(s_stack) - i * 4);
}

void sdw_get_stats(struct sdw_stats *out, bool reset) {
    *out = s_stats;
    if (reset) {
        memset(&s_stats, 0, sizeof(s_stats));
    }
}

bool sdw_stack_ok(void) {
    return s_stack[0] == GUARD;
}
