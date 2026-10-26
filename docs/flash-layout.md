# Flash layout and the LittleFS migration

The gateway's flash is a 4 MB W25Q32 on the Pico 2 W. Four copies of the map
must agree, and `tools/run_tests.py` ("OTA flash layout is consistent")
checks them against each other: `partitions.json` (what the boot ROM reads),
`src/fs/fs.h` (`FS_FLASH_OFF`, `FS_SIZE`, `FS_PEX_OFF`, `FS_PEX_SIZE`,
`FS_BT_TAIL`), `linker/pico_flash_region.ld` (one slot) and `fs.c`.

## The map

| Offset | Size | What | Notes |
|---|---|---|---|
| `0x000000` | 64 KB | partition table | unpartitioned; `tools/provision.sh` writes it |
| `0x010000` | 1024 KB | Main A | firmware slot (`rp2350-arm-s`); executes at `0x10000000` |
| `0x110000` | 1024 KB | Main B | same linked image as A; OTA writes the inactive one |
| `0x210000` | 256 KB | Wi-Fi Firmware | CYW43 blob, provisioned once, never OTA'd |
| `0x250000` | **1200 KB** | LittleFS | `cfg/`, `lua/`, web assets. 300 blocks |
| `0x37C000` | **512 KB** | **PEX** | native modules, executed in place ([pex-format.md](pex-format.md) 6.1). Id `0x7065780000000000` ("pex"), family `data` |
| `0x3FC000` | 16 KB | BTstack NVM | bonds + LE device DB; unpartitioned, never touched |

On the RP2350 the QSPI XIP controller maps either Main slot onto
`0x10000000`, which is the only address the image is linked at. The same
bytes boot from A or from B. An RP2040 XIP window does not translate, so
the same layout there would be two images: one linked for slot A's
addresses and one linked for slot B's. This table is the RP2350 map.
[OTA.md](../OTA.md) has the boot rule.

The bench board is already on the table above. This image mounts the 1200 KiB
volume at `0x250000` and does not run the layout mover. The earlier shrink,
from 2032 KiB at `0x200000` down to 1776 KiB, is what the rest of this page
describes. It has finished on that board (`cfg/migrated.json`).

```json
"fs": {"kb": 1200, "layout_kb": 1200, "migration": "done",
       "pex": {"offset": 3653632, "size": 524288, "state": "reserved"}}
```

`kb` is the geometry actually mounted (2032 while a migration is pending),
`migration` is `none` (a filesystem formatted in the new layout), `done`
(migrated, `cfg/migrated.json` present), `migrated this boot: ...`, or
`migration pending: <why>`. PEX `state` is `inside the old LittleFS until it
migrates` while the old geometry is still mounted.

## Why a migration is needed at all

littlefs cannot shrink a filesystem, and refuses (`LFS_ERR_INVAL`) to mount
one whose superblock block count differs from its configuration. The
firmware used to **format on any mount error**, so simply shipping a smaller
`FS_SIZE` would have wiped `cfg/` -- Wi-Fi, Ethernet and the WireGuard keys,
the only way to reach a remote gateway -- on the first boot.

So `fs_init()` now mounts a mismatching filesystem **as found** (block count
taken from the superblock, bounded by the old region). The move itself is
`CONFIG_FS_MIGRATE` (`src/fs/fs_migrate.c`), which defaults to **n**. A
normal image does not link it. An old geometry still mounts, `sys.info` and
the root of `/api/fs` say `migration pending: not in this build`, and the
PEX partition stays inside that filesystem so nothing is written over it.
Set the option (it needs `CONFIG_SD`) for the one boot that copies through
the card, **before** any configuration is read, so Wi-Fi, Ethernet and
WireGuard come up from the restored files.

**Firmware older than this layout formats a migrated filesystem** (it sees
444 blocks where it expects 508, fails to mount, formats). Never boot a
pre-PEX image on a migrated board -- any build without this change (its
`sys.info` has no `"fs"` object; 1.187 and older on feature/lua-net-fs, every
dev build so far) -- not by OTA rollback, not by arming the old slot. See
"OTA" below.

## The migration

Runs when the mounted filesystem is larger than `FS_SIZE`, or when a
migration was cut short (`.migrating` in the root of LittleFS, or an empty
filesystem with a matching state on the card).

1. **Checks, nothing written yet.** The files must fit the new size (blocks in
   use + 8 <= 444); the SD card must be there (5 s to mount) and have room
   (all bytes + 128 KB per file + 1 MB). Any failure: *pending*.
2. **Copy.** State file `sd:/.lfs-migrate/state` says `copying`, run `<n>`.
   Every file (all folders, recursively, `cfg/` included) is copied to
   `sd:/.lfs-migrate/<n>/f/<index>`, then **read back from the card** and
   checked (size + CRC-32 against what was read from flash). Paths, sizes and
   CRCs go into `sd:/.lfs-migrate/<n>/manifest` (text: `D <path>`,
   `F <index> <size> <crc32> <path>`, and a final `E <entries> <crc32>` over
   every line before it). Numbered files sidestep FAT's case-insensitivity,
   its name rules and the job path limit. `cfg/` is also kept in RAM. The
   manifest is read back and checked too. Any failure: *pending*, nothing
   formatted.
