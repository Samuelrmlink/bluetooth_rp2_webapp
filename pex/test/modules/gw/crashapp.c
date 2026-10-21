/* crashapp: an app that faults on purpose {"delay":ms} after it starts --
 * for the gateway's fault path: attribution, reboot, no autostart next boot. */
#include "pex.h"
__attribute__((noinline)) static void boom(void) { __builtin_trap(); }
static int later(void *arg) { pex_log(PEX_LOG_WARN, "crashing now (on purpose)"); boom(); return PEX_TASK_DONE; }
int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    if (reason == PEX_APP_RUN) {
        int64_t d = 1000;
        pex_json_int(arg, "delay", &d);
        pex_sched_after((uint32_t)d, later, 0);
    }
    return PEX_OK;
}
