/* Minimal NO_SYS=1 lwIP arch glue.
 *
 * pico-sdk ships this exact code in pico_lwip_nosys, but that module also
 * pulls in pico_lwip_nosys/async_context so sys_check_timeouts() runs off
 * an async worker — machinery this project doesn't need since it already
 * has its own bare while(true) polling loop (see src/main.c), matching
 * the pattern TinyUSB's own net_lwip_webserver example uses (checked
 * directly: lib/tinyusb/examples/device/net_lwip_webserver/src/main.c
 * defines these same four functions itself and calls sys_check_timeouts()
 * straight from its main loop). We link pico_lwip + pico_lwip_arch (for
 * the arch/cc.h include path and PICO_LWIP_CUSTOM_LOCK_TCPIP_CORE) but
 * deliberately not pico_lwip_nosys, and provide these ourselves instead.
 */
#include "lwip/sys.h"
#include "pico/time.h"

sys_prot_t sys_arch_protect(void) {
    return 0;
}

void sys_arch_unprotect(sys_prot_t pval) {
    (void)pval;
}

uint32_t sys_now(void) {
    return to_ms_since_boot(get_absolute_time());
}

uint32_t sys_jiffies(void) {
    return time_us_32();
}
