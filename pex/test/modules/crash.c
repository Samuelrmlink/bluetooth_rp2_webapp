/* crash: faults on purpose, two calls deep, so the host's fault attribution
 * can be checked: module name, build id, offset of the faulting instruction. */
#include "pex.h"

__attribute__((noinline)) static int level2(volatile int *p) {
    __builtin_trap();                 /* UDF: UsageFault, escalated to HardFault */
    return *p;
}
__attribute__((noinline)) static int level1(int x) {
    volatile int v = x;
    return level2(&v) + 1;
}
int crash_now(int x) { return level1(x) + 1; }
PEX_EXPORT(crash_now);

/* The offset the host should report: level2 starts here (the trap is in it). */
uintptr_t crash_where(void) { return (uintptr_t)&level2; }
PEX_EXPORT(crash_where);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) { return PEX_OK; }
