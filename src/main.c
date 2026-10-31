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
#include "hardware/watchdog.h"
#include "api/api.h"
#include "ota/ota_slot.h"     /* ota_reboot, for sys.reboot */

#include "autoconf.h"
#include "app_config.h"
#include "fs/fs.h"
#include "fs/fs_migrate.h"
#include "fs/fs_layout.h"
#if CONFIG_FS_LAYOUT
#include "fs/fs_internal.h"   /* fs_is_mounted, after the mover retargets */
#endif
#include "fs/fs_watch.h"
#include "config/config.h"
#include "net/http.h"
#include "net/usb_ncm.h"
#include "tusb.h"
#include "net/mdns.h"
#include "api/ops.h"
#include "util/crumb.h"
#include "util/stackmark.h"
#include "launch.h"
#include "autoconf.h"
#if CONFIG_PEX
#include "pex/pex_gw.h"
#endif
#if CONFIG_SPI_BUS
#include "io/spibus.h"
#endif
#include "net/eth_w5500.h"
#if CONFIG_SD
#include "sd/sd_core.h"
#endif
#if CONFIG_LUA_ENABLE
#include "lua/lrt.h"
#endif
#include "net/wg.h"
#include "net/uplink.h"
#include "net/fwd.h"
#include "net/sntp_time.h"
#if CONFIG_NET_WIFI
#include "net/wifi.h"
#if CONFIG_OTA
#include "ota/smp.h"
#endif
#endif
#if CONFIG_BT_LE
#include "bt/bt.h"
#include "bt/le/central.h"
#if CONFIG_BT_CLASSIC_NAP
#include "bt/classic/nap.h"
#endif
#endif

/* Set while the previous boot ended in a watchdog reset, so the fact survives
 * into sys.info. A wedge on a device reached over WireGuard is otherwise
 * invisible: it reboots, comes back, and nobody ever learns it happened.
 *
 * Read it with one caveat: a debugger-driven reset can set the same flag, so
 * `true` immediately after a flash or an openocd `reset run` means nothing.
 * It is evidence only when the board rebooted on its own. */
bool boot_after_watchdog;

