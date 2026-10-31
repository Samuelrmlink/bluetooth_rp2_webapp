# bluetooth_rp2_webapp

Gateway for a Raspberry Pi Pico 2 W (`PICO_BOARD=pico2_w`, RP2350). The boot ROM, the firmware image, native PEX modules, the Lua VM, and a browser session are separate layers. A higher layer can be left out of a build. The product image is the RP2350 one. An RP2040 Pico W build of this firmware is supported in principle and is not a validated product image. `pex/test/host` also builds for `rp2040` / `pico`, which is how an `m0p` module is exercised.

## Layers

### Layer 0 — Bootloader

The RP2350 image uses the chip's boot ROM. Flash holds two application slots. A release image is try-before-you-buy: the ROM boots it on a flash-update, and rolls back to the other slot if that image is not confirmed. The ROM rejects an image whose picobin block it will not accept. The same ROM can require a signed firmware image once an OTP key is provisioned. This tree does not record that lock as programmed.

The two slots hold the same image. The linker places it at `0x10000000` (`linker/pico_flash_region.ld`). The boot ROM points the QSPI XIP controller (QMI address translation) at whichever slot it picked, so that slot's bytes execute at `0x10000000`. Slot A is stored at `0x010000` and slot B at `0x0E8000`. Those are storage offsets. They are not the addresses the code runs at.

An RP2040 build uses that chip's second-stage (`boot2`). Its XIP window is a straight map of the flash, with no translation: the byte at flash offset N executes at `0x10000000 + N`. An image linked for slot A has slot A's addresses in it, and the same bytes in slot B would run at the wrong addresses. An RP2040 A/B update is a different image for each slot, each linked for that slot. This tree builds the one RP2350 image. It does not build the pair of RP2040 slot images.

The confirm step is in [OTA.md](OTA.md). The 4 MB map is in [docs/flash-layout.md](docs/flash-layout.md).

### Layer 1 — Firmware

Code linked into the image. Kconfig turns pieces on and off at build time, and a file under `configs/` is one image. `configs/pico2w_sd_defconfig` is the Pico 2 W image with the SD card.

This layer owns:

- the main loop, the heap, and DMA
- Wi-Fi, mDNS, and Ethernet (the W5500 is opt-in; the SD image turns it on)
- Bluetooth Low Energy as an ATT/GATT central
- the HTTP server and the websocket router that carries the ops API
- internal LittleFS, and an SD card formatted FAT32 or exFAT
- generic crypto (mbed TLS, and the codec the device protocols share)
- over-the-air update through MCUmgr SMP, writing the inactive slot

`BT_CLASSIC` is a Kconfig option so the seam stays visible. It defaults off. Classic is not implemented.

SMP is a UDP socket on port **1337**, the MCUmgr UDP port. USB-NCM, Wi-Fi, and WireGuard are three IP paths to that same socket. The groups are `os`, `image`, `stat`, and `fs`. `smpmgr --ip`, `mcumgr --conntype udp`, and mcumgr-client all speak it.

UART is not an SMP transport. USB serial is not one either: `pico_enable_stdio_uart` and `pico_enable_stdio_usb` are off, and logs go out over SWD RTT. `smpmgr --port` names a serial device on the PC. It does not select UDP 1337, and this firmware does not accept SMP on a serial port.

### Layer 2 — PEX modules

Native freestanding C for a device protocol: Anker, Pinecil, a switch, a sensor, another device. A module calls the firmware only through the host API (`pex/host_api.json`, `pex/sdk/include/pex.h`). It does not call the pico-sdk.

A module usually executes in place from the PEX flash partition. A smaller binary can run from the RAM pool instead. That pool is 64 KB on the `pico2w` and `pico2w_sd` images (the Lua arena on those images is 100 KB), so a module the size of the signed Anker app stays in flash. The format is [docs/pex-format.md](docs/pex-format.md).

Stop ends the module's tasks and leaves the code loaded, including any ops it registered. Unload runs deinit and drops those ops. The file in `pex/` and an installed copy in the partition stay until uninstall. The editor's Stop button does both. Lua's `pex.stop` only stops, so a script can pause a module and start it again.

Development images load unsigned PEX modules. `CONFIG_PEX_ALLOW_UNSIGNED` defaults on, including when OTA is on. A signed module still verifies with the OTA key (`pexlink sign`). A release image turns the flag off.

### Layer 3 — Lua VM (optional)

Lua 5.4.8, vendored in `third_party/lua`. The VM compiles a script to bytecode and runs that. `CONFIG_LUA_ENABLE` leaves the VM out of a build. With `CONFIG_PEX_LUA` (the default when Lua and PEX are both on), Lua can load and start PEX modules, and `require` finds a `<name>.<arch>.pex` before a Lua source.

This is the place for high-level logic that changes often and keeps running with no client attached: a reaction to a GPIO input, a timer, a decision on a message from an outside node. Crypto, frames, and the device wire protocols stay in the firmware and in PEX modules. `lua/pinecil.lua` is a launcher. The scanner starts the Pinecil module from `pex/pinecil.m33.pex`. The Anker module starts from `pex.load` and `pex.start` with no Lua file.

A manual unload of a module Lua is holding calls the global `pex.on_unload(name)` while the module is still callable, drops Lua's reference, then unloads. A later call raises `PEX module '<name>' was unloaded`. The log lines are in [docs/Development.md](docs/Development.md#unload-while-lua-holds-a-pex-module).

### Layer 4 — External session

The HTTP server and the websocket router are Layer 1. A browser, or any other client, is a session on that server. While the session is connected it can read and write configuration and drive the firmware, a PEX module, and Lua. When it disconnects, that live control stops.

