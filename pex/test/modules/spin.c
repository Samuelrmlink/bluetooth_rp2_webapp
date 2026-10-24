/* spin: one export that never returns. The host's slice must abort the call,
 * fault this module, and keep running whatever else is scheduled. */
#include "pex.h"

int spin_forever(void) {
    for (volatile int i = 0;; i++) {}
    return 0;
}
PEX_EXPORT(spin_forever);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host; (void)reason; (void)arg;
    return PEX_OK;
}
