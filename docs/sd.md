# SD card

An optional microSD card as a second filesystem next to the LittleFS on
flash: FatFs (FAT12/16/32 and exFAT), reached over HTTP at `/api/sd` (the
same API as km003c_rp2_webapp) and shown in the web editor as a top-level
`sd` folder. Two buses: **SDIO** 4-bit on PIO, and **SPI** through the shared
SPI bus layer. No board has the slot yet. The feature is compiled, and
tested in simulation, so it is ready when one does.

Status: every layer above the wire is tested on the host against a
simulated card, on both buses (tier 0). The firmware builds themselves run
in mcusim (`tools/run_tests.py --sim`): boot, the storage worker switching on
the M33, `/api/sd` over the simulated USB network, and, with card detect
held low and nothing on the bus, the PIO driver and the SPI driver (on SPI1
after the absent W5500 let go of it) identifying, failing and retrying
cleanly. **Not yet run against a card**: real data over the PIO/DMA wire
waits for mcusim's SD card model (then the differential test against
carlk3's driver, "Testing"), then hardware.

## Build

`CONFIG_SD` (default **n** in every shipped defconfig). `menuconfig` →
"SD card":

| Option | Default | |
|---|---|---|
| `SD_BUS_SDIO` / `SD_BUS_SPI` | SDIO | the bus |
| `SD_SDIO_CLK`, `SD_SDIO_CMD` | 16, 17 | any GPIOs (CLK is not tied to D0) |
| `SD_SDIO_D0` | 18 | D1–D3 = D0+1..D0+3 (consecutive) |
| `SD_SDIO_PIO` | −1 | −1 = whichever PIO has room |
| `SD_SDIO_KHZ` | 25000 | requested; the clock is sys/7 at most (21.4 MHz at 150 MHz) |
| `SD_SPI_BUS`, `_SCK`, `_MOSI`, `_MISO` | 1, 14, 15, 8 | the W5500's SPI1, shared |
| `SD_SPI_CS` | 9 | |
| `SD_SPI_KHZ` | 25000 | |
| `SD_DETECT`, `SD_DETECT_LEVEL` | 22, 0 | −1 = no switch (a card is assumed present) |
| `SD_XFER_BUF`, `SD_XFERS` | 4096, 2 | HTTP transfer rings |
| `SD_WORKER_STACK` | 4096 | |

Builds: `configs/pico2w_sd_defconfig` (pico2w_eth + SDIO) and
`configs/pico2w_sdspi_defconfig` (pico2w_eth + SPI sharing SPI1 with the
W5500). Cost over pico2w_eth: SDIO +59 KB flash, +17.8 KB RAM; SPI +52 KB,
+16.7 KB (most of the flash is FatFs with exFAT and mkfs).

Wiring (SDIO): CLK GP16, CMD GP17, D0–D3 GP18–21, card detect GP22 (switch
to GND), 3.3 V, pull-ups on CMD and D0–D3 (internal ones are enabled; a
socket with 10–50 kΩ externals is better). The radio's GP23/24/25/29 and the
W5500's GP8/10/11/13/14/15 are untouched. Every SD pin is reserved in the
pin broker, so a script can never claim one.

## Layers (`src/sd/`)

```
HTTP /api/sd (src/net/http.c, CONFIG_SD)      main loop: holds the connection,
        |                                     moves bytes (http_sd_pump)
sd_api.c   transfers, format, bench, status   <- ported from km003c sd_api.c
sd_core.c  the worker loop: card detect, settle, mount, jobs
sd_store.c ls_* on FatFs                      <- km003c log_store_fatfs.c
third_party/fatfs  FatFs R0.15 + km003c's ffconf.h
sd_diskio.c  disk_* -> sd_card
sd_card.c  addressing, retries, counters, CSD/CID
   |-- sd_sdio.c  SD-mode protocol             (own, from the SD spec)
   |      sdio_hw.h -> sdio_hw_pico.c + sdio.pio (PIO + DMA, own)
   `-- sd_spi.c   SPI-mode framing on spibus   <- km003c km_sd.c framing
