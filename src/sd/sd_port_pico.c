/* sd_port.h on the board: the SDK clock, the storage worker's yield, and the
 * card-detect GPIO (reserved in the pin broker like the bus pins). */
#include "sd/sd_port.h"

#include "autoconf.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "sd/sd_card.h"
#include "sd/sd_worker.h"
#if CONFIG_IO_PINS
#include "io/pins.h"
#endif

uint64_t sd_port_now_us(void) { return time_us_64(); }
void sd_port_yield(void) { sdw_yield(); }
void sd_port_maybe_yield(uint32_t budget_us) { sdw_maybe_yield(budget_us); }
void sd_port_delay_us(uint32_t us) { busy_wait_us_32(us); }

void sd_port_detect_init(int pin) {
#if CONFIG_IO_PINS
    pins_reserve((uint8_t)pin, PIN_FN_GPIO, "sd");
#endif
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, GPIO_IN);
    gpio_pull_up((uint)pin);
}

bool sd_port_detect_level(int pin) { return gpio_get((uint)pin); }

void sd_port_wait_step(uint64_t t0) {
    if (time_us_64() - t0 > SD_SPIN_US) { sdw_yield(); }
}
