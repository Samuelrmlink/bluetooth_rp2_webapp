# Sharing an SPI bus

Several devices can share one hardware SPI block: the W5500 on SPI1, an SD
card in SPI mode, and a future MCP2515 CAN controller. Each device must keep
working while the others are busy. In particular, the W5500 must keep
passing traffic, and an MCP2515 must be read before its two receive buffers
overflow, **even while the SD card is in the middle of a format**.

This document is the contract. `src/io/spibus.c` implements the bus side,
each driver follows the device rules, and
`test/host/spibus_share_test.c` checks both (tier 0: "SPI bus shared
fairly").

## The model

- **One semaphore per bus.** A device claims the bus, runs one transaction,
  and releases it. Claiming sets the clock and SPI mode for that device.
  Chip select is a plain GPIO driven by the bus layer.
- **Transactions are short.** A transaction is one *unit*: a command and its
  response, or one data block. A device never holds the bus while waiting
  for its own chip. An SD card's busy and a W5500's command latency are both
  waited out with the bus released.
- **DMA moves the bytes in the background.** A unit is a chain of DMA
  segments with CS held low across all of it. The DMA IRQ starts the next
  segment. The completion callback runs from `spibus_poll()` in the main
  loop. The SD worker yields while its block's DMA runs, and keeps the claim
  while it does: a block is one unit.
- **Everything that claims runs in the main context.** That covers the main
  loop and the SD worker coroutine, which runs inside the main loop. Only
  `spibus_want_isr()` may be called from an interrupt.

## Fairness: the guarantees

**F1. Waiting is explicit.** A device is *waiting* from the moment it asks
until the bus is granted to it. It can ask in three ways:

- `spibus_try_claim()` is refused. The bus is busy, or it is someone else's
  turn. The device then holds a ticket and keeps it while it keeps trying.
- `spibus_want(dev)`, from the loop: "I will need the bus".
- `spibus_want_isr(dev)`, from an interrupt. It sets one byte, and the next
  bus call turns that byte into a ticket. This is for a device whose
  interrupt says there is work, such as the MCP2515's INT or the W5500's
  INT. A device that already asked this way is contended against
  immediately, even if the SD worker is mid-slice and the device's loop
  code has not run yet.

**F2. Grants follow ticket order.** `spibus_try_claim()` is refused whenever
another device holds an older ticket, even if the bus is free. A device
with no ticket stands behind everyone who has one. In practice, a holder
that releases while someone waits **cannot take the bus straight back**.
That includes reclaiming without yielding, inside one worker slice. The
releaser's claim is refused (`claim_yield` counts this), and the refusal
gives the releaser a ticket of its own. Two busy devices therefore
alternate unit by unit.

**F3. A holder lets go at its next safe point.** `spibus_contended(dev)` is
true while any other device waits. A driver checks it at every safe point
(below) and, if it is true, releases the bus and yields to the main loop.
Units are never split. A safe point is a boundary between units.

**F4. Wants lapse.** If a ticket is not renewed by a claim attempt within
`SPIBUS_WANT_TTL_MS` (10 ms), it is dropped. A driver that gave up, or a
device that was torn down, cannot stall the bus. `spibus_dev_deinit()` and
`spibus_unwant()` drop a ticket immediately.

**F5. A device alone on its bus is unaffected.** With nobody waiting there
are no tickets, no refusals, and the timing is identical to before. That
covers the W5500 on its own SPI1, and the SD card on its own SPI0 in the
default wiring.

**F6. Never two chip selects low.** No card may drive MISO while another
device is selected. An SD card keeps driving DO until it sees a clock with
CS high, so it releases with `spibus_release_trailing()`. The bus layer
refuses a release while a transfer is in flight.

## SD card rules (`src/sd/sd_spi.c`)

| Where | What the driver does |
|---|---|
| Every transaction | claim → unit(s) → `spibus_release_trailing(dev, 1)` (CS high + one clock) |
| Before each command | `give_way()`: if contended or the worker's slice is spent, release, yield, reclaim |
| Busy wait (`wait_ready`) | poll; once `SD_SPIN_US` has passed **or** someone waits, release, yield, reclaim, then CS low again. A card keeps programming with CS high (SD spec 7.2.4) |
| Multi-block write (CMD25) | after each accepted block (data response 0x05), if contended: release during the card's programming and resume with the next block's token |
| Multi-block read (CMD18) | after each block, if contended: CMD12, release, and resume with a new CMD18 at the next address. The card streams blocks for as long as it is clocked, so there is no in-transfer pause |
| ACMD41 power-up loop | release between polls |
| CMD0 retries, recovery | release between tries; recovery's busy waits go through `wait_ready` |
| Read data-token wait (N_AC) | **held**. The card is mid-command, and releasing there is not reliable across cards. Typically under 1 ms; bounded by `SD_READ_TIMEOUT_US`. This is the one documented exception |

`f_mkfs` is just many writes: zero-filled 8-sector work buffers, the FAT,
the up-case table and the root directory. Every block boundary is a safe
point, so a format holds the bus one block at a time.

## Worst-case wait for the other device

For a device that is already waiting (F1), the wait is

```
L = H + R + P
```

| Term | Meaning |
|---|---|
| H | the longest unit the holder runs between safe points |
| R | the release: CS high, one trailing clock (0.3 µs at 25 MHz), the semaphore given back |
| P | the rest of the main-loop pass before the waiter's step runs. The holder yields as soon as its own reclaim is refused |
| (S) | the reconfiguration on grant: `spi_set_baudrate` + `spi_set_format`, a few µs, paid only when clock or mode differ from the last holder's |

H for the SD card, at 25 MHz SPI (40 ns per bit, 0.32 µs per byte):

| Unit | Bytes | Wire time | With software overhead (5–6 small blocking exchanges, ~3–5 µs each) |
|---|---|---|---|
| Command + R1 (CMD24/25/17/18/12/13) | ~16 | 5 µs | ~30 µs |
| Write block: token + 512 + CRC16 + data response | 516 | 165 µs | ~0.2 ms |
| Read block: 512 + CRC16, after the token | 514 | 165 µs | ~0.2 ms, **plus N_AC** (typically 0.1–1 ms; ≤ 100 ms per the SD spec, ≤ 200 ms by our timeout) |
| Identification at 400 kHz: one command | ~16 | 0.32 ms | ~0.35 ms |
| Identification: CSD/CID read (token + 16 + CRC) | ~19 | 0.38 ms | ~0.4 ms + N_AC |

So at 25 MHz, **L ≈ 0.2 ms + P** during writes and **≈ 0.2 ms + N_AC + P**
during reads. During identification at 400 kHz, L ≈ 0.4 ms + P.

H for the W5500, at 30 MHz (seen by the SD card waiting on it): a register
access is 3 + n bytes (~1–2 µs). The largest unit is a full frame,
3 + 1514 bytes = 0.4 ms. An MCP2515 at 10 MHz: READ RX BUFFER is 1 + 13
bytes, 11 µs.

A main-loop device that has **not** asked yet (no want, its loop code not
run) is not waiting on the bus. It waits for the CPU. The worker slice
budget bounds that (`SD_SLICE_US`, about 2 ms), and the storage worker's
own rules guarantee it, independently of the bus. A device that must not
pay that latency registers from its interrupt (`spibus_want_isr`). Its
wait is then L, because the SD driver checks `spibus_contended()` at its
next safe point, mid-slice or not.

## Mode and clock per device

`took()` reprograms the block only when the new holder's baud or mode
differs from the current one: `spi_set_baudrate` computes a prescaler, and
`spi_set_format` sets CPOL/CPHA. That costs a few µs, which is small
against a unit. Devices may use different modes: the SD card is mode 0,
the W5500 0 or 3, the MCP2515 0 or 3. The SD card's fill byte (0xFF) is
per device.

## How each guarantee is tested

`test/host/spibus_share_test.c`:

| Guarantee | Check |
|---|---|
| F1, F2 | part A: a refused device is in line; the releaser cannot reclaim; ticket order with three devices; alternation |
| F1 (interrupt) | part A: `spibus_want_isr` makes the holder contended and blocks its reclaim |
| F4 | part A: an unrenewed want lapses after the TTL; `unwant`; `dev_deinit` |
| F5 | part A: a lone device's release and reclaim is unchanged, with no yields counted. The existing `spibus_test` and `w5500_test` still pass |
| F3 + SD rules | part B: a format-shaped workload (1 MB in 8-block writes, 1 MB in 64-block writes, 200 single-sector FAT writes, 1 MB read back in 64-block reads) with an eth device (3 transactions every 500 µs) and a can device (an interrupt every 0.3–2 ms, then 2 transactions). Run with a card that buffers writes (50 µs busy per block) and one that programs each block (300 µs). Asserted: (a) every grant within 1 ms of its request; (b) every job done during the format; (c) no CS overlap and no MISO fight (`fake_cs_overlaps`, `fake_bus_conflicts`); (d) the card image byte-exact |
| F6 | part B (c), and `sd_card_test` ("no MISO conflict on the shared bus, ever") |

Time in part B is the card simulator's. Every byte advances it at its
device's clock. The bus layer's millisecond clock (the TTL) follows it.

## Gaps found against this spec (2026-10-20) and their status

1. **The releaser could reclaim ahead of a waiter.** `try_claim` was a bare
   semaphore try, so the SD worker's release-and-reclaim within one slice
   always won. Shown on the old `spibus.c`: y is refused, x releases and
   reclaims at once. *Fixed* in `spibus.c` with tickets (F1, F2).
2. **No way to register intent from an interrupt.** *Fixed*:
   `spibus_want_isr`.
3. **A CMD25 multi-block write held the bus for every block** unless the
   card's busy ran past `SD_SPIN_US` (200 µs). A card that buffers writes
   never does. Measured in part B: eth and can waited up to **17.3 ms**
   (mean 3.4 ms), and eth jobs were dropped. *Fix*: release after each
   accepted block when contended (an `sd_spi.c` change).
4. **A CMD18 multi-block read held the bus for every block.** Measured
   **14.3 ms** worst case with a 300 µs-busy card, where writes already
   released. *Fix*: stop with CMD12 and resume when contended
   (`sd_spi.c`).
5. **`wait_ready` released only after spinning 200 µs**, even with a
   waiter. *Fix*: also release when contended (`sd_spi.c`).
6. **`send_cmd` gave way only when the slice was spent.** *Fix*:
   `give_way()` = contended or slice spent (`sd_spi.c`).
7. **Remaining, by design:** the read token wait (N_AC) is held. It is
   typically under 1 ms, but a slow card can approach the SD spec's
   100 ms. It shows up as `busy_max_us`-like latency for the peer. If a
   card proves slow there, the remedy is single-block reads (CMD17) while
   contended, which cap it at one N_AC per block.
8. **Limits:** 6 fairness slots per bus (`SPIBUS_MAX_DEVS`). Tickets are
   main-context only, so a second core using the bus would need the ticket
   update under the semaphore's spin lock. `spibus_claim_blocking` (init
   only) ignores tickets.

With gaps 1–6 fixed, part B measured: the worst wait was **363 µs** (eth)
and **275 µs** (can) with the buffering card, and 279 µs with the
programming card. All jobs completed, with no overlap and no conflicts. The
cost to the SD card under constant contention is 12% (buffering card) and
2.5% (programming card) on this workload. With nobody waiting the cost is
zero.
