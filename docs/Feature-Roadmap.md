# Feature roadmap

The target is the five layers in [../README.md](../README.md). An item on this list changes one of those layers, or removes a second way of doing the same job. A bugfix that leaves the layers alone does not belong here.

The board that is running today is firmware 1.239. Several items below are already written in the tree and are not on that image. A checkbox is ticked when the behavior is true of the image and the pages the board is actually serving, or when a decision is written down and the code matches it. A check that needs a device on the bench is [Hardware-Testing.md](Hardware-Testing.md), not a box on this list.

## How to change this list

Add an item by copying the template at the bottom into Now, Next, Later, or Parked. Give it the next `R` number. Fill in why it belongs to a layer, what has to exist before it can ship, and the condition that ticks the box.

Move an item between headings to change its priority. Tick the box when the done condition is met, then move the whole entry under Done and add the date. Leave the entry there so the reason is still readable.

The work for an item is committed on a feature branch. How those branches reach `dev` is in [Development.md](Development.md).

Parked means we are not doing it. Out of scope means it was considered and rejected.

## Recorded

These are part of the architecture. They are not tasks.

- **Slots.** On RP2350 the QSPI XIP controller maps either slot onto `0x10000000`, so both slots are the same linked image. On RP2040 each slot executes at its own flash offset, so each slot is a separate image linked for that address. This tree builds the RP2350 image. See [../README.md](../README.md) layer 0 and [../OTA.md](../OTA.md).

## Now

- [ ] **R1 — The running board matches the unload contract**
  - Layer: 2 and 3, driven from layer 4.
  - Why: The README says Stop unloads a module, and that Lua gets `pex.on_unload` first. Firmware 1.239 refuses `pex.unload` while Lua still holds a reference, and the pages on the board only stop.
  - Needs: The hook in `pex/lua/pexlua.c` and the editor Stop path (`pex.stop`, then `pex.unload`) are in the tree. Shipping them is two separate steps. A new try-before-you-buy image carries the Lua half. Uploading the editor pages before that image is confirmed makes Stop unload the signed Anker module that is running, and the charger ops disappear until the next load and start. The signed file stays.
  - Done when: A confirmed image logs the Lua unload lines, the editor on the board says it stopped and unloaded, and a call into a module after that raises `PEX module '<name>' was unloaded`.

- [ ] **R2 — One way for the editor to hear that a file changed**
  - Layer: 1 notifies layer 4.
  - Why: Two browsers, or a phone that reconnects, do not share an editor buffer. LittleFS has no mtime, and the SD card's timestamps are fixed, so the page hashes open text files on a timer.
  - Needs: A `fs.changed` event `{path, op, size, hash}` for editor paths on the SD card and on LittleFS, from a second callback beside the one Lua already owns. `fs.stat` for up to 32 paths, for the reconnect case. Files over 256 KB send path, op, and size, and skip the hash. The poll in the editor goes away once those events arrive. This rides in the same image as R3. It does not merge two unsaved copies of one file.
  - Done when: A write from one session updates a clean buffer in another, a dirty buffer is asked before it is replaced, and the five-second poll is gone.

## Next

- [ ] **R3 — The page knows which board answered**
  - Layer: 1 tells layer 4. Layer 2 build selection uses it.
  - Why: A `Pexfile` can hold several targets. `pex.list` reports the CPU (`m33` on this board) and does not report the board name, so two targets for different boards with the same CPU cannot be told apart.
  - Needs: `"board":"pico2_w"` on `sys.info`, from `PICO_BOARD`. No new op. The editor keeps targets whose board and arch both match, and says so on the Build button. A target chosen for a different board is dropped on the next connect.
  - Done when: `sys.info` on a Pico 2 W image includes `"board":"pico2_w"`, and the Build button names that board.

- [ ] **R4 — A PEX build finishes, and signing stays on the host**
  - Layer: 2, produced from layer 4, accepted by layer 1.
  - Why: The product image loads signed modules. The host cmake build and the in-browser `pexcc` build both emit unsigned files. The browser build also stops at the SHA-256 of the image, because `crypto.subtle` is missing on plain `http://`. The page cannot hold the OTA key.
  - Needs: A SHA-256 in the page, so the browser build writes `<out>/<name>.m33.pex` and `build.log` on the SD card. Signing stays `pexlink sign` with the OTA key on the host. The product defconfig keeps `CONFIG_PEX_ALLOW_UNSIGNED` off. `configs/pico2w_pexcc_defconfig` remains the image that loads unsigned modules.
  - Done when: A `Pexfile` build on the board's own HTTP page writes the `.pex`, and that file loads on the product image only after `pexlink sign`.

