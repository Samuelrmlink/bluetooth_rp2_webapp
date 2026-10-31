/* hello: logs on init and when launch starts it. Exported so pex.call can
 * reach it. No Lua. The same source shape an RP2350 app module uses. */
#include "pex.h"

int hello_answer(void) { return 42; }
PEX_EXPORT(hello_answer);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)arg;
    switch (reason) {
    case PEX_INIT:
        pex_log(PEX_LOG_INFO, "hello from a c5 module on %s", host->host_name);
        return PEX_OK;
    case PEX_APP_RUN:
        pex_log(PEX_LOG_INFO, "hello running");
        return PEX_OK;
    case PEX_APP_STOP:
    case PEX_DEINIT:
        pex_log(PEX_LOG_INFO, "hello stopped");
        return PEX_OK;
    default:
        return PEX_OK;
    }
}
