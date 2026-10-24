#ifndef BTGW_LAUNCH_H
#define BTGW_LAUNCH_H

#include <stdbool.h>
#include <stddef.h>

/*
 * One boot list for Lua scripts and PEX apps (cfg/launch.json).
 *
 * launch_boot() runs once, after boot_after_watchdog is known and before
 * watchdog_enable(). A normal boot arms watchdog scratch 2. A second reset
 * before the main loop has been healthy for about a second finds that arm
 * and is safe mode: nothing starts. A watchdog boot clears the arm and
 * starts nothing, so the reboot after it is an ordinary one. A power cycle
 * clears the scratch, so it is not safe mode.
 *
 * launch_poll() is the only starter. It waits for the list's gate, then
 * starts enabled entries one at a time. The next entry waits until the one
 * just started has no connection attempt in flight and its negotiation has
 * gone quiet, so two programs do not call le_connect together. A program
 * that never connects, or a link that never finishes, gives up that wait
 * rather than holding the rest of the list. The old cfg/autostart.json and
 * cfg/pex_autostart.json timers do not start anything. They are the list
 * only while cfg/launch.json is absent.
 */

void launch_boot(void);
void launch_poll(void);

/* True on the safe-mode boot (a second reset), for sys.info. */
bool launch_safe_mode(void);

/* Extra Bluetooth hold while the list is waiting on WireGuard. The Wi-Fi
 * grace is unchanged. Meshtastic does not hold the radio. Launch now and a
 * skipped boot leave this false. */
bool launch_radio_hold(void);

size_t launch_status_json(char *dst, size_t cap);

#endif
