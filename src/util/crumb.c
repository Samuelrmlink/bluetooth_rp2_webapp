/* See crumb.h. */
#include "util/crumb.h"

#include <stdio.h>
#include <string.h>

uint32_t crumb_last, crumb_last_fault_pc;
/* Weak: src/pex/pex_gw.c attributes a fault to a PEX module (pex/ is optional). */
void crumb_fault_hook(const uint32_t *frame) __attribute__((weak));
static crumb_rec_t prev;          /* the previous boot's record, if any */
static int have_prev;

void crumb_boot(void) {
    crumb_last = watchdog_hw->scratch[0];
    crumb_last_fault_pc = watchdog_hw->scratch[1];
    watchdog_hw->scratch[0] = 0;
    watchdog_hw->scratch[1] = 0;
    volatile crumb_rec_t *r = CRUMB_REC;
    if (r->magic == CRUMB_MAGIC) {
        memcpy(&prev, (const void *)r, sizeof prev);
        have_prev = 1;
    }
    memset((void *)r, 0, sizeof *r);
    r->magic = CRUMB_MAGIC;
}

size_t crumb_report_json(char *dst, size_t cap) {
    if (!have_prev) {
        return (size_t)snprintf(dst, cap, "{\"present\":false,\"last_crumb\":%lu,\"last_fault_pc\":%lu}",
                                (unsigned long)crumb_last, (unsigned long)crumb_last_fault_pc);
    }
    size_t n = (size_t)snprintf(dst, cap,
        "{\"present\":true,\"last_crumb\":%lu,\"fault\":%lu,\"pc\":%lu,\"lr\":%lu,\"sp\":%lu,"
        "\"xpsr\":%lu,\"exc_lr\":%lu,\"r0\":%lu,\"r1\":%lu,\"r2\":%lu,\"r3\":%lu,\"r12\":%lu,"
        "\"cfsr\":%lu,\"hfsr\":%lu,\"mmfar\":%lu,\"bfar\":%lu,\"n\":%lu,\"ring\":[",
        (unsigned long)crumb_last, (unsigned long)prev.fault, (unsigned long)prev.pc,
        (unsigned long)prev.lr, (unsigned long)prev.sp, (unsigned long)prev.xpsr,
        (unsigned long)prev.exc_lr, (unsigned long)prev.r0, (unsigned long)prev.r1,
        (unsigned long)prev.r2, (unsigned long)prev.r3, (unsigned long)prev.r12,
        (unsigned long)prev.cfsr, (unsigned long)prev.hfsr, (unsigned long)prev.mmfar,
        (unsigned long)prev.bfar, (unsigned long)prev.n);
    uint32_t k = prev.n > CRUMB_RING ? prev.n - CRUMB_RING : 0;
    for (uint32_t i = k; i < prev.n && n + 16 < cap; i++) {
        n += (size_t)snprintf(dst + n, cap - n, "%s%lu", i == k ? "" : ",",
                              (unsigned long)prev.ring[i % CRUMB_RING]);
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]}");
}

/* HardFault: record the stacked frame and the fault status, then spin until
 * the watchdog resets us (nothing feeds it from here). */
void crumb_hardfault_c(uint32_t *frame, uint32_t exc_lr) {
    volatile crumb_rec_t *r = CRUMB_REC;
    r->r0 = frame[0]; r->r1 = frame[1]; r->r2 = frame[2]; r->r3 = frame[3];
    r->r12 = frame[4]; r->lr = frame[5]; r->pc = frame[6]; r->xpsr = frame[7];
    r->sp = (uint32_t)frame;
    r->exc_lr = exc_lr;
    r->cfsr = *(volatile uint32_t *)0xE000ED28u;
    r->hfsr = *(volatile uint32_t *)0xE000ED2Cu;
    r->mmfar = *(volatile uint32_t *)0xE000ED34u;
    r->bfar = *(volatile uint32_t *)0xE000ED38u;
    r->fault = 1;
    watchdog_hw->scratch[1] = frame[6] | 1u;
    if (crumb_fault_hook) { crumb_fault_hook(frame); }   /* PEX: which module, persisted */
    for (;;) { __asm volatile("nop"); }
}

__attribute__((naked)) void isr_hardfault(void) {
    __asm volatile(
        "mov r1, lr\n"
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b crumb_hardfault_c\n");
}
