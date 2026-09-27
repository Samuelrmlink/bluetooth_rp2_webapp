/*
 * Keep USB alive while the cyw43 driver waits for the radio, and measure how
 * long it waits.
 *
 * THE PROBLEM. A board was found unreachable on BOTH USB-NCM and WiFi,
 * halted with its PC inside sem_acquire_block_until; only a debugger reset
 * recovered it. That semaphore has exactly one reachable caller in the whole
 * image -- async_context_poll_wait_for_work_until -- and in turn exactly one
 * function reaches THAT: cyw43_await_background_or_timeout_us(), the SDK's
 * pico_cyw43_driver/cyw43_driver.c:266. The cyw43 driver calls it from the
 * wait loop inside cyw43_do_ioctl():
 *
 *     #define CYW43_DO_IOCTL_WAIT cyw43_await_background_or_timeout_us(1000)
 *
 * spinning there until CYW43_IOCTL_TIMEOUT_US. So the board was blocked
 * waiting for an ioctl response from the radio that never arrived.
 *
 * WHY THAT TAKES THE WHOLE DEVICE DOWN. main()'s loop is the only thing that
 * pumps tud_task(), and TinyUSB is not driven from an interrupt here by
 * design. Block anywhere inside that loop and the USB host stops being
 * answered; after a few hundred milliseconds it drops the device. The WiFi
 * side is already gone, because the thing that is stuck IS the radio. So one
 * unresponsive ioctl removes both ways of reaching the board at once, which
 * is precisely the failure that was seen.
 *
 * WHAT THIS DOES. It wraps that one function (via -Wl,--wrap) and services
 * TinyUSB before delegating to the real wait. tud_task() is safe to call
 * here, and that is not an accident of this file: usb_ncm.c's receive
 * callback only memcpys into a ring buffer, and lwIP is touched exclusively
 * in usb_ncm_poll(), which this does NOT call. So nothing here can re-enter
 * lwIP, and therefore nothing here can re-enter the cyw43 driver that is
 * mid-ioctl above us. Pumping usb_ncm_poll() as well would route a packet
 * out of the WiFi netif and back into that driver, so it is deliberately
 * left out.
 *
 * This does not stop the radio hanging. It stops a hung radio from also
 * taking USB with it, which is the difference between a board you can still
 * talk to and diagnose and a board that needs a debugger.
 *
 * It also records how long the driver spends waiting, because the trigger
 * was never reproduced: normal joins, a join to an SSID that does not exist,
 * and repeated reboots all fail to provoke it. The next occurrence should
 * leave evidence rather than another one-off halt, so sys.info reports the
 * longest single wait and the cumulative time.
 */
#include <stdbool.h>
#include <stdint.h>

#include "pico/time.h"
#include "hardware/exception.h"
#include "tusb.h"

#include "net/cyw43_wait.h"

void __real_cyw43_await_background_or_timeout_us(uint32_t timeout_us);

static uint32_t wait_max_us;
static uint64_t wait_total_us;
static uint32_t wait_calls;
static bool     in_wrap;

void __wrap_cyw43_await_background_or_timeout_us(uint32_t timeout_us) {
    /* In an exception the real function takes a different path and tud_task()
     * must not run at all. Re-entry would mean the driver called itself
     * through us, which it should not, but guard rather than assume. */
    if (__get_current_exception() || in_wrap) {
        __real_cyw43_await_background_or_timeout_us(timeout_us);
        return;
    }

    in_wrap = true;
    absolute_time_t start = get_absolute_time();

    /* Answer the host BEFORE blocking, so a long wait starts with USB
     * already serviced rather than already overdue. */
    tud_task();
    __real_cyw43_await_background_or_timeout_us(timeout_us);

    uint32_t us = (uint32_t)absolute_time_diff_us(start, get_absolute_time());
    if (us > wait_max_us) { wait_max_us = us; }
    wait_total_us += us;
    wait_calls++;
    in_wrap = false;
}

uint32_t cyw43_wait_max_us(void)    { return wait_max_us; }
uint64_t cyw43_wait_total_us(void)  { return wait_total_us; }
uint32_t cyw43_wait_calls(void)     { return wait_calls; }

void cyw43_wait_reset_stats(void) {
    wait_max_us = 0;
    wait_total_us = 0;
    wait_calls = 0;
}
