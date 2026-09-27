#ifndef BTGW_CYW43_WAIT_H
#define BTGW_CYW43_WAIT_H

#include <stdint.h>

/* How long the cyw43 driver has spent blocked waiting for the radio.
 *
 * Reported because the one time the board wedged, it was inside exactly that
 * wait, and the trigger has not been reproducible since. A single wait longer
 * than a few milliseconds means the radio stopped answering an ioctl, which
 * is the shape of the failure that took USB and WiFi down together. See
 * cyw43_wait.c. */
uint32_t cyw43_wait_max_us(void);
uint64_t cyw43_wait_total_us(void);
uint32_t cyw43_wait_calls(void);
void     cyw43_wait_reset_stats(void);

#endif
