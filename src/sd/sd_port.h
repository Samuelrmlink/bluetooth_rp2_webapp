#ifndef BTGW_SD_PORT_H
#define BTGW_SD_PORT_H
/*
 * What the SD code needs from its surroundings. On the board: the worker's
 * yield, the SDK's clock and the card-detect GPIO (sd_port_pico.c). In host
 * tests: a fake clock that advances when the code yields or spins.
 */
#include <stdbool.h>
#include <stdint.h>

uint64_t sd_port_now_us(void);
/* Give the main loop a turn (from the worker; a no-op outside it). */
void     sd_port_yield(void);
/* Yield if this worker slice has run longer than budget_us. */
void     sd_port_maybe_yield(uint32_t budget_us);
void     sd_port_delay_us(uint32_t us);
/* Card detect: configure once; read the raw level. */
void     sd_port_detect_init(int pin);
bool     sd_port_detect_level(int pin);

/* In a wait loop that started at t0: spin for SD_SPIN_US, then yield on
 * every poll (most waits end in the spin, and a trip through the main loop
 * costs more than that). */
void     sd_port_wait_step(uint64_t t0);

#endif
