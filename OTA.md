# Over-the-air firmware updates

This firmware updates itself over the network with the **MCUmgr / SMP**
protocol, so the tooling is not ours: `smpmgr`, `mcumgr-client` and nRF
Connect Device Manager all drive it unchanged. Underneath there is **no
custom bootloader** — the RP2350 boot ROM does the slot selection. That
shapes everything below, so it is worth thirty seconds on the model before
the commands.

## The model

Flash holds **two application slots, A and B** (`partitions.json`). On the
RP2350 they hold **one compiled binary**. The linker places that image at
`0x10000000` (`linker/pico_flash_region.ld`). The boot ROM programs the QSPI
XIP controller — QMI address translation — so whichever slot it picked
appears at `0x10000000` and runs there. Slot A is stored at `0x010000` and
slot B at `0x0E8000`. Code in the running image sees its own slot at
`0x10000000`, so the firmware keeps the other slot's location as a storage
offset and reads it through the NOTRANSLATE alias (`src/ota/ota_slot.h`).

An RP2040 has no such translation. XIP is a straight map: flash offset N
executes at `0x10000000 + N`. Slot A and slot B are different addresses, so
each slot needs an image linked for that slot. Copying the slot A image
into slot B runs it at the wrong addresses. This tree's partition table is
the RP2350 family (`rp2350-arm-s`) and the linker origin is the one shared
window. It does not produce the two RP2040 slot images.

The RP2350 ROM picks the slot whose image carries the **higher version number**
(`pico_set_binary_version`), so **every update must have a larger version
than the one it replaces** or the ROM keeps running the old one.

An update is written into the slot that is **not** running, then the device
reboots into it with a *flash-update boot*. New images are built
**try-before-you-buy (TBYB)**: such an image boots *only* via that
flash-update path, and the ROM's own ~16.7 s watchdog reverts to the old
slot unless the new firmware **buys** itself after proving it runs. So a bad
update cannot brick the device — worst case it boots the new image once,
fails to buy, and the ROM falls back.

LittleFS (`cfg/`, `lua/`, web assets) is a third partition **shared by both
slots and never touched by an update**. The Wi-Fi firmware blob is its own
partition, flashed once and never OTA'd. After LittleFS comes the PEX
partition (256 KB, reserved for native modules) and BTstack's NVM; the whole
map, and the one-time migration that made room for PEX, are in
[docs/flash-layout.md](docs/flash-layout.md).

Three things must line up, and they are all checked by
`tools/run_tests.py`: `partitions.json`, `src/fs/fs.h`, and
`linker/pico_flash_region.ld`.

---

## One-time setup

### 1. The signing key (once per developer machine)

Updates are refused unless they carry a valid **ECDSA P-256 signature over
the image's SHA-256**, checked against a public key baked into the firmware.

```sh
tools/ota_keygen.py
```

This writes the **private** key to `~/.config/btgw/ota-signing.pem` (mode
0600, **outside the repository**, never uploaded to the device) and the
**public** key to `src/ota/ota_pubkey.h`, which *is* committed. Override the
private-key path with `BTGW_OTA_KEY=/path/to/key`.

Losing the private key means you can no longer sign updates that this
firmware will accept; recover by re-flashing over SWD (below), which
bypasses OTA entirely. Back it up like any credential. To rotate it, see the
header comment in `tools/ota_keygen.py`.

### 2. Provision the flash layout (once per board)

A fresh board has no partition table. Provisioning writes three regions —
the partition table, the firmware into slot A, and the Wi-Fi blob — and
leaves LittleFS alone. The firmware must be a **non-TBYB** build, because the
normal boot that follows provisioning won't run a try-before-you-buy image:

```sh
cmake -B build -DBTGW_TBYB=OFF          # (defconfig already sets CONFIG_OTA=y)
cmake --build build -j8
```

**Over SWD** (with a debug probe):

```sh
tools/provision.sh                       # writes PT + slot A + Wi-Fi blob, resets
```

**Over USB, no probe** — build a single drag-and-drop image:

```sh
tools/make_provision_uf2.sh              # -> build/btgw-provision.uf2
```

