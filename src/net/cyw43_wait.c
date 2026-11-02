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
 * ROOT CAUSE (cyw43-driver deep-dive, task #37). In poll mode the SDK's real
 * wait bottoms out in a bare __wfe() (best_effort_wfe_or_timeout in pico_time)
 * woken only by (a) the chip's WL_HOST_WAKE GPIO IRQ or (b) the alarm-timer
 * IRQ issuing __sev() at the deadline. When BOTH wakes are missed, the "1 ms"
 * wait blocks far past its deadline -- ~258 ms if a late GPIO IRQ eventually
 * rescues it (a survivable boot), or FOREVER (the wedge). And cyw43_do_ioctl's
 * outer 500 ms timeout is only re-checked BETWEEN waits, so one never-returning
 * wait hangs the whole cooperative loop in sem_acquire_block_until -- taking
 * WiFi, WireGuard and USB-NCM down together until a power-cycle.
 *
 * WHAT THIS DOES. It wraps that function (via -Wl,--wrap) and does NOT enter
 * the WFE wait at all. It bounds the ~1 ms back-off on the FREE-RUNNING timer
 * counter (time_reached), which provably keeps advancing during the failure,
 * and pumps tud_task() throughout. So every wait returns within timeout_us no
 * matter whether the WFE wake would have arrived; cyw43_do_ioctl re-polls the
 * chip over SPI each iteration as it always did; and on a genuinely stuck chip
 * its 500 ms timeout now fires CLEANLY (-CYW43_ETIMEDOUT, which wifi.c handles
 * by retrying). The wedge is structurally impossible: no single wait can block
 * past its deadline, so the loop can never hang.
 *
 * tud_task() is safe to call here -- usb_ncm.c's receive callback only memcpys
 * into a ring buffer, and lwIP is touched exclusively in usb_ncm_poll(), which
 * this does NOT call. So nothing here can re-enter lwIP, and therefore nothing
 * here can re-enter the cyw43 driver that is mid-ioctl above us. Pumping
 * usb_ncm_poll() as well would route a packet out of the WiFi netif and back
 * into that driver, so it is deliberately left out. The GPIO-IRQ early-wake we
 * give up was only a power/latency optimisation; the trade is a busy-spin (up
 * to timeout_us, or up to the 500 ms ioctl bound on a failed call) with USB
 * kept alive -- the right call for a radio that would otherwise hang the whole
 * device.
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
#include "net/mdns.h"

void __real_cyw43_await_background_or_timeout_us(uint32_t timeout_us);
void __real_cyw43_cb_process_ethernet(void *cb_data, int itf, size_t len, const uint8_t *buf);

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

/* With the counter-bounded spin a wait returns within ~timeout_us, so any
 * overrun this large now means tud_task() itself stalled -- still worth a log. */
#define WAIT_REPORT_US 20000u

void __wrap_cyw43_await_background_or_timeout_us(uint32_t timeout_us) {
    if (__get_current_exception() || in_wrap) {
        __real_cyw43_await_background_or_timeout_us(timeout_us);
        return;
    }
    in_wrap = true;

    /* Counter-bounded back-off, NOT the WFE wait -- see the header. Every call
     * returns within timeout_us because time_reached() reads the free-running
     * timer, which keeps advancing regardless of any lost IRQ/SEV wake. USB is
     * pumped throughout so it never dies with the radio. */
    absolute_time_t t0 = get_absolute_time();
    tud_task();
    absolute_time_t t1 = get_absolute_time();
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (!time_reached(deadline)) {
        tud_task();
    }
    absolute_time_t t2 = get_absolute_time();

    uint32_t usb_us  = (uint32_t)absolute_time_diff_us(t0, t1);  /* first tud_task */
    uint32_t wait_us = (uint32_t)absolute_time_diff_us(t0, t2);  /* whole bounded wait */
    if (usb_us > usb_max_us) { usb_max_us = usb_us; }
    if (wait_us > wait_max_us) {
        wait_max_us       = wait_us;
        wait_max_at_ms    = to_ms_since_boot(t0);
        wait_max_asked_us = timeout_us;
    }
    if (wait_us >= WAIT_REPORT_US && wait_us > timeout_us * 4u) {
        printf("cyw43: wait OVERRAN (tud_task stall?) -- asked %lu us, took %lu us, at %lu ms\n",
               (unsigned long)timeout_us, (unsigned long)wait_us,
               (unsigned long)to_ms_since_boot(t0));
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

/* The bytes are already in `buf`. Drop mDNS before the driver allocates a
 * PBUF_POOL buffer. Called from cyw43_ll.c, so --wrap sees the reference. */
void __wrap_cyw43_cb_process_ethernet(void *cb_data, int itf, size_t len, const uint8_t *buf) {
    if (buf && mdns_should_drop(buf, len)) return;
    __real_cyw43_cb_process_ethernet(cb_data, itf, len, buf);
}
