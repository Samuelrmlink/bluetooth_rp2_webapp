/* The PEX port for any pico-sdk firmware (RP2040, RP2350 Arm). */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "port/pex_port.h"

uint32_t pex_port_ms(void) { return to_ms_since_boot(get_absolute_time()); }
uint64_t pex_port_us(void) { return time_us_64(); }

/* Weak: a host with its own log sink (the gateway's, a file) overrides it. */
__attribute__((weak)) void pex_port_log(int level, const char *line) {
    (void)level;
    printf("%s\n", line);
}

void pex_port_code_sync(void *p, uint32_t n) {
    (void)p; (void)n;
    __dsb();
    __isb();
}

extern char __StackTop, __StackLimit, __StackBottom;
__attribute__((weak)) int pex_port_stack_bounds(uintptr_t *lo, uintptr_t *hi) {
    *lo = (uintptr_t)&__StackLimit;
    *hi = (uintptr_t)&__StackTop;
    return 1;
}

/* One hardware alarm, claimed on first use (the default pool owns alarm 3).
 * The handler is naked so it still sees the thread's exception frame: the
 * SDK alarm callback runs too late, after the C prologue has moved the stack.
 * Lowest priority, so it never preempts USB or the radio; it only runs when
 * the stuck code is back in thread mode. */
static int s_alarm = -1;
static uint s_irq;

/* Not static: the naked handler branches to it by name. */
void pex_slice_isr_c(uint32_t *frame, uint32_t exc_lr) {
    timer_hw_t *timer = PICO_DEFAULT_TIMER_INSTANCE();
    if (s_alarm >= 0) {
        timer->intr = 1u << s_alarm;
        hw_clear_bits(&timer->intf, 1u << s_alarm);
    }
    if (!pex_slice_active()) { return; }
    if (pex_slice_preempt(frame, exc_lr)) { return; }
    /* This IRQ interrupted another handler. The module is still underneath. */
    if (s_alarm >= 0) {
        timer->alarm[s_alarm] = (uint32_t)(time_us_64() + 1000);
    }
}

__attribute__((naked)) void pex_slice_isr(void) {
    __asm volatile(
        "movs r0, #4\n"
        "mov r1, lr\n"
        "tst r0, r1\n"
        "beq 1f\n"
        "mrs r0, psp\n"
        "b pex_slice_isr_c\n"
        "1:\n"
        "mrs r0, msp\n"
        "b pex_slice_isr_c\n");
}

static bool slice_hw_init(void) {
    if (s_alarm >= 0) { return true; }
    if (s_alarm < -1) { return false; }
    int a = hardware_alarm_claim_unused(false);
    if (a < 0) {
        s_alarm = -2;
        pex_port_log(0, "pex: no free hardware alarm; a stuck module resets the board");
        return false;
    }
    timer_hw_t *timer = PICO_DEFAULT_TIMER_INSTANCE();
    s_alarm = a;
    s_irq = timer_hardware_alarm_get_irq_num(timer, (uint)a);
    irq_set_exclusive_handler(s_irq, pex_slice_isr);
    irq_set_priority(s_irq, PICO_LOWEST_IRQ_PRIORITY);
    hw_set_bits(&timer->inte, 1u << a);
    irq_set_enabled(s_irq, true);
    return true;
}

void pex_port_slice_arm(uint32_t ms) {
    if (!ms || !slice_hw_init()) { return; }
    if (hardware_alarm_set_target((uint)s_alarm, make_timeout_time_ms(ms))) {
        hardware_alarm_force_irq((uint)s_alarm);
    }
}

void pex_port_slice_disarm(void) {
    uint32_t save = save_and_disable_interrupts();
    if (s_alarm >= 0) {
        timer_hw_t *timer = PICO_DEFAULT_TIMER_INSTANCE();
        hardware_alarm_cancel((uint)s_alarm);
        timer->intr = 1u << s_alarm;
        hw_clear_bits(&timer->intf, 1u << s_alarm);
        irq_clear(s_irq);
    }
    pex_slice_note_disarmed();
    restore_interrupts(save);
}
