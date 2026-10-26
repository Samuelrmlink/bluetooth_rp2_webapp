# Development

How to build each layer, put it on a board, and what the firmware will accept. The layer map and the short commands are in [../README.md](../README.md). Slot selection, try-before-you-buy, and the confirm step are in [../OTA.md](../OTA.md).

The product image is `configs/pico2w_sd_defconfig` (`PICO_BOARD=pico2_w`, RP2350, SD card). Commands below use `build-ota-sd` as the firmware build directory and `<addr>` as the board. On USB-NCM that address is `192.168.8.1`. On the bench WireGuard tunnel it is `10.255.255.23`.

## Git

`dev` is the branch that collects finished work. Day-to-day work is committed on a `feature/<topic>` branch cut from current `dev`, and that branch is pushed to `origin` as the commits land.

You confirm a feature branch before it is merged into `dev`. After that merge is on `origin/dev`, the feature branch is deleted locally and on `origin`. A branch that has not been merged stays.

Commits do not land on `dev` directly, and `dev` is not force-pushed.

## Firmware image

```sh
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.0
export PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1
cmake -S . -B build-ota-sd -G Ninja -DAPP_DEFCONFIG=configs/pico2w_sd_defconfig
cmake --build build-ota-sd -j
python3 tools/ota_release.py build-ota-sd/btgw.bin
```

`tools/genconfig.py` runs at configure time. A new build directory is seeded from `APP_DEFCONFIG`. After that, cmake reads `build-ota-sd/.config` and ignores a later change to `-DAPP_DEFCONFIG`. `.config` is not committed. To start from a different defconfig, use a new build directory.

`BTGW_VERSION_MINOR` defaults to `git rev-list --count HEAD`. Override it with `-DBTGW_VERSION_MINOR=<n>` at configure time. The ROM boots the slot with the higher version, so an update has to be newer than the image it replaces.

`BTGW_TBYB` defaults ON. That image boots only on a flash-update and rolls back unless it is confirmed. The first image written to a blank board is built with `-DBTGW_TBYB=OFF` and provisioned with `tools/provision.sh` (SWD) or `tools/make_provision_uf2.sh` (BOOTSEL). Both are in [OTA.md](../OTA.md).

The signing key is created once per machine with `tools/ota_keygen.py`. The private key stays outside this repository, at `~/.config/btgw/ota-signing.pem` (or `$BTGW_OTA_KEY`). The public key is `src/ota/ota_pubkey.h`. `ota_release.py` appends the 64-byte ECDSA P-256 signature to `btgw.bin` and writes `btgw-<version>.smp.bin` next to it. The same key signs a PEX module (`pex/tools/pexlink.py sign`, or `link --sign`). The editor Build button has no key: it writes an unsigned `.pex` on the SD card.

`CONFIG_PEX_ALLOW_UNSIGNED` defaults on. Development images, including `configs/pico2w_sd_defconfig`, load unsigned modules and say so in `pex.list` as `UNSIGNED ALLOWED (development)`. A signed module still verifies. A release image sets the symbol off and then reports `signed-only`. An existing build directory keeps its `.config`, so a directory configured before this default needs the symbol set there, or a fresh directory.

`configs/pico2w_pexcc_defconfig` is the same SD image, used when the browser compiler is under test. Use a fresh directory. After the first configure, cmake keeps that directory's `.config` and ignores a later `-DAPP_DEFCONFIG`.

```sh
cmake -S . -B build-pexcc -G Ninja -DAPP_DEFCONFIG=configs/pico2w_pexcc_defconfig
cmake --build build-pexcc -j
python3 tools/ota_release.py build-pexcc/btgw.bin
```

Shipped defconfigs:

| File | What it turns on |
|---|---|
| `pico2w_defconfig` | Wi-Fi, USB-NCM, mDNS, BLE, Lua, OTA. No Ethernet, no SD |
| `pico2w_eth_defconfig` | `pico2w` plus the W5500 |
| `pico2w_sd_defconfig` | `pico2w_eth` plus an SD card on SDIO. The image used with the card |
| `pico2w_sdspi_defconfig` | The same card on SPI |
| `pico2w_pexcc_defconfig` | The SD image, same unsigned-module default, used with the browser compiler |
| `pico2w_minimal_defconfig` | BLE over USB-NCM. No Wi-Fi, no Lua, no OTA |
| `pico2w_noanker_defconfig` | Wi-Fi and Lua, with the Anker codec left out. OTA is off, so unsigned PEX modules load |

