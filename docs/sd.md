# SD card

An optional microSD card as a second filesystem next to the LittleFS on
flash: FatFs (FAT12/16/32 and exFAT), reached over HTTP at `/api/sd` (the
same API as km003c_rp2_webapp). A file or folder is also readable at
`/sd/<path>`. The web editor shows the card as a top-level `sd` folder. Two buses: **SDIO** 4-bit on PIO, and **SPI** through the shared
SPI bus layer. No board has the slot yet. The feature is compiled, and
tested in simulation, so it is ready when one does.

Status: **SDIO works on hardware** (2026-10-19, Pico 2 W / RP2350 A2,
Adafruit SDIO breakout, a 32 GB SDHC card, updated and driven over the
WireGuard tunnel):

| | |
|---|---|
| identification | CMD0 → ACMD6, 4-bit, 18.75 MHz |
| format (MBR + FAT32, 32 KB clusters) | 6 s, no retries |
| 4 MB random file up and down (tunnel / USB) | byte-identical |
| mkdir, rename, list, delete (non-empty refused) | as specified |
| bench, 10 MB on the card | write 2.0 MB/s, read 2.7 MB/s |
| retries / CRC errors / timeouts | 0 / 0 / 0; longest worker slice 4.3 ms |

SPI mode (SPI0, 25 MHz, CRC on) passed the same card tests on the same
wiring on 2026-10-20: W 1020 / R 1194 kB/s, 0 errors. The
differential test against carlk3's driver waits for mcusim's SD card model.

What bring-up found (each now has a host test that fails without the fix):

* **Busy is only visible while clocked.** One look at D0 right after the CRC
  status token read "not busy"; the next CMD24 met a card still
  programming, which ignores it (illegal outside the transfer state).
  `sdio_hw_wait_busy` now clocks first, and every write ends with CMD13
  until the card is back in the transfer state.
* **A garbled answer can hide a command the card took.** A CMD24 whose
  response failed its CRC left the card waiting for data ("rcv"), ignoring
  every write after. A failed CMD17/18/24/25 now stops the transfer (CMD12),
  and the CMD13 wait ends any transfer it finds still open.
* **CMD was sampled too close to the edge.** At 7 PIO cycles a clock
  (21.4 MHz) CMD was sampled at the rising edge, ~9 ns after the card's
  latest output change; responses occasionally failed their CRC. It is now
  sampled one cycle later, 8 cycles a clock (18.75 MHz).
* A short from GP17 (CMD) to ground on the board (fixed on the board).
* **Card detect is active-high on Adafruit's SDIO breakout**: it drives DET
  high with a card in and low without (both ways, not a switch to ground), so
  the default "low = present" read the card as absent. `pico2w_sd` and
  `pico2w_sdspi` now set `SD_DETECT_LEVEL=1`; on hardware the state goes
  `none` → `settling` (2 s) → `ok` on a pull and reinsert.
* A card seated badly after a hot reinsert gave CRC errors on every read
  (all caught -- never wrong data -- but listings came back empty and free
  space 0) until it was reseated: nothing ever re-identified the card.
  **Recovery** (`sd_card.c`): a read or write that still fails after its
  retry with a CRC error or timeout re-identifies the card (CMD0 onwards:
  clears a card left mid-transfer or on another bus width) and tries the
  operation again; if failures go on, each further recovery steps the data
  clock down (18.75 → 12 → 6 → 3 → 1 MHz) for a marginal contact. Removing
  the card restores full speed. The card's own error answers never trigger
  it. Status: `worker.card_recoveries`, `bus.clock_khz`. Host tests: a wedged
  card (recovered within the same read, no slowdown) and a contact that fails
  above 10 MHz (readable and writable at 6 MHz). On hardware, repeated hot
  pulls and reinserts during continuous 4 MB reads came back to correct
  reads at full speed every time with no manual step.

## Build

`CONFIG_SD` is off in `configs/pico2w_defconfig`. `pico2w_sd`,
`pico2w_sdspi`, and `pico2w_pexcc` turn it on. `menuconfig` →
"SD card":

