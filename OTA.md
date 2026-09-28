# Over-the-air firmware updates

This firmware updates itself over the network with the **MCUmgr / SMP**
protocol, so the tooling is not ours: `smpmgr`, `mcumgr-client` and nRF
Connect Device Manager all drive it unchanged. Underneath there is **no
custom bootloader** — the RP2350 boot ROM does the slot selection. That
shapes everything below, so it is worth thirty seconds on the model before
the commands.

## The model

Flash holds **two application slots, A and B** (`partitions.json`). One
compiled binary boots from either, because the ROM sets up QMI address
translation so whichever slot it picks appears at `0x10000000`. The ROM
picks the slot whose image carries the **higher version number**
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
partition, flashed once and never OTA'd.

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

### 2. Provision the flash layout (once per board, over SWD)

A fresh board has no partition table. Build a **non-TBYB** image (so a
normal boot runs it — TBYB images only boot via a flash-update) and load the
table, slot A and the Wi-Fi blob:

```sh
cmake -B build -DBTGW_TBYB=OFF          # (defconfig already sets CONFIG_OTA=y)
cmake --build build -j8
tools/provision.sh                       # over the SWD probe; see tools/debug.sh
```

`provision.sh` erases the first 64 KB, writes `pt.bin`, `btgw.bin` (→ slot A
at `0x10010000`) and `btgw_wifi_firmware.bin` (→ `0x101C0000`), then resets.
LittleFS is left alone. Confirm it came up:

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

The **same commands work over every transport** — only the address changes:

| Transport | Address |
|---|---|
| USB-NCM (wired) | `192.168.8.1` |
| Local Wi-Fi (LAN) | the device's DHCP address, e.g. `192.168.1.162` |
| WireGuard (remote) | `10.255.255.23` |

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
```

The device brings the new image up and, once it has started cleanly,
**buys itself automatically** (a boot-health gate a few hundred ms into the
loop). So after step 4, `ota.status` should show the new slot `active` with
`bought:true`, and `image state-read` show it `confirmed`. A belt-and-braces
explicit confirm is also honoured and is a no-op once bought:

```sh
smpmgr --ip $ADDR image state-write --confirm
```

Notes on the hash in step 3: use the hash `state-read` prints for the slot
you just uploaded (it is the SHA-256 of the whole release file). The device
accepts either that file hash or the image's own digest, so a client that
hashed `btgw.bin` alone works too. The signature is verified during
**upload**, when the last sector is written — an image with a bad or missing
signature never becomes bootable and step 3 refuses it with
`HASH_NOT_FOUND`.

`smpmgr … upgrade --format any <file>` bundles steps 1–4 into one command;
the explicit sequence is documented here because it makes each stage
(and any failure) visible, which is what you want the first few times.

---

## Rolling back

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

## Known limitation

The boot-health **buy** runs a flash operation that briefly stops servicing
USB, and on the bench that has been enough to drop the **USB-NCM** link right
after an update boot — the device keeps running, but you may lose the
`192.168.8.1` route until it re-enumerates. Prefer the **LAN or WireGuard**
address for confirming an update, or re-check with `ota.status` once the link
is back. Pacing USB across the buy is tracked work; it does not affect the
revert guarantee (an image that never buys is dropped by the ROM on its next
boot).
