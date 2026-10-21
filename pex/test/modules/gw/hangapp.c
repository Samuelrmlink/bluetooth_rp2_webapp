/* hangapp: an app that never returns from a callback {"delay":ms} after it
 * starts -- the watchdog resets the board and the next boot names the module. */
#include "pex.h"
static int later(void *arg) {
    pex_log(PEX_LOG_WARN, "hanging now (on purpose)");
    for (volatile int i = 0;; i++) {}
    return PEX_TASK_DONE;
}
int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    if (reason == PEX_APP_RUN) {
        int64_t d = 1000;
        pex_json_int(arg, "delay", &d);
        pex_sched_after((uint32_t)d, later, 0);
    }
    return PEX_OK;
}