| Option | Default | |
|---|---|---|
| `SD_BUS_SDIO` / `SD_BUS_SPI` | SDIO | the bus |
| `SD_SDIO_CLK`, `SD_SDIO_CMD` | 2, 3 | any GPIOs (CLK is not tied to D0) |
| `SD_SDIO_D0` | 4 | D1–D3 = D0+1..D0+3 (consecutive) |
| `SD_SDIO_PIO` | −1 | −1 = whichever PIO has room |
| `SD_SDIO_PULLUPS` | y | internal pull-ups on CMD and D0-D3 (pull-ups only: RP2350-E9 on A2 silicon is a pull-down problem) |
| `SD_SDIO_KHZ` | 25000 | requested; the clock is sys/8 at most (18.75 MHz at 150 MHz) |
| `SD_SPI_BUS`, `_SCK`, `_MOSI`, `_MISO` | 0, 2, 3, 4 | SPI0, on the SDIO wiring (card CLK, CMD, D0) |
| `SD_SPI_CS` | 7 | the card's D3 |
| `SD_SPI_KHZ` | 25000 | |
| `SD_DETECT`, `SD_DETECT_LEVEL` | 22, 0 (9, 1 in the SD defconfigs) | −1 = no switch (a card is assumed present); 1 = high means present (Adafruit's breakout) |
| `SD_XFER_BUF`, `SD_XFERS` | 4096, 2 | HTTP transfer rings |
| `SD_WORKER_STACK` | 4096 | |

Builds: `configs/pico2w_sd_defconfig` (pico2w_eth + SDIO) and
`configs/pico2w_sdspi_defconfig` (pico2w_eth + SPI on SPI0), on the same
wiring. Cost over pico2w_eth: SDIO +59 KB flash, +17.8 KB RAM; SPI +52 KB,
+16.7 KB (most of the flash is FatFs with exFAT and mkfs).

Wiring, the same for **both** buses:

| card | GPIO (pin) | SDIO | hardware SPI0 |
|---|---|---|---|
| CLK | GP2 (4) | CLK | SCK |
| CMD | GP3 (5) | CMD | MOSI (TX) |
| D0 | GP4 (6) | D0 | MISO (RX) |
| D1 | GP5 (7) | D1 | pulled up |
| D2 | GP6 (9) | D2 | pulled up |
| D3 | GP7 (10) | D3 | CS |
| DET | GP9 (12) | card detect | card detect |

SDIO only needs D0–D3 consecutive (CLK and CMD may be any pins). The RP2350's
SPI pins have fixed roles -- GPIO n is SPI(n/8 mod 2), RX/CSn/SCK/TX for
n mod 4 = 0/1/2/3 -- and GP2/3/4 are SPI0's SCK/TX/RX: the card's CLK/CMD/D0.
3.3 V; pull-ups on CMD and D0–D3 (the breakout's, or the internal ones,
`SD_SDIO_PULLUPS`). The radio's GP23/24/25/29 and the W5500's
GP8/10/11/13/14/15 are untouched; in SPI mode the card is alone on SPI0. Every
SD pin is reserved in the pin broker, so a script can never claim one. (The
first hardware runs used CLK 16 / CMD 17 / D0–D3 18–21, which suits SDIO
only: GP16 is an SPI RX pin and cannot clock a card.)

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
(`sd_worker_coro.c`), resumed once per main-loop pass (`sd_poll()`), so a
slow card never stalls USB, WiFi, Bluetooth or HTTP. It never runs inside
`cyw43_wait.c` (tier-0 guard). A slice ends -- the worker yields -- at the
first of these once it has run `SD_SLICE_US` (2 ms):

* before every block read or write (`sd_card.c`), every SDIO command and
  every block of a multi-block SDIO write;
* in every wait (`sd_port_wait_step()`: after a 200 µs spin, or once the
  budget is spent -- so a run of short waits, e.g. a 400 kHz command
  sequence, still yields);
* SPI: only between commands, with the bus released first (`breathe()` in
  `sd_spi.c`), never holding a bus another device may share -- including
  between ACMD41 polls while a card powers up (up to 1 s);
* FatFs's CPU-only loops: `FF_YIELD()` (a local addition, see
  `third_party/fatfs/VENDORED.txt`) in f_mkfs's exFAT up-case table, which
  made 65536 `ff_wtoupper()` calls without a disk access: a **227 ms** main
  loop stall per exFAT format until 2026-10-20.

Measured on the board (SDIO, 2026-10-20): longest slice 2.2 ms through boot
+ mount, an exFAT format, a 10 MB bench and a 4 MB round trip (it was 227 ms
in the format). The status reports the longest slice and what the worker
was doing in it (`slice_max_us`, `slice_max_phase`: `media` = detect, mount,
free space; `format`; `get`, `put`, `bench`, ...).

What still runs without a yield, by design (each bounded):

| | worst case |
|---|---|
| one SPI command at 400 kHz (identification only) | ~0.4 ms of blocking bytes |
| one block of FatFs work between two disk accesses | ~0.1 ms |
| SDIO: stopping the PIO clock (`clk_leave`: queued bursts drain, `sm_idle`) | normally µs; bounded at 2 ms by `sm_idle`, the drain wait by 100 ms (a stuck PIO) |
| SPI: a blocking few-byte transfer (`spibus_xfer_blocking`) | µs; 50 ms timeout |

### SDIO

Our own design (carlk3's driver is ZuluSCSI's GPL-3.0 code; pico-extras'
`pico_sd_card` is a read-only prototype):

* The **command** state machine is the only clock source: 8 PIO cycles per
  SD clock (3 low, 5 high), CMD driven after the falling edge and sampled
  one PIO cycle after the rising one. A command is bits out then a response window clocked in;
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