3. **Point of no return.** State `formatting` (with the run, the counts, the
   board's unique id, an attempt counter). From now on the old filesystem is
   never trusted again: every boot restores from the card until a restore has
   been verified.
4. **Format** LittleFS with 444 blocks; the first thing written is `.migrating`
   in its root (an in-progress note no API can reach).
5. **Restore `cfg/` first**, then everything else, from the card. Each file
   is checked as it is read from the card (CRC against the manifest; a misread
   is retried) and **read back from flash** and checked again. A failed pass
   is retried once from a fresh format. If it still fails, `cfg/` is written
   back from RAM (and verified), the board runs, and the next boot retries
   (at most 3 boots; then *pending: restore by hand*).
6. **Finish.** State `done`, then `cfg/migrated.json`
   (`{"from_kb":2032,"to_kb":1776,"entries":..,"files":..,"bytes":..,"run":..,"sd":".lfs-migrate/<n>","ms":..}`),
   then `.migrating` is removed. The copy on the card is kept as a
   backup; delete `sd:/.lfs-migrate/` by hand when no longer wanted.

Everything runs in the boot path with the watchdog fed (every SD job, every
4 KB of flash I/O) and the SD storage worker driven from there (`sd_poll`),
so card I/O stays in the worker as everywhere else. The network is not up
yet; RTT shows progress (`migrate: ...` lines). In mcusim the real gateway's
filesystem (193 files, 30 folders, 680 KB) migrates in ~30 s of device time.

### Failure handling

| What | Result |
|---|---|
| no SD card / card does not mount within 5 s | *pending: no SD card*; old geometry mounted, board fully working |
| card identifies but every read fails (e.g. a data line not driven) | *pending: SD card present but not readable (see /api/sd)*; RTT says which line (`sdio: CRC error ...; DAT3 high`) |
| card too small | *pending: SD card full (needs N KB free)* |
| a write, close or read-back fails during the copy, card pulled | *pending: SD copy failed: ...; nothing formatted* |
| files would not fit 1776 KB | *pending: files need N KB ...* |
| try-before-you-buy image not bought yet | *pending: waiting for the OTA confirm*; after the confirm the board reboots by itself (3 s) and migrates |
| power loss during the copy | next boot copies again (same run) |
| power loss after `formatting`, during the format or the restore | next boot sees the state (old geometry still mounting, an empty filesystem, or `.migrating`) and restores from the card again |
| power loss after `done` | the marker or `.migrating` says how far; it finishes or verifies again |
| card unreadable after the format | `cfg/` from RAM (this boot) or restored first (later boots); *pending: restore incomplete* |
| a state file from another board on the card | ignored (board id), a new run |

*Pending* is reported in `sys.info` (`fs.migration`), on `GET /api/fs/`
(`"migration":"..."`) and over RTT, and is retried on every boot.

A build without `CONFIG_SD` never migrates (*pending: no SD card support in
this build*) and keeps the old geometry.

## OTA and the migration

OTA carries the new firmware but **not** the new partition table (the ROM's
table is only rewritten over SWD/BOOTSEL, `tools/provision.sh`). That is
fine: the firmware places LittleFS and PEX by `fs.h`, not by the table, and
the migration needs only the card. On a board updated over OTA:

* The new image boots try-before-you-buy and **does not migrate** while it
  is unbought: until the confirm, any reboot goes back to the old firmware,
  which would format a migrated filesystem. It reports *pending: waiting for
  the OTA confirm*.
* After `smpmgr ... image state-write --confirm`, it reboots by itself and
  migrates (card required).
* Afterwards the other slot still holds the old firmware: **do not roll back
  to it** (it would format LittleFS; the card's copy would still be there to
  restore by hand from `sd:/.lfs-migrate/<n>/`).

## First board: over SWD with the debugger

The plan in [pex-format.md](pex-format.md) section 14: flash the new partition
table and a non-TBYB build into slot A over SWD (`tools/provision.sh`, which
leaves LittleFS and the BTstack NVM alone), watch the migration over RTT,
check WireGuard. Take a raw flash dump first (`dump_image`), and put the
SD card in.

## Testing

`test/sim/lfs_migrate.py` runs it all in mcusim against an image made by the
old firmware: the happy path (every file byte-identical, `cfg/` identical,
config read after the migration, Wi-Fi joined and WireGuard configured from
the restored files, PEX in `sys.info`), no card (then a card), a full card,
failing writes, a pulled card, power cuts at eight points through the
migration, two cuts in a row, and an unbought TBYB image. Its host tools
`test/sim/lfs_tool.c` and `test/sim/sd_tool.c` read and write LittleFS and
FAT/exFAT images with the firmware's own littlefs and FatFs.
