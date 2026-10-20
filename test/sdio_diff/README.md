# SDIO differential test

Proves the gateway's own SDIO driver (`src/sd/sd_sdio.c`, `sdio_hw_pico.c`,
`sdio.pio`) behaves like carlk3's proven one
([no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)),
in mcusim, on the same simulated card.

* `sddiff.c`: one seeded script, one JSON line per step over RTT. Two
  scenarios: `raw` (identification, reads at the edges, 1/2/8/64-block
  writes and read-backs, then faults the card injects at fixed blocks) and
  `fs` (FatFs R0.15 on top: mkfs, files, rename, delete, listing).
* `be_ours.c` links the gateway's driver sources unchanged; `be_carlk3.c`
  links carlk3's library at a pinned commit, **fetched into the build
  directory only**. Its SDIO core is ZuluSCSI's GPL-3.0 code, so it is never
  committed here and never linked into the gateway; these ELFs are test
  instruments only.
* `tools/sdio_diff.py` runs all four ELFs in mcusim and compares them:
  result classes, read hashes, identification, the card's final image (raw)
  or file hashes plus fsck (fs), and the card's normalised command trace
  (accepted differences in `allowlist.txt`, each with a reason).

```sh
PICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 cmake -S test/sdio_diff -B build-sdio-diff -G Ninja
cmake --build build-sdio-diff
tools/sdio_diff.py            # exit 0 same, 1 different, 77 skipped
```

Status: the four ELFs build and run in mcusim (RTT output checked with no
card fitted). The comparison waits for mcusim's `sdcard` device. What
`tools/sdio_diff.py` asks of that device is its `CARD_TOML`: SDIO pins,
card detect, an image file written back, a JSONL command trace, and
one-shot faults (`crc_read`, `crc_write`, `write_error`, `no_response`,
`busy`, `pull` with reinsertion).

Already visible with no card fitted: ours answers `nocard` on every step (it
reads card detect first); carlk3's sends commands to the empty slot and
reports `timeout` / `io`. That difference is expected; the comparison only
means something with the card model in place.
