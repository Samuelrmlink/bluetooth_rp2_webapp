/* See crumb.h. */
#include "util/crumb.h"

uint32_t crumb_last, crumb_last_fault_pc;

void crumb_boot(void) {
    crumb_last = watchdog_hw->scratch[0];
    crumb_last_fault_pc = watchdog_hw->scratch[1];
    watchdog_hw->scratch[0] = 0;
    watchdog_hw->scratch[1] = 0;
}

/* HardFault: record the stacked PC, then spin until the watchdog resets us
 * (which it does: it is armed early in main and nothing feeds it from here). */
void crumb_hardfault_c(uint32_t *frame) {
    watchdog_hw->scratch[1] = frame[6] | 1u;    /* |1: nonzero even for PC 0 */
    for (;;) { __asm volatile("nop"); }
}

__attribute__((naked)) void isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b crumb_hardfault_c\n");
}
