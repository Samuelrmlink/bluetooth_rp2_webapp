/* hangmask: masks interrupts, then spins. The slice timer cannot preempt
 * that, so the watchdog resets the board and the next boot names the module
 * from the trace word. hangapp (interrupts left on) is the slice path. */
#include "pex.h"
static int later(void *arg) {
    (void)arg;
    pex_log(PEX_LOG_WARN, "masking irqs and hanging (on purpose)");
    __asm volatile("cpsid i" ::: "memory");
    for (volatile int i = 0;; i++) {}
    return PEX_TASK_DONE;
}
int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host;
    if (reason == PEX_APP_RUN) {
        int64_t d = 1000;
        pex_json_int(arg, "delay", &d);
        pex_sched_after((uint32_t)d, later, 0);
    }
    return PEX_OK;
}
