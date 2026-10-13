#ifndef BTGW_CRUMB_H
#define BTGW_CRUMB_H
/*
 * Breadcrumbs that survive a watchdog reset: where the main loop was when it
 * stopped, and the PC of a HardFault. Kept in WATCHDOG SCRATCH0/1, which the
 * boot ROM and the SDK leave alone (they use 4..7, and a flash-update reboot
 * may use 2..3). main() reports the previous boot's values in sys.info as
 * "last_crumb" / "last_fault_pc" and clears them.
 *
 * This is how a trial image that hangs is diagnosed without a debugger: it
 * reverts to the old slot on the watchdog reset, and the next boot of the
 * trial (re-armed) reads what the hung one left here.
 */
#include <stdint.h>
#include "hardware/structs/watchdog.h"

static inline void crumb(uint32_t where) { watchdog_hw->scratch[0] = where; }

/* Main-loop stages and a few deep spots worth telling apart. */
enum {
    CRUMB_LOOP_USB = 0x1001, CRUMB_LOOP_SPI, CRUMB_LOOP_ETH, CRUMB_LOOP_WIFI, CRUMB_LOOP_BT,
    CRUMB_LOOP_LUA, CRUMB_LOOP_UPLINK, CRUMB_LOOP_FWD, CRUMB_LOOP_WG, CRUMB_LOOP_OTA,
    CRUMB_LOOP_CFG, CRUMB_LOOP_WS, CRUMB_LOOP_END,
    CRUMB_MDNS_HOOK = 0x2001, CRUMB_MDNS_HOOK_BUILT, CRUMB_MDNS_HOOK_SENT,
    CRUMB_MDNS_ANNOUNCE = 0x2010, CRUMB_MDNS_GROUP_ADD, CRUMB_MDNS_GROUP_DEL,
    CRUMB_MDNS_SVC_ADD = 0x2020, CRUMB_MDNS_SVC_BOUND, CRUMB_MDNS_SVC_DONE,
};

extern uint32_t crumb_last, crumb_last_fault_pc;
void crumb_boot(void);   /* first thing in main() */

#endif