`stdio` over UART and over USB are both off. `pico_enable_stdio_rtt` is on, so logs come out of the SWD probe.

## SMP (`smpmgr`)

The server is `src/ota/smp_udp.c`. It binds **UDP port 1337** on every interface lwIP has up: USB-NCM, Wi-Fi, Ethernet when that build has it, and WireGuard. One request is handled at a time. A second datagram arriving while the first is still in progress is dropped, and the client retries.

`smpmgr --port` is a serial port. This firmware has no serial SMP server. Use `--ip`.

```sh
pipx install smpmgr
smpmgr --ip <addr> --mtu 1200 --timeout 10 image upload --format any \
  build-ota-sd/btgw-1.NN.smp.bin
smpmgr --ip <addr> image state-read
smpmgr --ip <addr> image state-write <inactive-slot-hash>
smpmgr --ip <addr> os reset
smpmgr --ip <addr> image state-write --confirm
```

`--format any` is required. The image is not an MCUboot image, and smpmgr's default magic check rejects it. The hash in `state-write` is a positional argument. `--confirm` takes no hash and commits the image that is running. Until that command, a reboot returns to the previous slot.

`smpmgr` sends each UDP request once. On a lossy tunnel, `tools/smp_upload.py --ip <addr> <file>` resends. The offset in the upload makes a retry resume. Then the same `state-read`, `state-write`, `os reset`, and `--confirm`.

Other groups on the same port:

- `smpmgr --ip <addr> os echo hi`
- `smpmgr --ip <addr> statistics list` and `statistics get <group>`
- `smpmgr --ip <addr> file download /lfs/<path> <local>` and `file upload`

Confirm over the LAN or the WireGuard address when you can. The buy writes flash with interrupts off, and on the bench that has dropped the USB-NCM link. The image keeps running. [OTA.md](../OTA.md) has the stall, resume, and rollback notes.

## Web assets

Static files are LittleFS objects served by `src/net/http.c`. `resolve_static()` prefers `<name>.gz` and answers with `Content-Encoding: gzip`. A browser decompresses that. A client that does not, such as Python `urllib`, receives the gzip bytes (`1f 8b`).

```sh
python3 tools/make_release.py
python3 tools/upload_fw_assets.py --base http://<addr>
python3 tools/upload_fw_assets.py --base http://<addr> --list
```

`make_release.py` copies `web/` into `web-dist/` and gzips every shipped file. `--minify` also bundles the JS with esbuild from `web/node_modules/`. The plain copy is what the board runs: the pages are ES modules.

`upload_fw_assets.py` uploads `*.gz` only. A path segment is at most 64 characters. A file is at most 1 MiB. `--sync` deletes remote `.gz` files that are not in `web-dist/`. Leave it off unless that deletion is what you want. The upload does not reflash firmware. Reload the page.

The editor against a mock, with no board:

```sh
cd web && python3 dev/devserver.py --port 8080
```

Playwright starts that server on port 8095. Its config is `web/playwright.config.js`.

## PEX modules

A module is freestanding C. The compile does not use the pico-sdk. The host API header is generated from `pex/host_api.json`. Two compilers show up:

- Host `arm-none-eabi-gcc` from `PICO_TOOLCHAIN_PATH` (or `~/.pico-sdk/toolchain/15_2_Rel1`), via `pex_add_module()`. `TARGETS m33` and `TARGETS m0p` are both real. Model `reloc` is the one the drivers use.
- The in-browser compiler `pexcc`, fetched from the SD card by the editor. It emits Cortex-M33 only. An `m0p` target is refused before compile.

The drivers in this repository:

```sh
export PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1
cmake -S pex/modules/drivers -B /tmp/pex-drivers -G Ninja
cmake --build /tmp/pex-drivers
```

Outputs land in `/tmp/pex-drivers/pex-drivers/` (`anker.m33.pex`, `pinecil.m33.pex`). A firmware configure with `CONFIG_PEX` builds the same unsigned files into `<build>/pex-drivers/`. `drivers.cmake` does not pass `SIGN`.

A development image loads that unsigned file. Sign with the OTA key when the image has `CONFIG_PEX_ALLOW_UNSIGNED` off:

```sh
python3 pex/tools/pexlink.py sign \
  /tmp/pex-drivers/pex-drivers/anker.m33.pex -o /tmp/anker.m33.pex
python3 pex/tools/pexlink.py dump /tmp/anker.m33.pex
```

`--key` selects a PEM. Otherwise the tool uses `$BTGW_OTA_KEY`, then `~/.config/btgw/ota-signing.pem`. `link --sign` signs while linking. Development images already load the unsigned file. The configure lines are under Firmware image above.