int main(void) {
    crumb_boot();
    stackmark_paint();
    stdio_init_all();
    /* busy_wait_ms(): this runs BEFORE watchdog_enable() below, so a hang here
     * has nothing to recover it, and sleep_ms()'s __wfe() depends on an alarm
     * IRQ this early in boot. Spin on the timer instead. */
    busy_wait_ms(200);                   /* let RTT attach before the first line */
    printf("\n--- bluetooth_rp2_webapp ---\n");

    boot_after_watchdog = watchdog_caused_reboot();
    launch_boot();
    if (crumb_last || crumb_last_fault_pc) {
        printf("main: previous boot stopped at crumb 0x%04lx, fault pc 0x%08lx\n",
               (unsigned long)crumb_last, (unsigned long)crumb_last_fault_pc);
    }

    /* Armed BEFORE the radio and the filesystem, not after init. Init is ~2 s
     * and nothing in it should wait: a cyw43_arch_init() that never returns
     * (seen on a try-before-you-buy boot fed the wrong firmware partition)
     * used to sit forever with the debugger attached, since the RP2350
     * watchdog pauses while a core is halted only on faults -- and without a
     * debugger only the ROM's own ~16.7 s TBYB timer saved the board. With
     * this, a hung init reboots normally, and a TBYB image that has not been
     * bought is not bootable on a normal boot, so the old slot comes back.
     * The reset-on-watchdog flag is read above, before this re-arms it. */
#define WATCHDOG_TIMEOUT_MS 8000
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
    if (boot_after_watchdog) {
        printf("main: REBOOTED BY WATCHDOG -- the previous boot's loop stopped running\n");
    }

    /* Nothing here is fatal. A board with a corrupt filesystem should still
     * come up far enough to be talked to and fixed, so every failure is a log
     * line and the loop still runs. */
    bool fs_up = fs_init();
#if CONFIG_FS_LAYOUT
    /* Before cfg_load and before the radio. A normal boot with no arm file
     * returns immediately. The move, when armed, restores cfg/ and only then
     * falls through to cfg_load. */
    fs_layout_boot();
    fs_up = fs_is_mounted();
#endif
    if (!fs_up) {
        printf("fs: unavailable (the API still serves, the filesystem does not)\n");
    } else {
        /* BEFORE any config is read: a filesystem in the pre-PEX layout is
         * moved here (through the SD card), so Wi-Fi, Ethernet and WireGuard
         * come up from the restored cfg/. Does nothing on a migrated one.
         * The layout mover has its own volume; the old migrator must not
         * format it. */
#if CONFIG_FS_LAYOUT
        if (!fs_layout_suppress_migrate())
#endif
        fs_migrate_boot();
        cfg_load();
    }

    /* Brings up cyw43 + lwIP + BTstack on one async_context_poll. */
    if (cyw43_arch_init()) {
        printf("cyw43: init failed -- no Wi-Fi and no Bluetooth this boot\n");
    } else {
        /* Before bt_init / wifi: the radio just freed its 4 KB work area,
         * and the buy buffer has to come from that heap. See ota_slot.c. */
        ota_reserve_buy_buf();
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

#if CONFIG_NET_MDNS
    /* BEFORE any interface is brought up.
     *
     * usb_ncm_init() and wifi register their netifs with mdns_add_netif() as
     * they come up, and mdns_start() calls mdns_resp_init() and clears the
     * table of bound interfaces. Running it afterwards therefore threw away
     * the registration that had just been made -- the responder answered
     * nothing at all, on an interface it believed it had never been given.
     * lwIP is up as soon as cyw43_arch_init() has run, which is above. */
    mdns_start();
#endif

#if CONFIG_NET_USB_NCM
    /* Explicit, because pico_stdio_usb is off: nothing else brings TinyUSB up,
     * and without this the host sees the pull-up but every control transfer
     * times out ("device not accepting address, error -71"). tud_task() is
     * then pumped from the loop below -- never from an IRQ. */
    tud_init(0);
    usb_ncm_init();
    printf("ncm: device %d.%d.%d.%d\n", USB_IP0, USB_IP1, USB_IP2, USB_IP3);
#endif

#if CONFIG_NET_ETH_W5500
    /* After cyw43_arch_init() -- the radio has claimed its DMA channels by
     * now, and spibus takes two of what is left -- and after mdns_start(),
     * which would otherwise forget the netif registration. No chip fitted
     * is a log line and nothing else. */
    if (!eth_init()) {
        printf("eth: no Ethernet this boot\n");
    }
#endif

#if CONFIG_NET_WIREGUARD
    /* After the interfaces, because the tunnel rides on one of them -- and
     * it is additive: if this fails, everything above still works. */
    wg_init();
#endif
#if CONFIG_NET_UPLINK
    /* After the tunnel: the chosen uplink is handed to it. */
    uplink_init();
#endif
#if CONFIG_NET_FORWARD
    fwd_init();
#endif
#if CONFIG_NET_SNTP
    /* Wall-clock time for sys.time() and TLS dates; it waits for a route. */
    sntp_time_init();
#endif

#if CONFIG_SD
    /* The storage worker: it identifies and mounts the card itself, from the
     * loop (sd_poll), so a slow or absent card never delays boot. After
     * eth_init(): on SPI the card may share the W5500's bus. */
    sd_init();
#endif
    http_server_start();
    ops_init_all();
#if CONFIG_OTA
    /* After the network: the SMP socket serves every interface, and the
     * try-before-you-buy gate inside ota_tick() counts loop passes from here. */
    ota_init();
#endif
    printf("http: listening on :80  (ws at /ws)\n");

    /* A pass longer than this means USB went unserviced long enough to
     * matter. TinyUSB tolerates a few tens of ms; the host starts giving up
     * in the hundreds. */
#define LOOP_STALL_MS 250
    uint32_t last_pass_ms = 0;

    /*
     * The loop below is the only thing pumping USB, WiFi, BLE and Lua, and a
     * call inside it CAN block indefinitely: a board was found parked in
     * sem_acquire_block_until, unreachable on both USB-NCM and WiFi, and only
     * a debugger-driven reset brought it back. The stall detector at the
     * bottom of the pass cannot report that, because it only runs when the
     * pass completes -- the one case it most needs to catch is the one where
     * it never runs.
     *
     * So the loop is watchdogged. This does not fix whatever blocks; it
     * bounds the damage to one reboot, which matters because the device is
     * reachable over WireGuard from somewhere nobody can power-cycle it.
     *
     * 8 s: a flash erase inside flash_safe_execute() is allowed 2 s, and a
     * legitimate pass must never trip this. pause_on_debug stops a target
     * halted under openocd from resetting itself mid-inspection -- without it
     * every attempt to examine a wedge destroys the evidence.
     */
    printf("main: init done, watchdog at %d ms since before the radio\n", WATCHDOG_TIMEOUT_MS);

    while (true) {
        watchdog_update();
        if (api_reboot_due()) {
            printf("main: reboot requested over the API\n");
#if CONFIG_OTA
            ota_reboot(false);          /* powers the radio down first */
#else
            watchdog_reboot(0, 0, 100);
            for (;;) { __asm volatile("wfi"); }
#endif
        }
#if CONFIG_NET_USB_NCM
        crumb(CRUMB_LOOP_USB);
        tud_task();
        usb_ncm_poll();
        usb_ncm_recover_tick();
#endif
#if CONFIG_SPI_BUS
        /* SPI completions: callbacks run here, in the loop, never in the
         * DMA IRQ -- and never from cyw43_wait.c, which pumps USB only. */
        crumb(CRUMB_LOOP_SPI);
        spibus_poll();
#endif
#if CONFIG_NET_ETH_W5500
        crumb(CRUMB_LOOP_ETH);
        eth_poll();
#endif
#if CONFIG_SD
        /* The storage worker's turn (it yields within ~2 ms), then whatever
         * it produced or took for the HTTP side. */
        crumb(CRUMB_LOOP_SD);
        sd_poll();
        http_sd_pump();
#endif
        /* After the worker's turn, so a note it just queued is in this pass.
         * LittleFS notes are queued on the write itself and leave here too. */
        fs_watch_poll();
        /* Drives cyw43, lwIP timers and the BTstack run loop in one call. */
#if CONFIG_NET_WIFI
        /* wifi_poll() makes that same call and THEN advances the station/AP
         * state machine. Calling only cyw43_arch_poll() here left the machine
         * frozen: the radio came up, reported mode "idle" and "0 known
         * network(s)", and then never scanned, never joined and never started
         * the fallback AP -- for the whole life of the firmware. Nothing
         * complained, because every individual piece was working. */
        crumb(CRUMB_LOOP_WIFI);
        wifi_poll();
#else
        cyw43_arch_poll();
#endif

        /* Before Bluetooth, so a WireGuard launch gate is already holding
         * the radio when the controller would otherwise power on. */
        launch_poll();
#if CONFIG_BT_LE
        crumb(CRUMB_LOOP_BT);
        bt_power_on_tick();   /* deferred controller power-on; staggers BT behind WiFi at boot */
        le_central_tick();
#if CONFIG_BT_CLASSIC_NAP
        /* After the radio poll, so a BNEP event can mark cfg/nap.json dirty
         * without writing flash from inside cyw43_arch_poll. */
        nap_tick();
#endif
#endif
#if CONFIG_LUA_ENABLE
        crumb(CRUMB_LOOP_LUA);
        lrt_tick();
#endif
#if CONFIG_PEX
        crumb(CRUMB_LOOP_PEX);
        pex_gw_poll();
#endif
#if CONFIG_NET_UPLINK
        crumb(CRUMB_LOOP_UPLINK);
        uplink_tick();
#endif
#if CONFIG_NET_FORWARD
        crumb(CRUMB_LOOP_FWD);
        fwd_tick();
#endif
#if CONFIG_NET_WIREGUARD
        crumb(CRUMB_LOOP_WG);
        wg_tick();
#endif
#if CONFIG_OTA
        crumb(CRUMB_LOOP_OTA);
        ota_tick();
#endif
        crumb(CRUMB_LOOP_CFG);
        cfg_tick();
        fs_migrate_tick();
        /* Coalesce whatever the pass produced into as few segments as possible;
         * the per-line tcp_output() was removed deliberately. */
        crumb(CRUMB_LOOP_WS);
        http_flush_ws();
        crumb(CRUMB_LOOP_END);

        /*
         * How long did that pass take?
         *
         * This loop is the only thing pumping tud_task(), so a pass that takes
         * long enough stops servicing USB and the host drops the device --
         * which presents as "the board fell off the network" with nothing
         * anywhere saying why. The cyw43 driver can do exactly that: a WiFi
         * join blocks inside it, and poll mode's wait is a bare sleep_until()
         * that never services the async context.
         *
         * Reported rather than fixed here, because knowing a stall happened
         * and how long it was is the thing that was missing.
         */
        uint32_t pass_end = to_ms_since_boot(get_absolute_time());
        if (last_pass_ms && (uint32_t)(pass_end - last_pass_ms) > LOOP_STALL_MS) {
            printf("main: loop stalled %lu ms -- USB was not serviced\n",
                   (unsigned long)(pass_end - last_pass_ms));
        }
        last_pass_ms = pass_end;
    }
}
