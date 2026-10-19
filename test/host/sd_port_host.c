/* sd_port.h for host tests: time is the simulated card's, and every look at
 * the clock moves it a microsecond, so a polling loop always progresses. A
 * test can hook the yield (the SPI tests run spibus_poll() there, as the main
 * loop would). Card detect reads the simulated card's presence. */
#include "sd/sd_port.h"

#include "sd/sd_card.h"
#include "sdcard_sim.h"

void (*sd_host_yield_hook)(void);
sdsim_t *sd_host_detect_card;
int sd_host_yields;

uint64_t sd_port_now_us(void) { sdsim_advance_us(1); return sdsim_now_us(); }
void sd_port_yield(void) {
    sd_host_yields++;
    sdsim_advance_us(50);
    if (sd_host_yield_hook) { sd_host_yield_hook(); }
}
void sd_port_maybe_yield(uint32_t budget_us) { (void)budget_us; }
void sd_port_delay_us(uint32_t us) { sdsim_advance_us(us); }
void sd_port_detect_init(int pin) { (void)pin; }
bool sd_port_detect_level(int pin) {
    (void)pin;
    return !(sd_host_detect_card && sd_host_detect_card->present);   /* low = present */
}
void sd_port_wait_step(uint64_t t0) {
    if (sd_port_now_us() - t0 > SD_SPIN_US) { sd_port_yield(); }
}
