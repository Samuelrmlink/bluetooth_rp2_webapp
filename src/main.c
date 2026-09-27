/*
 * One cooperative loop on core0, no RTOS.
 *
 * The important structural fact for this firmware: on a Pico W / Pico 2 W a
 * single cyw43_arch_init() brings up the cyw43 driver, lwIP (NO_SYS=1) AND
 * BTstack on one shared async_context_poll -- see the SDK's
 * pico_cyw43_arch/cyw43_arch_poll.c. So cyw43_arch_poll() from here drives
 * Wi-Fi, TCP and Bluetooth together, sys_arch_protect() stays a no-op, and
 * there is no lwIP locking anywhere. Everything downstream assumes that: any
 * handler that blocks stalls the radio and the network at once.
 *
 * The console is RTT over SWD (pico_stdio_rtt). No UART is wired from the
 * debug probe to the target, and CDC-ACM is off, so this is the only channel
 * during early boot -- deliberately, since RTT is non-halting and semihosting
 * would stall this loop on every printf.
 */
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "autoconf.h"
#include "app_config.h"
#include "fs/fs.h"
#include "config/config.h"
#include "net/http.h"
#include "net/usb_ncm.h"
#include "tusb.h"
#include "net/mdns.h"
#include "api/ops.h"
#if CONFIG_LUA_ENABLE
#include "lua/lrt.h"
#endif
#if CONFIG_NET_WIFI
#include "net/wifi.h"
#endif
#if CONFIG_BT_LE
#include "bt/bt.h"
#include "bt/le/central.h"
#endif

int main(void) {
    stdio_init_all();
    sleep_ms(200);                       /* let RTT attach before the first line */
    printf("\n--- bluetooth_rp2_webapp ---\n");

    /* Nothing here is fatal. A board with a corrupt filesystem should still
     * come up far enough to be talked to and fixed, so every failure is a log
     * line and the loop still runs. */
    if (!fs_init()) {
        printf("fs: unavailable (the API still serves, the filesystem does not)\n");
    } else {
        cfg_load();
    }

    /* Brings up cyw43 + lwIP + BTstack on one async_context_poll. */
    if (cyw43_arch_init()) {
        printf("cyw43: init failed -- no Wi-Fi and no Bluetooth this boot\n");
    } else {
        printf("cyw43: up\n");
#if CONFIG_BT_LE
        bt_init();
#endif
#if CONFIG_NET_WIFI
        if (!wifi_init()) {
            printf("wifi: unavailable, continuing over USB-NCM only\n");
        }
#endif
    }

#if CONFIG_NET_USB_NCM
    /* Explicit, because pico_stdio_usb is off: nothing else brings TinyUSB up,
     * and without this the host sees the pull-up but every control transfer
     * times out ("device not accepting address, error -71"). tud_task() is
     * then pumped from the loop below -- never from an IRQ. */
    tud_init(0);
    usb_ncm_init();
    printf("ncm: device %d.%d.%d.%d\n", USB_IP0, USB_IP1, USB_IP2, USB_IP3);
#endif
#if CONFIG_NET_MDNS
    mdns_start();
#endif

    http_server_start();
    ops_init_all();
    printf("http: listening on :80  (ws at /ws)\n");

    while (true) {
#if CONFIG_NET_USB_NCM
        tud_task();
        usb_ncm_poll();
        usb_ncm_recover_tick();
#endif
        /* Drives cyw43, lwIP timers and the BTstack run loop in one call. */
#if CONFIG_NET_WIFI
        /* wifi_poll() makes that same call and THEN advances the station/AP
         * state machine. Calling only cyw43_arch_poll() here left the machine
         * frozen: the radio came up, reported mode "idle" and "0 known
         * network(s)", and then never scanned, never joined and never started
         * the fallback AP -- for the whole life of the firmware. Nothing
         * complained, because every individual piece was working. */
        wifi_poll();
#else
        cyw43_arch_poll();
#endif

#if CONFIG_BT_LE
        le_central_tick();
#endif
#if CONFIG_LUA_ENABLE
        lrt_tick();
#endif
        cfg_tick();
        /* Coalesce whatever the pass produced into as few segments as possible;
         * the per-line tcp_output() was removed deliberately. */
        http_flush_ws();
    }
}