On the device the file lives at `pex/<name>.<arch>.pex` on LittleFS. `pex.install` copies it into the PEX partition. `pex.load` then `pex.start` run it. `pex.load` of an `sd:` path reads the whole file into the RAM pool and does not install it. The pool on the SD image is 48 KB (`CONFIG_PEX_POOL_KB`). A file larger than `pool.largest` from `pex.list` cannot take that path. The signed Anker module is one of those: it executes in place from the partition.

`pex.stop` ends tasks and leaves ops registered. `pex.unload` runs deinit and drops the ops. The editor Stop button sends stop, then unload. Uninstall removes the partition copy. Deleting `pex/<name>.<arch>.pex` is a separate step.

A project on the card is a directory with a `Pexfile`. The editor's Build button walks from the open file to the first `Pexfile`. The artifact is written under that target's `out` directory on the SD card (usually `build/<name>.m33.pex`), and the log next to it. Build and run from flash copies the artifact to LittleFS, installs it, and starts it when `xip_ready` is true. Build and run from RAM loads the SD file into the pool when it fits. The page cannot sign. The key stays on the host.

## Lua scripts and modules

Lua 5.4.8 is linked into the firmware when `CONFIG_LUA_ENABLE` is set. The device is the compiler. A `.lua` file uploaded as text is the source. The VM compiles it to bytecode on the board.

Put a script at `lua/<name>.lua` and start it:

```sh
curl -T lua/worker.lua http://<addr>/api/fs/lua/worker.lua
curl -X POST http://<addr>/api/op \
  -d '{"op":"script.start","params":{"name":"worker"}}'
```

`name` and `script` are the same parameter (`src/lua/lops.c`). The editor Run button (F5) sends the buffer as a RAM draft and does not write flash. Save writes the open path. At start, the runtime picks the RAM draft if one exists, otherwise `staged/<name>.lua`, otherwise `lua/<name>.lua`.

A file under `lua/` that another script `require`s is a module. It is compiled once, when the source changes, and stored as stripped bytecode in `cache/<path>.luac`. The `.lua` stays the source. Top-level `lua/*.lua` files are scripts and are not cached. A staged file is compiled from source on each load so line numbers survive. `require` with `CONFIG_PEX_LUA` looks for `pex/<name>.<arch>.pex` before a Lua source.

`cfg/autostart.json` lists script names to start after boot. The shape and the ops are in [lua-net-fs.md](lua-net-fs.md).

## Unload while Lua holds a PEX module

`require` of a PEX module holds one runtime reference for as long as one of its functions is reachable. `pex.unload` from the websocket, and `pex.unload` from Lua, both ask Lua to release that reference first (`pexlua_before_unload` in `pex/lua/pexlua.c`):

1. While the module is still loaded, call the global `pex.on_unload(name)`. The script can still call the module.
2. Drop the anchor and the `require` cache entry. A handler that calls `pex.unload` is folded into this unload.
3. Run `pex_unload`.

The device log says what Lua did:

| Log line | When |
|---|---|
| `pexlua: no pex.on_unload handler for <name>` | Lua held the module and never set a handler. Unload still proceeds |
| `pexlua: pex.on_unload(<name>): …` | The handler raised. Unload still proceeds |
| `pexlua: Lua used <name> after it was unloaded` | A later call. Printed once per anchor. The call raises `PEX module '<name>' was unloaded` |
| `pexlua: Lua was detached from <name> but N reference(s) remain(s)` | Something besides the Lua anchor still holds the module. Later Lua calls still error |
| `pexlua: Lua was detached from <name> but unload failed: …` | The anchor was dropped and `pex_unload` still refused |

A module with tasks still running, or one sitting on a call stack, is left attached. Unload stays refused until it is idle, which is why Stop comes first. Dropping the last Lua reference without an explicit unload still stop-and-unloads a module that Lua owns, from the anchor's garbage collection, and that path does not call `pex.on_unload`.

`pex/test/unit/pex_lua_unload.c` is the host test, wired into `tools/run_tests.py` (tier 0). `pex/test/remote/pex_lua_test.lua` is the on-device script. It expects the hook above, so it fails on a firmware image built before that hook.

## Checks

```sh
python3 tools/run_tests.py
cd web && node --test
cd web && npx playwright test
```

`run_tests.py` includes the flash-layout agreement check and the PEX host tests. Playwright's config ignores `pexbuild.sim.spec.js`, which needs mcusim.