Hold BOOTSEL while plugging the board in, then copy `btgw-provision.uf2`
onto the `RP2350` drive (or `picotool load build/btgw-provision.uf2`). Its
blocks are absolute-addressed, so they write the same bytes to the same
offsets as `provision.sh` — the partition table at `0x10000000`, the
firmware at `0x10010000` (slot A) and the blob at `0x101C0000` — and the
board reboots into the new firmware. LittleFS (`0x200000`+) is untouched, so
a reprovision keeps `cfg/`, `lua/` and the web assets. (A board provisioned
before the PEX partition gets the new table this way; its LittleFS is then
migrated by the firmware on the next boot, through the SD card:
[docs/flash-layout.md](docs/flash-layout.md).)

Confirm it came up:

```sh
python3 tools/wsapi.py ota.status
# {"slot":0,"active":"A","version":"1.NN.0", ... }
```

---

## Building an update

The version is `1.<minor>`, where **minor defaults to the git commit
count**, so every commit already bumps it. Build a **TBYB** image (the
default) and package it:

```sh
cmake -B build -DBTGW_TBYB=ON            # ON is the default; shown for clarity
cmake --build build -j8
tools/ota_release.py
# build/btgw-1.NN.smp.bin: <image bytes> + 64 signature bytes; image sha256 ...
```

`ota_release.py` calls `tools/ota_sign.py`, which signs `build/btgw.bin`
with the private key and appends the 64-byte signature. The result,
`build/btgw-<ver>.smp.bin`, is the file you upload — image followed by its
signature, nothing else.

To force a specific version (e.g. testing two updates without committing):

```sh
cmake -B build -DBTGW_TBYB=ON -DBTGW_VERSION_MINOR=99
```

---

## Applying an update

`smpmgr` is the reference client:

```sh
pipx install smpmgr        # or: pip install --user smpmgr
```

The same commands work over every IP path. Only the address changes. USB-NCM, Wi-Fi, and WireGuard all reach the SMP server on **UDP port 1337**:

| Transport | Address |
|---|---|
| USB-NCM (wired) | `192.168.8.1` |
| Local Wi-Fi (LAN) | the device's DHCP address, e.g. `192.168.1.162` |
| WireGuard (remote) | `10.255.255.23` |

UART is not an SMP transport, and neither is USB serial. `smpmgr --port` names a serial device on the PC. It does not select UDP 1337, and this firmware does not accept SMP there.

The flow, step by step (this is the sequence verified on the bench):

```sh
ADDR=192.168.8.1

# 1. Upload into the inactive slot. --format any is REQUIRED: the image is
#    not an MCUboot image, so smpmgr's default magic check would reject it.
smpmgr --ip $ADDR --mtu 1200 --timeout 10 image upload --format any \
    build/btgw-1.NN.smp.bin

# 2. Read the slots and copy the INACTIVE slot's hash (active:false).
smpmgr --ip $ADDR image state-read

# 3. Arm the new image for a test boot on the next reset.
smpmgr --ip $ADDR image state-write <hash-from-step-2>

# 4. Reboot into it (a flash-update boot).
smpmgr --ip $ADDR os reset

# 5. Verify the new image is up and REACHABLE, then commit it:
smpmgr --ip $ADDR image state-write --confirm
```

The new image boots **try-before-you-buy and does NOT commit itself.** It runs
"buy pending" indefinitely (it owns and feeds the watchdog), waiting for the
explicit `--confirm` in step 5. This is deliberate: the commit (a brief
interrupts-off flash write) is kept well clear of the boot-time radio storm,
and — more importantly — an image is only made permanent once you have
confirmed it is actually reachable. **Until you send step 5, any reboot
(a hang, a power-cycle, or `os reset`) reverts to the previous slot**, which
stays intact until the buy. So verify connectivity first (`ota.status`,
`wg.status`, a ping), *then* confirm. After step 5, `ota.status` shows the new
slot `bought:true` and `image state-read` shows it `confirmed`.

If the update broke networking, you simply never send step 5: power-cycle and
the device comes back on the old firmware. Recovering a wedged board still
needs a power-cycle, but you never lose the known-good slot.

Notes on the hash in step 3: use the hash `state-read` prints for the slot
you just uploaded (it is the SHA-256 of the whole release file). The device
accepts either that file hash or the image's own digest, so a client that
hashed `btgw.bin` alone works too. The signature is verified during
**upload**, when the last sector is written — an image with a bad or missing
signature never becomes bootable and step 3 refuses it with
`HASH_NOT_FOUND`.

