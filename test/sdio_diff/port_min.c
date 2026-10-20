/* sd_port.h without the storage worker: the differential firmware does
 * nothing but the script, so a wait just waits. */
#include "sd/sd_port.h"

#include "hardware/gpio.h"
#include "pico/time.h"
#include "sd/sd_card.h"

uint64_t sd_port_now_us(void) { return time_us_64(); }
void sd_port_yield(void) {}
void sd_port_maybe_yield(uint32_t budget_us) { (void)budget_us; }
bool sd_port_slice_spent(void) { return false; }   /* no main loop to share with */
void sd_port_delay_us(uint32_t us) { busy_wait_us_32(us); }
void sd_port_detect_init(int pin) { gpio_init((uint)pin); gpio_set_dir((uint)pin, GPIO_IN); gpio_pull_up((uint)pin); }
bool sd_port_detect_level(int pin) { return gpio_get((uint)pin); }
void sd_port_wait_step(uint64_t t0) { (void)t0; }
