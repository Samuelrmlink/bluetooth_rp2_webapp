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
#include <stdio.h>

#include "pico/time.h"
#include "hardware/exception.h"
#include "tusb.h"

#include "net/cyw43_wait.h"

void __real_cyw43_await_background_or_timeout_us(uint32_t timeout_us);

/* Two clocks, kept apart on purpose. The first version of this file timed
 * tud_task() and the real wait together, so a 2.75 s reading could have been
 * either -- and they mean opposite things. USB time is our own doing; radio
 * time is the driver waiting on the chip. */
static uint32_t wait_max_us;          /* longest single REAL wait */
static uint32_t wait_max_at_ms;       /* when it happened, ms since boot */
static uint32_t wait_max_asked_us;    /* the timeout that wait was given */
static uint32_t usb_max_us;           /* longest tud_task() from in here */
static uint64_t wait_total_us;
static uint32_t wait_calls;
static bool     in_wrap;

/* A wait that overruns its own deadline by this much is reported at once,
 * because the deadline is 1 ms and the one seen was 2.75 s: that is not a
 * slow radio, it is the wake-up mechanism failing to wake us. */
#define WAIT_REPORT_US 20000u

void __wrap_cyw43_await_background_or_timeout_us(uint32_t timeout_us) {
    if (__get_current_exception() || in_wrap) {
        __real_cyw43_await_background_or_timeout_us(timeout_us);
        return;
    }
    in_wrap = true;

    absolute_time_t t0 = get_absolute_time();
    tud_task();
    absolute_time_t t1 = get_absolute_time();
    __real_cyw43_await_background_or_timeout_us(timeout_us);
    absolute_time_t t2 = get_absolute_time();

    uint32_t usb_us  = (uint32_t)absolute_time_diff_us(t0, t1);
    uint32_t wait_us = (uint32_t)absolute_time_diff_us(t1, t2);
    if (usb_us > usb_max_us) { usb_max_us = usb_us; }
    if (wait_us > wait_max_us) {
        wait_max_us       = wait_us;
        wait_max_at_ms    = to_ms_since_boot(t1);
        wait_max_asked_us = timeout_us;
    }
    if (wait_us >= WAIT_REPORT_US && wait_us > timeout_us * 4u) {
        printf("cyw43: radio wait OVERRAN -- asked %lu us, took %lu us, at %lu ms\n",
               (unsigned long)timeout_us, (unsigned long)wait_us,
               (unsigned long)to_ms_since_boot(t1));
    }
    wait_total_us += wait_us;
    wait_calls++;
    in_wrap = false;
}

uint32_t cyw43_wait_max_us(void)        { return wait_max_us; }
uint32_t cyw43_wait_max_at_ms(void)     { return wait_max_at_ms; }
uint32_t cyw43_wait_max_asked_us(void)  { return wait_max_asked_us; }
uint32_t cyw43_wait_usb_max_us(void)    { return usb_max_us; }
uint64_t cyw43_wait_total_us(void)      { return wait_total_us; }
uint32_t cyw43_wait_calls(void)         { return wait_calls; }

void cyw43_wait_reset_stats(void) {
    wait_max_us = wait_max_at_ms = wait_max_asked_us = usb_max_us = 0;
    wait_total_us = 0;
    wait_calls = 0;
}