`smpmgr … upgrade --format any <file>` bundles the upload + test + reset, but
it does NOT replace step 5: this firmware never auto-buys, so you still send
`image state-write --confirm` yourself once you've verified the new image is
reachable. The explicit sequence above is the one to use.

---

## Updating a board from before the PEX partition

Firmware from the PEX layout on can shrink LittleFS from 2032 KB to 1776 KB, by
copying it through the SD card and back ([docs/flash-layout.md](docs/flash-layout.md)).
That mover is `CONFIG_FS_MIGRATE`, off unless the image was built with it.
Over OTA, on an image that has it:

* the new image **does not migrate while it is unbought** (`sys.info`:
  `"migration":"migration pending: waiting for the OTA confirm ..."`): an
  unbought image reverts on any reboot, and the old firmware would format a
  migrated filesystem;
* after step 5 (`--confirm`) it **reboots by itself** 3 s later and migrates
  (needs the SD card; without one it stays on the old geometry, working, and
  says *pending: no SD card*);
* from then on **never roll back** to a pre-PEX image (see below): it would
  format LittleFS. The copy on the card (`sd:/.lfs-migrate/<n>/`) stays as a
  backup.

## Rolling back

Not to a firmware older than the PEX layout once a board has migrated
(`cfg/migrated.json` exists): such a firmware formats the smaller LittleFS.

If a freshly-bought update misbehaves, the previous version is still intact
in the other slot. Upload nothing — just arm and reboot into it:

```sh
smpmgr --ip $ADDR image state-read                       # find the old slot's hash
smpmgr --ip $ADDR image state-write <old-slot-hash>
smpmgr --ip $ADDR os reset
```

The absolute fallback, for a board you can reach physically, is SWD
(`tools/provision.sh` or `tools/debug.sh -c "program build/btgw.bin
0x10010000 verify reset"`) or BOOTSEL (hold the button, drag `build/btgw.uf2`
onto the `RP2350` drive). Both bypass OTA verification by design — that is
the person holding the board.

---

## Also on the same port (UDP 1337)

The SMP server carries three more groups, useful during and after an update:

- **`smpmgr … os echo hi`** — a liveness check that needs no application code.
- **`smpmgr … statistics list` / `statistics get <group>`** — `sys`, `radio`,
  `lwip_ip/udp/tcp/mem`, `lua`, `bt`, `ncm` counters as plain integers (the
  same numbers `sys.info` and `lua.stats` report).
- **`smpmgr … file download /lfs/<path> <local>` / `file upload`** — read and
  write LittleFS (`cfg/`, `lua/`, staged uploads) over the same protocol.
  Writes go one chunk at a time and are paced by the flash, not the network.

---

## WireGuard and reboots

The device has no clock, so it fabricates WireGuard's handshake timestamp
from a persisted boot counter (see `src/net/wg.c`). This keeps the tunnel
reconnecting immediately after a reboot — including the reboot an OTA
triggers — instead of being locked out for the length of the previous
uptime. The only assumption is that a single boot never runs longer than
~2.1 years continuously; if one somehow did, the recovery is to restart
WireGuard on the server (`wg-quick down/up`, or reboot the VPS), which clears
the peer timestamp it remembers. No device access needed.

## Two things to expect in practice

**A stalled upload is safe, and resumes.** Over a marginal link (a phone
hotspot ran at ~22 kB/s against ~115 kB/s on USB-NCM) the transfer sometimes
stops part-way and `image state-read` then shows the inactive slot still empty
(`0.0.0`). The running image is untouched -- nothing is armed until step 3 --
and the SMP image upload is offset-based, so simply **running the same upload
command again resumes it**. Bench runs completed on the second attempt every
time. Scripted deployments should retry the upload a few times before giving
up rather than treating the first stall as a failure.

**Confirm over LAN or WireGuard, not USB-NCM.** See below.

## Known limitation

The boot-health **buy** runs a flash operation that briefly stops servicing
USB, and on the bench that has been enough to drop the **USB-NCM** link right
after an update boot — the device keeps running, but you may lose the
`192.168.8.1` route until it re-enumerates. Prefer the **LAN or WireGuard**
address for confirming an update, or re-check with `ota.status` once the link
is back. Pacing USB across the buy is tracked work; it does not affect the
revert guarantee (an image that never buys is dropped by the ROM on its next
boot).