```

All card I/O runs in the **storage worker**, km003c's stackful coroutine
(`sd_worker_coro.c`), resumed once per main-loop pass (`sd_poll()`); it
yields every ~2 ms and on every wait, so a slow card never stalls USB, WiFi,
Bluetooth or HTTP. It never runs inside `cyw43_wait.c` (tier-0 guard).

### SDIO

Our own design (carlk3's driver is ZuluSCSI's GPL-3.0 code; pico-extras'
`pico_sd_card` is a read-only prototype):

* The **command** state machine is the only clock source: 7 PIO cycles per
  SD clock (3 low, 4 high), CMD driven after the falling edge and sampled at
  the rising one. A command is bits out then a response window clocked in;
  the start bit is found in software.
* The **data** state machine never drives CLK: it follows it with absolute
  `wait gpio` (patched to the CLK pin at load). So CLK may be any pin, and the
  two programs may sit in different PIO blocks.
* A follower cannot pause the clock, so a read's DMA is fully armed before its
  command: a control channel walks a list of `{address, count}` pairs (every
  block and every CRC) into the data channel. Reads run in groups of up to 32
  blocks. A write chains data and CRC by DMA, runs exactly 1043 clocks, then
  reads the CRC status token; busy is D0, polled while clocking.
* The four D0–D3 CRC16s are one CRC64 with P(x⁴) = x⁶⁴+x⁴⁸+x²⁰+1, table
  driven (`sd_crc.c`), checked bit by bit in the tests.
* Data is always 4-bit (mandatory for SD memory cards).

### SPI, on a shared bus

The card may share SPI1 with the W5500. The bus semaphore makes every
transaction exclusive. What an SD card needs on top (`src/io/spibus.c`):

* fill byte 0xFF for reads (`spidev_t.fill`);
* power-up clocks with CS high (`spibus_xfer_*_cs`);
* **`spibus_release_trailing()`**: a card drives MISO until it sees a clock
  with CS high, so every SD transaction ends with that clock *before* the
  semaphore is given back (a plain release corrupts the W5500's next read; the
  host test shows both);
* busy waits release the bus between polls, so a 1.5 s write busy does not
  starve the W5500;
* bus refcount: an absent W5500 no longer closes a bus the card shares.

## HTTP API — `/api/sd`

Without `CONFIG_SD` every `/api/sd` path answers `404` (the web app checks
`sys.info.modules` for `sd`).

| Method | Path | |
|---|---|---|
| `GET` | `/api/sd[?reset=1]` | Status (below). Always `200`. `reset=1` clears the maxima after reading |
| `POST` | `/api/sd/format` | `{"fs":"exfat"\|"fat32","cluster":N}` (0/absent = default). `202`, then follow `format` in the status. `409` while one runs |
| `POST` | `/api/sd/bench` | `{"mb":N}` (default 10). `202`; results in the status `bench` |
| `GET` | `/api/sd/files[/<path>]` | A folder: `{"dir":"…","files":[{"name":"…","dir":true},{"name":"…","size":123}]}` (streamed, no Content-Length). A file: its bytes with Content-Length and a type by extension |
| `PUT` | `/api/sd/files/<path>` | Upload, streamed to the card with TCP back-pressure; parents created; an existing file replaced. `{"ok":true,"bytes":n}`. `507` before writing if it cannot fit |
| `PUT` | `/api/sd/files/<path>/` | Make a folder (and its parents). `{"ok":true,"dir":true}` (added here; km003c has no mkdir) |
| `DELETE` | `/api/sd/files/<path>` | A file or an empty folder |
| `POST` | `/api/sd/rename` | `{"from":"a/b","to":"c/d"}`; the target's parents are made; `409` if it exists |

Paths: relative to the card's root, `/`-separated, percent-escaped per
segment (decoded on the device), at most 96 characters; `..`, `.`, empty
segments, control characters and `\ : * ? " < > |` are refused (`400`).

Errors are `{"ok":false,"error":"…","card":"<state>"}` with `Connection: close`:

| | Status |
|---|---|
| no card / settling / not mountable / formatting | `503` with `Retry-After` 5 / 2 / — / 5 |
| the transfer slots are busy | `503`, `Retry-After: 1` |
| a non-empty folder; an existing rename target; PUT onto a folder | `409` |
| not found | `404` |
| card full | `507` |
| card error | `500` |
| card pulled mid-download | the connection is aborted: a short body, never wrong data |

```json
{"ok":true,"present":true,"mounted":true,"state":"ok","fs":"exfat",
 "total":62400000000,"free":62300000000,
 "bus":{"type":"sdio","width":4,"clock_khz":21428},
 "format":{"available":true,"state":"idle","phase":"","done":0,"total":0,"fs":"","cluster":0,"error":""},
 "card":{"bytes":62537072640,"type":"SDXC",
         "cid":{"mid":3,"oem":"SD","name":"SN64G","rev":"8.0","serial":"0x1a2b3c4d","date":"2024-05"}},
 "defaults":{"fs":"exfat","cluster":{"fat32":32768,"exfat":131072},"align":16777216},
 "options":{"fat32":[4096,…,65536],"exfat":[4096,…,1048576]},
 "bench":{"state":"idle","mb":0,"chunk":4096,"done_kb":0,"write_kBps":0,"read_kBps":0,"card_busy_max_us":0,"error":""},
 "worker":{"stack":4096,"stack_used":1480,"xfers":0,"xfer_max":2,"slices":…,"slice_max_us":…,
           "card_busy_max_us":…,"card_busy_timeouts":0,"card_write_retries":0,"card_dma_blocks":…,
           "card_read_retries":0,"card_crc_errors":0,"card_inits":1,"xfer_bytes":…,
           "last_fail":{…},"last_conn_end":{…}}}
```

