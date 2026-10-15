#ifndef BTGW_CRUMB_H
#define BTGW_CRUMB_H
/*
 * Breadcrumbs that survive a reset: where the main loop was when it stopped,
 * and -- on a HardFault -- the fault registers and stacked frame.
 *
 * The last crumb and the faulting PC live in WATCHDOG SCRATCH0/1 (the ROM and
 * SDK use 4..7, a flash-update reboot may use 2..3). A fuller record -- a ring
 * of the last crumbs and the fault frame -- lives at a fixed address in
 * SCRATCH_X RAM, which this firmware never uses (core1 is not started) and
 * which no boot in between clears. The next boot reports it (sys.fault, and
 * last_crumb / last_fault_pc in sys.info) and clears it.
 *
 * This is how a trial image that hangs is diagnosed without a debugger: it
 * reverts to the old slot on the watchdog reset, and the next boot of the
 * trial (re-armed) reads what the hung one left behind.
 */
#include <stddef.h>
#include <stdint.h>
#include "hardware/structs/watchdog.h"

#define CRUMB_RING 16
typedef struct {
    uint32_t magic;
    uint32_t n;                      /* crumbs written (ring index) */
    uint32_t ring[CRUMB_RING];       /* code << 16 | detail */
    uint32_t fault;                  /* 1 = HardFault frame below is valid */
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr, sp, exc_lr;
    uint32_t cfsr, hfsr, mmfar, bfar;
} crumb_rec_t;

#define CRUMB_REC   ((volatile crumb_rec_t *)0x20080100u)
#define CRUMB_MAGIC 0xC5B0C5B0u

static inline void crumb(uint32_t where) { watchdog_hw->scratch[0] = where; }

/* A crumb with detail, also kept in the ring. Cheap enough for rare events. */
static inline void crumb_d(uint32_t where, uint32_t detail) {
    watchdog_hw->scratch[0] = where;
    volatile crumb_rec_t *r = CRUMB_REC;
    r->ring[r->n % CRUMB_RING] = (where << 16) | (detail & 0xFFFFu);
    r->n++;
}

/* Main-loop stages and a few deep spots worth telling apart. */
enum {
    CRUMB_LOOP_USB = 0x1001, CRUMB_LOOP_SPI, CRUMB_LOOP_ETH, CRUMB_LOOP_WIFI, CRUMB_LOOP_BT,
    CRUMB_LOOP_LUA, CRUMB_LOOP_UPLINK, CRUMB_LOOP_FWD, CRUMB_LOOP_WG, CRUMB_LOOP_OTA,
    CRUMB_LOOP_CFG, CRUMB_LOOP_WS, CRUMB_LOOP_END,
    CRUMB_MDNS_HOOK = 0x2001, CRUMB_MDNS_HOOK_BUILT, CRUMB_MDNS_HOOK_SENT,
    CRUMB_MDNS_ANNOUNCE = 0x2010, CRUMB_MDNS_GROUP_ADD, CRUMB_MDNS_GROUP_DEL,
    CRUMB_MDNS_SVC_ADD = 0x2020, CRUMB_MDNS_SVC_BOUND, CRUMB_MDNS_SVC_DONE,
    CRUMB_MDNS_SVC_DEL, CRUMB_MDNS_TXT,
};

extern uint32_t crumb_last, crumb_last_fault_pc;
void crumb_boot(void);   /* first thing in main() */
size_t crumb_report_json(char *dst, size_t cap);

#endif
