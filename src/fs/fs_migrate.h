/* Moving a pre-PEX LittleFS (2032 KiB) into the smaller one (FS_SIZE) that
 * leaves room for the PEX partition, through a verified copy on the SD card.
 * docs/flash-layout.md has the whole story; in short:
 *
 *   boot, fs_init() mounted the old geometry as found (fs_legacy())
 *     -> copy every file to sd:/.lfs-migrate/<run>/ and verify it (CRC-32 +
 *        size, read back from the card), cfg/ also into RAM
 *     -> state "formatting" on the card -> format the new geometry
 *     -> restore cfg/ first, then the rest, verifying each file on flash
 *     -> state "done" on the card, cfg/migrated.json on flash.
 *
 * Nothing is formatted unless the copy on the card is complete and verified:
 * no card, a full card, a write that fails, or files that would not fit leave
 * the board on the old geometry, working, with "migration pending: <why>" in
 * sys.info, /api/fs and over RTT. A reboot at any point resumes (the state
 * file on the card says how far it got).
 *
 * fs_migrate_boot() runs in main() after fs_init() and BEFORE the network
 * config is read, so Wi-Fi, Ethernet and WireGuard come up from the restored
 * files. It feeds the watchdog and drives the SD worker itself.
 *
 * CONFIG_FS_MIGRATE defaults to n. The .c file is linked only then. With it
 * off, fs.c still answers these three: boot records a pending note when the
 * mounted geometry is the old one, and nothing is formatted. */
#ifndef BTGW_FS_MIGRATE_H
#define BTGW_FS_MIGRATE_H

#include <stddef.h>

void fs_migrate_boot(void);
/* Main loop: after an OTA confirm (buy), the reboot that runs a migration a
 * try-before-you-buy boot had to leave pending. A no-op when the migrator
 * is not linked. */
void fs_migrate_tick(void);
/* {"kb":..,"layout_kb":..,"migration":"..","pex":{...}} for sys.info. */
int fs_migrate_json(char *buf, size_t cap);

#endif
