/* The PEX port for any pico-sdk firmware (RP2040, RP2350 Arm). */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
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