Compared with km003c: no `logging` member, a `bus` member added, worker
counters for the card (read retries, CRC errors, inits) instead of the
logger's. `state` is `ok`, `none`, `settling` (2 s after insertion),
`error` (present but not mountable: format it) or `formatting`. A format
writes an MBR partition table and one FAT32 (`0x0C`) or exFAT (`0x07`)
partition aligned to 4 MiB up to 32 GiB, 16 MiB above.

WebSocket: `sd.status` returns the same JSON.

## Testing

Tier 0 (host, `tools/run_tests.py`):

* `sd_crc_test`: CRC7/CRC16 against the spec's examples and bit-serial
  references; the CRC64 = four interleaved CRC16s claim.
* `sd_card_test` (72 checks): both buses through `sd_card.c` against one
  simulated card (`test/host/sdcard_sim.c`: SD-mode state machine, R1–R7,
  SPI tokens and data response, busy in simulated time, SDSC/SDHC/v1,
  faults; its CRCs computed independently). SDHC/SDSC/v1/slow power-up,
  1–64 block I/O at the edges, CRC and write errors retried, long busy
  (and the W5500 getting the bus meanwhile), a dead command, a pulled and
  reinserted card, no MISO conflict on the shared bus.
* `sd_stack_test` (125 checks): worker → API → store → FatFs → driver →
  card, both buses, FAT32 and exFAT on a 300 MiB card: upload, download,
  listing, mkdir, rename, delete, the error map, bench, pull + settle +
  remount. The resulting images pass `fsck.fat` / `fsck.exfat`.
* guards: worker-only, trailing-clock releases, pins reserved, no GPL
  SDIO source in the tree.

Tier 1: `pico2w_sd` and `pico2w_sdspi` build; the four other builds link no
SD code.

Tier 2: Playwright against the devserver's mock card (`web/tests/e2e/sdcard.spec.js`):
the editor's `sd/` folder (browse, open, edit and save, New folder, the
128 KB flash limit not applying, a cross-root move refused, no card, a build
without the module) and Settings › SD card (status, format with
confirmation and progress, bench).

`tools/run_tests.py --sim` (needs `~/Work/mcusim` built): the SD
builds booted in mcusim with card detect low and no card -- state `error`,
retries, no worker slice over 10 ms, `/api/sd` answering -- and the
**differential test** (`test/sdio_diff/`,
`tools/sdio_diff.py`). One seeded script on our SDIO driver and on
carlk3's, each in mcusim on the same simulated card with the same faults;
compared: every result class, read hashes, identification, the final card
image (raw scenario) or file hashes plus fsck (FatFs scenario), and the
card's normalised command trace (`test/sdio_diff/allowlist.txt` for accepted
differences, each with a reason). carlk3's library is fetched at a pinned
commit into the build directory only (GPL-3.0 SDIO core: never in this tree,
never in the gateway). Today the four ELFs build and run in mcusim; the
comparison is skipped until mcusim has its `sdcard` device (the stanza it
needs is `CARD_TOML` in `tools/sdio_diff.py`). Then: the gateway ELFs
(`pico2w_sd`, `pico2w_sdspi`) end to end in mcusim over USB-NCM; then
hardware.

## Web

* `web/js/fs.js` routes any path under `sd/` to `/api/sd/files` (same
  answers as `/api/fs`), adds the `sd` folder at the root when `sys.info`
  lists the `sd` module, refuses a move between the card and flash, and has
  no 128 KB limit there (FAT32's 4 GB).
* The editor keeps flash-only rules off the card: no `.bak` on save, no
  compression, no tar download of a card folder, Lua script status only for
  `lua/` and `staged/`, `sd/` skipped by a whole-device search (search it by
  starting inside). The card's own folder has no rename/delete and links to
  its settings; every folder now has "New folder here…".
* Settings › SD card (`web/js/settings/sdcard.js`, km003c's format
  arithmetic in `sd-format.js`): state, card and CID, filesystem and space,
  bus and clock; Format with the cluster sizes the card can take and a
  confirmation naming it, with progress; a 10 MB bench.