The session can leave the board able to run on its own: an OTA image, a PEX module in the partition, or a Lua script. After that, Layers 1–3 keep going with no client.

The pages live in `web/`. Uploading them changes the UI on the board. It does not update the firmware.

## Addresses

| What | Where |
|---|---|
| HTTP, including the pages | TCP port 80 |
| Ops websocket | `ws://<addr>/ws` on that same port |
| One-shot ops | `POST /api/op` on port 80 |
| SMP (`smpmgr`) | UDP port 1337 on the same IP |
| USB-NCM | `192.168.8.1` |
| Wi-Fi | the address DHCP gave the board |
| WireGuard | the tunnel address. The bench uses `10.255.255.23` |
| mDNS | `<CONFIG_NET_HOSTNAME>.local`. The SD image is `bluetooth-rp2-webapp.local` |

## Building

The commands are the short form. Signing, the confirm step, and what each artifact is allowed to do are in [docs/Development.md](docs/Development.md).

### Firmware

```sh
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.0
export PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1
cmake -S . -B build-ota-sd -G Ninja -DAPP_DEFCONFIG=configs/pico2w_sd_defconfig
cmake --build build-ota-sd -j
python3 tools/ota_release.py build-ota-sd/btgw.bin
```

`ota_release.py` writes `build-ota-sd/btgw-<version>.smp.bin`: the image followed by a 64-byte ECDSA P-256 signature. The version minor is the git commit count unless `-DBTGW_VERSION_MINOR` is set. Apply the file with `smpmgr --ip <addr>` over UDP 1337. A try-before-you-buy image stays unconfirmed until `image state-write --confirm`. The sequence is in [OTA.md](OTA.md).

The first image a blank board boots normally is built with `-DBTGW_TBYB=OFF` and written by `tools/provision.sh` or `tools/make_provision_uf2.sh`. Release images leave `BTGW_TBYB` at its default, ON.

The signing key stays outside this repository (`~/.config/btgw/ota-signing.pem`, or `$BTGW_OTA_KEY`). Development images, including `configs/pico2w_sd_defconfig`, load unsigned PEX modules. The editor Build button writes an unsigned `.pex` and cannot sign it. That file loads on these images. Turn `CONFIG_PEX_ALLOW_UNSIGNED` off for a release image that should refuse it. The host sign command is in [docs/Development.md](docs/Development.md).

### Web assets

```sh
python3 tools/make_release.py
python3 tools/upload_fw_assets.py --base http://<addr>
```

`make_release.py` copies `web/` into `web-dist/` and gzips each file (`mtime=0`, level 9). The uploader PUTs only the `.gz` files to LittleFS over `PUT /api/fs/<path>`. The HTTP server prefers `<name>.gz`. No firmware reflash. Reload the page after the upload. Pass the board's address: the script's default base is `http://192.168.8.1`. `--sync` also deletes `.gz` files on the device that are not in `web-dist/`.

### PEX module

The drivers in this tree, built on the host. The `.pex` this produces is unsigned:

```sh
cmake -S pex/modules/drivers -B /tmp/pex-drivers -G Ninja
cmake --build /tmp/pex-drivers
python3 pex/tools/pexlink.py sign \
  /tmp/pex-drivers/pex-drivers/anker.m33.pex -o /tmp/anker.m33.pex
```

The signing key is `~/.config/btgw/ota-signing.pem`, or `$BTGW_OTA_KEY`. A firmware build with `CONFIG_PEX` also drops unsigned copies in `<build>/pex-drivers/`. A new module is a `pex_add_module()` call, described at the top of [pex/sdk/cmake/pex.cmake](pex/sdk/cmake/pex.cmake).

The editor can compile a `Pexfile` on the SD card with the `pexcc` toolchain stored on the card. That output is unsigned. An OTA image refuses it until `pexlink sign` has been run. `pexcc` emits Cortex-M33 only. An `m0p` module is built with the host cmake and `TARGETS m0p`.

The signed file is uploaded to LittleFS as `pex/<name>.<arch>.pex`, then `pex.install`, `pex.load`, and `pex.start`. Stop is `pex.stop` then `pex.unload`. Uninstall removes the partition copy. The file in `pex/` stays until it is deleted.

### Lua script or module

There is no host compile step. The source is the file. The board compiles it to bytecode.

```sh
curl -T lua/worker.lua http://<addr>/api/fs/lua/worker.lua
curl -X POST http://<addr>/api/op \
  -d '{"op":"script.start","params":{"name":"worker"}}'
```

`script.start` accepts `name` or `script`. A file that other scripts `require`, under `lua/`, is compiled once and kept as `cache/<path>.luac`. Top-level `lua/*.lua` scripts are not cached. The editor's Run button sends the open buffer as a RAM draft and does not write the file. Save writes it. The source order at run time is the RAM draft, then `staged/<name>.lua`, then `lua/<name>.lua`.

## Reading next

- [docs/Feature-Roadmap.md](docs/Feature-Roadmap.md) — what changes a layer, in priority order
- [docs/Hardware-Testing.md](docs/Hardware-Testing.md) — checks that wait on a device on the bench
- [docs/Development.md](docs/Development.md) — the build, the upload, the Lua unload contract, and how a feature branch reaches `dev`
- [OTA.md](OTA.md) — slots, try-before-you-buy, confirm
- [docs/flash-layout.md](docs/flash-layout.md) — the flash map and the LittleFS migration
- [docs/memory-optimizations.md](docs/memory-optimizations.md) — measured flash and SRAM, and why the SD Classic image leaves HTTPS off
- [docs/pex-format.md](docs/pex-format.md) — the module format and the runtime
- [docs/network.md](docs/network.md), [docs/sd.md](docs/sd.md), [docs/lua-net-fs.md](docs/lua-net-fs.md)