- [ ] **R5 — One Anker source tree**
  - Layer: 2.
  - Why: The sources the editor builds are `sd/projects/anker/src/`. An older copy is still at `sd/pex/`. The signed module in the PEX partition is a third artifact, and it is the one that is running.
  - Needs: A project build that reads `sd/projects/anker/` and writes a `.pex`. That has not happened yet: the last build reached the link and stopped on the digest bug in R4. After a build has read the new tree, deleting `sd/pex/` is a separate decision. Nothing here replaces the signed module, uninstalls `iti` or `otap`, or starts a second Anker.
  - Done when: The card has one Anker source directory, the editor builds from it, and the running module is still the signed one until a signed replacement is deliberately installed.

## Later

- [ ] **R6 — Map and disassembly beside the module**
  - Layer: 2, files written by layer 4.
  - Why: The flash offset of an installed module is known after `pex.install`. A map and a disassembly next to the `.pex` are what a later debugger would use.
  - Needs: `<out>/<name>.map` and `<out>/<name>.dis` from the link. After install, the map can record the partition offset. Check the SWD address of one known symbol against that formula before any tool uses it. This does not drive a probe.
  - Done when: A build writes both files next to the `.pex`, and one symbol's address has been checked by hand.

- [ ] **R7 — Protocols live in PEX, and the Lua files that remain are launchers**
  - Layer: 3 calls layer 2.
  - Why: `lua/anker.lua`, `lua/anker2.lua`, and `lua/pinecil.lua` are launchers. The protocol text is still in `lua/profiles/`. Crypto, frames, and the device wire formats stay in the firmware and in PEX modules.
  - Needs: R11, R12, and R13. Do not start the Lua Anker stack beside the PEX Anker app. Do not add a launch list that starts Anker on boot.
  - Done when: R13 is ticked.

- [ ] **R11 — Can the Anker Lua drop 64-bit values?**
  - Layer: 3, at the boundary with the Anker PEX module.
  - Why: `third_party/lua/lua_pico_conf.h` asks for `LUA_32BITS`, so `lua_Integer` is 32-bit and `lua_Number` is a single-precision float. Neither chip has a double-precision FPU, and a 64-bit number is twice the size in every table. `third_party/lua/luaconf.h` then defines `LUA_32BITS` as 0, and that is the value the firmware build uses, so the VM still runs 64-bit integers and doubles ([lua-net-fs.md](lua-net-fs.md)).
  - Needs: A pass over `lua/profiles/anker/` and over the numbers `pex/modules/drivers/anker_app.c` pushes toward Lua. Record each value that does not fit in a signed 32-bit integer or a single-precision float, and name the field. This item does not change the VM width.
  - Done when: The answer is written here. Either every Anker Lua value fits, or the list of values that do not is on this item. A code change, if the answer allows one, is a new item.

- [ ] **R12 — Finish moving the Pinecil protocol into the PEX module**
  - Layer: 2. The Lua profile is the spec.
  - Why: `pex/modules/drivers/pinecil_app.c` already scans, connects, reads identity, polls the 56-byte live characteristic, and registers `pinecil.set_temp`. `lua/pinecil.lua` only starts that module. `lua/profiles/pinecil.lua` is still the behavior spec, and it still contains the protocol.
  - Needs: Compare the profile to the module and port the confirmed behavior the module does not yet perform. Settings the profile marks as unconfirmed stay marked. The hardware check is H3 in [Hardware-Testing.md](Hardware-Testing.md).
  - Done when: The confirmed Pinecil behavior runs from the PEX module, and H3 is ticked.

- [ ] **R13 — Retire the old Anker and Pinecil Lua ports**
  - Layer: 3.
  - Why: `lua/profiles/anker/` and `lua/profiles/pinecil.lua` are the protocol ports. The live drivers are the PEX modules. The launchers `lua/anker.lua`, `lua/anker2.lua`, and `lua/pinecil.lua` stay.
  - Needs: R12 for Pinecil. H1 and H2 for Anker, and H3 for Pinecil, in [Hardware-Testing.md](Hardware-Testing.md). Those boxes are ticked only after a session on the hardware. Until then the profile files stay, and they stay the spec.
  - Done when: H1, H2, and H3 are ticked, and the profile files are gone or reduced to a pointer at the PEX module. The launchers are still there.

- [ ] **R14 — Further PEX optimization, after the modules work**
  - Layer: 2.
  - Why: R11 asks whether the Anker Lua values fit in 32-bit integers and single-precision floats. Size and speed work past that waits until the modules and the features that use them are actually running.
  - Needs: R11 answered, and R12 and R13 done. Then a measured pass over the PEX modules that are in use: RAM in the pool, time on the main loop, and copies the host API forces. No change that breaks a module H1–H3 has already passed.
  - Done when: The measurements are written here, and any change that shipped has been re-checked on the hardware list.

