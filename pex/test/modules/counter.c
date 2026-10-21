/* counter: per-instance state. Built REENTRANT in model B, so two loads share
 * one copy of the code and each has its own RW (r9). */
#include "pex.h"

static int total;
static int calls = 100;

int counter_add(int n) { total += n; calls++; return total; }
PEX_EXPORT(counter_add);
int counter_calls(void) { return calls; }
PEX_EXPORT(counter_calls);
uintptr_t counter_rw_addr(void) { return (uintptr_t)&total; }
PEX_EXPORT(counter_rw_addr);
uintptr_t counter_code_addr(void) { return (uintptr_t)&counter_add; }
PEX_EXPORT(counter_code_addr);
uintptr_t counter_self(void) { return (uintptr_t)pex_self(); }
PEX_EXPORT(counter_self);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) { return PEX_OK; }