CRC is **on** in SPI mode: CMD59 right after CMD0 turns the card's checking
on, every command carries its CRC7, every block written its CRC16, and every
block read (CSD and CID too) is checked -- so a bad contact shows up as CRC
errors and drives recovery, as on SDIO. (km003c's driver left it off; then
bad data passes silently.) A card that refuses CMD59 runs without it;
`bus.crc` in the status says which.

The default wiring puts the card alone on SPI0, but it may also share a bus
(e.g. SPI1 with the W5500: GP14/15/8 and a CS of its own). The bus semaphore
makes every transaction exclusive. What an SD card needs on top (`src/io/spibus.c`):

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
`sys.info.modules` for `sd`). `/sd` is claimed only when the card is
compiled in. On a build without it, that URL is an ordinary LittleFS path.

| Method | Path | |
|---|---|---|
| `GET` | `/api/sd[?reset=1]` | Status (below). Always `200`. `reset=1` clears the maxima after reading |
| `POST` | `/api/sd/format` | `{"fs":"exfat"\|"fat32","cluster":N}` (0/absent = default). `202`, then follow `format` in the status. `409` while one runs |
| `POST` | `/api/sd/bench` | `{"mb":N}` (default 10). `202`; results in the status `bench` |
| `GET` | `/api/sd/files[/<path>]` | A folder: `{"dir":"…","files":[{"name":"…","dir":true},{"name":"…","size":123}]}` (streamed, no Content-Length). A file: its bytes with Content-Length and a type by extension |
| `GET` | `/sd[/<path>]` | A page, the same rule as LittleFS. `name.gz` is preferred over `name` and answered with `Content-Encoding: gzip` and the type of the name without `.gz`. A folder, including `/sd` and `/sd/`, serves `index.html` (or `index.html.gz`). A folder with no index is `404`. A GET that finds both transfer slots busy waits on the connection until one frees. `PUT`, `DELETE`, and `POST` answer `404` with `{"error":"GET"}`. `/sdk` is not this route. Folder listings stay on `/api/sd/files` |
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
| the transfer slots are busy | a `GET` waits on its connection until a slot frees. `PUT`, `DELETE`, and rename still answer `503`, `Retry-After: 1` |
| a non-empty folder; an existing rename target; PUT onto a folder | `409` |
| not found | `404` |
| card full | `507` |
| card error | `500` |
| card pulled mid-download | the connection is aborted: a short body, never wrong data |

```json
{"ok":true,"present":true,"mounted":true,"state":"ok","fs":"exfat",
 "total":62400000000,"free":62300000000,
 "bus":{"type":"sdio","width":4,"clock_khz":18750},
 "format":{"available":true,"state":"idle","phase":"","done":0,"total":0,"fs":"","cluster":0,"error":""},
 "card":{"bytes":62537072640,"type":"SDXC",
         "cid":{"mid":3,"oem":"SD","name":"SN64G","rev":"8.0","serial":"0x1a2b3c4d","date":"2024-05"}},
 "defaults":{"fs":"exfat","cluster":{"fat32":32768,"exfat":131072},"align":16777216},
 "options":{"fat32":[4096,…,65536],"exfat":[4096,…,1048576]},
 "bench":{"state":"idle","mb":0,"chunk":4096,"done_kb":0,"write_kBps":0,"read_kBps":0,"card_busy_max_us":0,"error":""},
 "worker":{"stack":4096,"stack_used":1480,"xfers":0,"xfer_max":2,"slices":…,"slice_max_us":…,"slice_max_phase":"put",
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
* `sd_stack_test` (417 checks): worker → API → store → FatFs → driver →
  card, both buses, FAT32 and exFAT on a 300 MiB card: upload, download,
  listing, a 300-file folder (made, listed, emptied), mkdir, rename,
  delete, the error map, bench, pull + settle + remount. The resulting
  images pass `fsck.fat` / `fsck.exfat`. A **work meter** checks that the
  worker never goes long without a chance to yield: the firmware sources
  are built with `-finstrument-functions`, and `test/host/sd_worker_uc.c`
  counts the calls (CPU work) and simulated microseconds (waits, wire time)
  between yield opportunities, per operation, against bounds (4000 calls,
  1 ms). It names the call site when one is exceeded -- that is how the
  up-case loop was found (69977 calls); emptying `ff_yield()` fails it.
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
  no 128 KB limit there (FAT32's 4 GB). A browser loads a card page at
  `GET /sd/<path>`: a folder serves `index.html`, and a stored `.gz` is sent
  with `Content-Encoding: gzip`. The editor stays on `/api/sd/files`.
* The editor keeps flash-only rules off the card: no `.bak` on save, no
  compression, no tar download of a card folder, Lua script status only for
  `lua/` and `staged/`, `sd/` skipped by a whole-device search (search it by
  starting inside). The card's own folder has no rename/delete and links to
  its settings; every folder now has "New folder here…".
* Settings › SD card (`web/js/settings/sdcard.js`, km003c's format
  arithmetic in `sd-format.js`): state, card and CID, filesystem and space,
  bus and clock; Format with the cluster sizes the card can take and a
  confirmation naming it, with progress; a 10 MB bench.