- [ ] **R15 — Bluetooth Classic RFCOMM, for the PM220**
  - Layer: 1. `BT_CLASSIC`, default off.
  - Why: The CYW43439 is dual-mode. `include/btstack_config.h` already has the Classic block (`ENABLE_CLASSIC`, SDP, RFCOMM, one channel) and it is not compiled. The shape follows `/home/samuel/Projects/pm220-pico2w`, which opens SPP by SDP and falls back to RFCOMM channel 1. `MAX_NR_HCI_CONNECTIONS` is shared with LE, so the RAM budget is part of turning the option on.
  - Needs: `BT_CLASSIC` builds and brings up inquiry, SDP, and one RFCOMM channel. A PEX module, or the firmware if the module boundary is wrong for it, opens that channel to a PM220 the way `pm220-pico2w` does. LE central stays the path to Anker and Pinecil. BNEP is R16, not this item.
  - Done when: With `BT_CLASSIC` on, the board opens an RFCOMM channel to a PM220 and exchanges the same first traffic `pm220-pico2w` does (`BATTERY?`). The default image still has the option off.

- [ ] **R16 — Bluetooth Classic BNEP / NAP**
  - Layer: 1. Its own Kconfig option, depending on `BT_CLASSIC`.
  - Why: A phone tether is another uplink. The board could serve the web app to the phone, and it could reach the WireGuard server through that uplink. `pm220-pico2w` sizes one BNEP channel and does not implement NAP.
  - Needs: R15. The option brings up BNEP and NAP. The netif joins the existing uplink choice (Wi-Fi, Ethernet, USB-NCM, WireGuard) instead of a second stack. Off in the default image.
  - Done when: A phone NAP connection can open the web app, and, when that uplink is selected, WireGuard comes up over it. The option is off unless the defconfig turns it on.

- [ ] **R17 — A BLE peripheral a PEX module or Lua can reshape**
  - Layer: 1, with the database filled from layer 2 or layer 3. New Kconfig option, default off.
  - Why: `BT_LE` is the central: it scans and connects out. The local ATT database is a 1024-byte buffer Lua can rebuild with `gatt_server.begin`, `service`, `characteristic`, and `commit` (`src/bt/le/gattserver.h`). A commit swaps the buffer and signals Service Changed. PEX cannot build that database. SMP is UDP port 1337 only, so a phone cannot send `smpmgr` over BLE.
  - Needs: The new option advertises a peripheral a peer can connect to, beside the central. A PEX module, and Lua, can replace the service and characteristic list while the board is running. One of those services can be MCUmgr's SMP service, so an OTA image can arrive over BLE as well as over UDP 1337. The central path to Anker and Pinecil stays. The default image leaves the option off.
  - Done when: A PEX module and a Lua script can each publish a service a phone can read, change it without a reflash, and the BLE SMP service accepts an image upload. `BT_LE` central behavior is unchanged when the option is off.

- [ ] **R18 — Wi-Fi link recovery**
  - Layer: 1.
  - Why: `src/net/wifi.c` already adopts a rejoin the CYW43 firmware does by itself, because a host join waits while a BLE link holds the radio and the driver's link bit stays down after a firmware rejoin. An access point the firmware does not rejoin still waits for Bluetooth to go idle. Association budget is 6 s (`WIFI_JOIN_MS`); failure backoff is 10 s (`WIFI_RETRY_MS`).
  - Needs: A pass at that scheme after the device protocols are working. The bench is hostapd on a Linux PC or a Raspberry Pi, recorded as H4 in [Hardware-Testing.md](Hardware-Testing.md). Do not change the rule that a join is deferred while the Anker link is up unless the new scheme is shown not to wedge the shared radio.
  - Done when: H4 is ticked, and the recovery that shipped is described on this item.

## Parked

Classic moved to R15 and R16.

- [ ] **R9 — OTP secure boot of the firmware image**
  - Layer: 0.
  - The RP2350 ROM can require a signed image once an OTP key is provisioned. This tree does not record that lock as programmed. PEX and OTA signatures use the OTA key, which is a different lock.

- [ ] **R10 — A network debugger**
  - Considered and rejected for now. R6 is the part that stays in scope. Patching disassembly to insert breakpoints, and debugging over the network, are out of scope.

## Done

- [x] **R0 — Write the layers down** (2026-10-23)
  - [README.md](../README.md) is the layer map and the short build commands. [Development.md](Development.md) is the procedure. SMP is UDP port 1337. UART is not an SMP transport.

## New item

```
- [ ] **R# — Title**
  - Layer:
  - Why:
  - Needs:
  - Done when:
```
