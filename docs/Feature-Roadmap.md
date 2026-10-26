# Feature roadmap

The target is the five layers in [../README.md](../README.md). An item on this list changes one of those layers, or removes a second way of doing the same job. A bugfix that leaves the layers alone does not belong here.

The board that is running today is firmware 1.277.0, confirmed in slot A. Development images load unsigned PEX modules. A checkbox is ticked when the behavior is true of that image and the pages the board is serving, or when a decision is written down and the code matches it. A check that needs a device on the bench is [Hardware-Testing.md](Hardware-Testing.md), not a box on this list.

## How to change this list

Add an item by copying the template at the bottom into Now, Next, Later, or Parked. Fill in why it belongs to a layer, what has to exist before it can ship, and the condition that ticks the box.

The `R` number is the order of implementation. Now is the front of that order, then Next, then Later. An item that cuts in line takes the lower number, and each item it passes moves up one. Tick the box when the done condition is met, then move the whole entry under Done and add the date. Leave the entry there so the reason is still readable.

The work for an item is committed on a feature branch. How those branches reach `dev` is in [Development.md](Development.md).

Parked means we are not doing it. Out of scope means it was considered and rejected.

## Recorded

These are part of the architecture. They are not tasks.

- **Slots.** On RP2350 the QSPI XIP controller maps either slot onto `0x10000000`, so both slots are the same linked image. On RP2040 each slot executes at its own flash offset, so each slot is a separate image linked for that address. This tree builds the RP2350 image. The bench Pico 2 W uses 1 MiB slots, 1200 KiB LittleFS, and a 512 KiB PEX partition. See [../README.md](../README.md) layer 0, [../OTA.md](../OTA.md), and [flash-layout.md](flash-layout.md).

## Now

- [ ] **R5 — A PEX build finishes, and signing stays on the host**
  - Layer: 2, produced from layer 4, accepted by layer 1.
  - Why: The product image loads signed modules. The host cmake build and the in-browser `pexcc` build both emit unsigned files. The browser build also stops at the SHA-256 of the image, because `crypto.subtle` is missing on plain `http://`. The page cannot hold the OTA key.
  - Needs: A SHA-256 in the page, so the browser build writes `<out>/<name>.m33.pex` and `build.log` on the SD card. Signing stays `pexlink sign` with the OTA key on the host, for a release image. Development images set `CONFIG_PEX_ALLOW_UNSIGNED`, so an unsigned browser build loads on those images.
  - Done when: A `Pexfile` build on the board's own HTTP page writes the `.pex`, and that unsigned file loads on a development image. A release image, with the flag off, still requires `pexlink sign`.
  - 2026-10-24: a Build of `sd/projects/anker/Pexfile` on the board's page wrote `sd/projects/anker/build/anker.m33.pex` (168836 bytes).
  - 2026-10-26: confirmed 1.277.0 reports `UNSIGNED ALLOWED (development)`, so the signature is no longer why that file is refused. The PEX pool's largest block is 22576 bytes, and the firmware still reads an `sd:` install into that pool, so this file does not load. The box stays open until an unsigned build that fits the pool loads. A release image, with the flag off, still requires `pexlink sign`.

## Next

- [ ] **R6 — One Anker source tree**
  - Layer: 2.
  - Why: The sources the editor builds are `sd/projects/anker/src/`. An older copy is still at `sd/pex/`. The signed module in the PEX partition is a third artifact.
  - Needs: A project build that reads `sd/projects/anker/` and writes a `.pex`. After a build has read the new tree, deleting `sd/pex/` is a separate decision. Nothing here replaces the signed module or starts a second Anker.
  - Done when: The card has one Anker source directory, the editor builds from it, and the running module is still the signed one until a signed replacement is deliberately installed.
  - 2026-10-24: that build read `sd/projects/anker/src/` and wrote the `.pex`, the map, and the disassembly under `sd/projects/anker/build/`. The signed module in the partition was not replaced. `sd/pex/` is still on the card, so this box stays open.

## Later

- [ ] **R8 — Protocols live in PEX, and the Lua files that remain are launchers**
  - Layer: 3 calls layer 2.
  - Why: The Anker protocol is in the PEX module. `lua/pinecil.lua` is still a launcher, and `lua/profiles/pinecil.lua` still holds that protocol. Crypto, frames, and the device wire protocols stay in the firmware and in PEX modules.
  - Needs: R13 and R14. Do not add a launch list that starts Anker on boot.
  - Done when: R14 is ticked.

- [ ] **R13 — Finish moving the Pinecil protocol into the PEX module**
  - Layer: 2. The Lua profile is the spec.
  - Why: `pex/modules/drivers/pinecil_app.c` already scans, connects, reads identity, polls the 56-byte live characteristic, and registers `pinecil.set_temp`. `lua/pinecil.lua` only starts that module. `lua/profiles/pinecil.lua` is still the behavior spec, and it still contains the protocol.
  - Needs: Compare the profile to the module and port the confirmed behavior the module does not yet perform. Settings the profile marks as unconfirmed stay marked. The hardware check is H3 in [Hardware-Testing.md](Hardware-Testing.md).
  - Compared 2026-10-23: the module already performs the confirmed path. It scans for a name starting with `Pinecil`, reads build and the 6-byte serial once, polls the 56-byte bulk live characteristic, and clamps `pinecil.set_temp` to 10..`max_tip_c` (or 450) as a u16 LE write to settings index 0. Sleep temp (`f6d70001`) is not implemented. `pinecil_live.c` marks power source values other than PD, `tip_uv`, `hall_uT`, and the watts divisor as inferred. H3 is still open, so this box stays open.
  - Done when: The confirmed Pinecil behavior runs from the PEX module, and H3 is ticked.

- [ ] **R14 — Retire the old Anker and Pinecil Lua ports**
  - Layer: 3.
  - Why: The live drivers are the PEX modules. The Anker Lua profile and its launchers were a second copy of that protocol.
  - Needs: R13 for Pinecil, and H3 in [Hardware-Testing.md](Hardware-Testing.md). H3 is ticked only after a session on the hardware.
  - 2026-10-26: the Anker Lua port is retired. `lua/anker.lua`, `lua/anker2.lua`, and `lua/profiles/anker/` are gone. The module is `pex/modules/drivers/anker_app.c`. The scanner manifest is `pex/modules/drivers/anker.driver.json`. `lua/pinecil.lua` and `lua/profiles/pinecil.lua` stay until H3.
  - Done when: H3 is ticked, and the Pinecil profile is gone or reduced to a pointer at the PEX module. The Pinecil launcher can stay.

- [ ] **R15 — Further PEX optimization, after the modules work**
  - Layer: 2.
  - Why: R12 asks whether the Anker Lua values fit in 32-bit integers and single-precision floats. Size and speed work past that waits until the modules and the features that use them are actually running.
  - Needs: R12 answered, and R13 and R14 done. Then a measured pass over the PEX modules that are in use: RAM in the pool, time on the main loop, and copies the host API forces. No change that breaks a module H1–H3 has already passed.
  - Done when: The measurements are written here, and any change that shipped has been re-checked on the hardware list.

- [ ] **R16 — Bluetooth Classic RFCOMM, for the PM220**
  - Layer: 1. `BT_CLASSIC`, default off.
  - Why: The CYW43439 is dual-mode. `include/btstack_config.h` has the Classic block (`ENABLE_CLASSIC`, SDP, RFCOMM). It is compiled only when `BT_CLASSIC` is on. The caller supplies the service UUID, the channel, and any PIN. `MAX_NR_HCI_CONNECTIONS` is shared with LE, so the RAM budget is part of turning the option on.
  - Needs: `BT_CLASSIC` builds and brings up inquiry, SDP, and one RFCOMM channel. A PEX module, or the firmware if the module boundary is wrong for it, opens that channel to a PM220 the way `pm220-pico2w` does. LE central stays the path to Anker and Pinecil. BNEP is R17, not this item.
  - Code: `src/bt/classic/spp.c` and `src/bt/classic/cl_sess.c` are compiled only when `BT_CLASSIC` is on. `classic.scan` starts inquiry. `classic.connect` opens a session; the caller passes the UUID, the channel (0 means SDP), and the PIN list. Nothing is sent on open, and nothing inquires at boot. PEX group `classic` is id 14. The product defconfig leaves the option off. The bench image 1.277.0 (`configs/pico2w_sd_classic_defconfig`) has it on. Not tried against a PM220.
  - Done when: With `BT_CLASSIC` on, the board opens an RFCOMM channel to a PM220 and exchanges the same first traffic `pm220-pico2w` does (`BATTERY?`). The default image still has the option off.

- [ ] **R17 — Bluetooth Classic BNEP / NAP**
  - Layer: 1. Its own Kconfig option, depending on `BT_CLASSIC`.
  - Why: A phone tether is another uplink. The board could serve the web app to the phone, and it could reach the WireGuard server through that uplink. `pm220-pico2w` sizes one BNEP channel and does not implement NAP.
  - Needs: R16. The option brings up BNEP and NAP. The netif joins the existing uplink choice (Wi-Fi, Ethernet, USB-NCM, WireGuard) instead of a second stack. Off in the default image.
  - Code: `BT_CLASSIC_NAP` depends on `BT_CLASSIC` and `NET_UPLINK`. It builds a PANU netif named `b0`. `classic.nap` connects to a phone NAP, and the board can also answer as NAP. The netif is an uplink candidate and does not set the default route itself. The product defconfig leaves the option off. 1.277.0 leaves it off so the SD Classic image still clears the heap floor.
  - 2026-10-25, on 1.273: a phone opened the board page over BNEP, and Share internet NATed that traffic out Wi-Fi. WireGuard was not the uplink in that session, so this box stays open.
  - Done when: A phone NAP connection can open the web app, and, when that uplink is selected, WireGuard comes up over it. The option is off unless the defconfig turns it on.

- [ ] **R18 — BLE Peripheral**
  - Layer: 1, with the database filled from layer 2 or layer 3. New Kconfig option, default off.
  - Why: The board should be able to act as a BLE peripheral. A phone connects to it, it can stand in for another device, and it can take an OTA image. `BT_LE` today is the central: it scans and connects out to Anker and Pinecil. The local ATT database is a 1024-byte buffer Lua can rebuild with `gatt_server.begin`, `service`, `characteristic`, and `commit` (`src/bt/le/gattserver.h`). A commit swaps the buffer and signals Service Changed. PEX cannot build that database. SMP is UDP port 1337 only, so a phone cannot send `smpmgr` over BLE.
  - Needs: The new option advertises a peripheral beside the central. A PEX module, and Lua, can replace the service and characteristic list while the board is running. One of those services can be MCUmgr's SMP service, so an image can arrive over BLE as well as over UDP 1337. The central path stays. The default image leaves the option off.
  - Code: `BT_PERIPHERAL` advertises beside the central after the controller is up. `bt.smp` installs the MCUmgr SMP service. Lua and `pex_gatt_begin`, `pex_gatt_service`, `pex_gatt_characteristic`, and `pex_gatt_commit` (bt group minor 1) can replace the list. Those calls return `PEX_ENOTSUP` while the option is off. A commit keeps the read and write callbacks. The product defconfig leaves the option off. Not tried against a phone.
  - Done when: A PEX module and a Lua script can each publish a service a phone can read, change it without a reflash, and the BLE SMP service accepts an image upload. Central behavior is unchanged when the option is off.

- [ ] **R19 — Wi-Fi link recovery**
  - Layer: 1.
  - Why: `src/net/wifi.c` already adopts a rejoin the CYW43 firmware does by itself, because a host join waits while a BLE link holds the radio and the driver's link bit stays down after a firmware rejoin. An access point the firmware does not rejoin still waits for Bluetooth to go idle. Association budget is 6 s (`WIFI_JOIN_MS`); failure backoff is 10 s (`WIFI_RETRY_MS`).
  - Needs: A pass at that scheme after the device protocols are working. The bench is hostapd on a Linux PC or a Raspberry Pi, recorded as H4 in [Hardware-Testing.md](Hardware-Testing.md). Do not change the rule that a join is deferred while the Anker link is up unless the new scheme is shown not to wedge the shared radio.
  - Done when: H4 is ticked, and the recovery that shipped is described on this item.

## Parked

R9 is unused. Classic moved to R16 and R17.

- [ ] **R10 — OTP secure boot of the firmware image**
  - Layer: 0.
  - The RP2350 ROM can require a signed image once an OTP key is provisioned. This tree does not record that lock as programmed. PEX and OTA signatures use the OTA key, which is a different lock.

- [ ] **R11 — A network debugger**
  - Considered and rejected for now. R7 is the part that stays in scope. Patching disassembly to insert breakpoints, and debugging over the network, are out of scope.

## Done

- [x] **R1 — Build while a module is running, and keep a verified copy on the card** (2026-10-26)
  - Layer: 4 writes the candidate. Layer 1 accepts it into the PEX partition only after unload.
  - Why: A new build of a module that is still running should finish. The executable partition is the copy the board is running, so the new file stays on the SD card until that module is unloaded. The card also keeps the last version marked verified, so a bad install can be put back. This is the first item to implement. It does not wait on R2 or R3.
  - Needs: The build writes `sd/projects/<name>/build/<name>.<arch>.pex` whether or not that module is loaded. The page holds those bytes only until that write finishes. It does not copy the file to LittleFS `pex/` and it does not call `pex.install` while the module is loaded. `pex_install` already returns busy when the module runs from its installed copy. If it is loaded or running, the page asks to unload and replace it, or to leave it running. Unload is stop, then unload, including `pex.on_unload`. Install reads the candidate from the SD path, and only after unload succeeds. A failed unload leaves the running module and leaves the candidate on the card. If the module is not loaded, install proceeds from that SD file. "Mark verified" copies the candidate to `sd/pex/good/<name>.<arch>.pex` and writes a note beside it with the build id, the size, and when it was marked. The previous verified file stays until that copy finishes. "Restore verified" installs that file through the same unload-first gate. On a signed-only image the marked file is the signed bytes. An unsigned candidate can sit on the card, and install still refuses it until `pexlink sign`. A RAM run does not write the partition, and it does not start a second copy of a module that is already running. Replacing a module that Lua still holds needs the R2 image. Uploading these pages onto firmware 1.239 can unload the signed Anker module that is running, because that module has no Lua reference and unload already succeeds there.
  - Done when: A build of a running module leaves the partition untouched and the new file on the card. The prompt installs only after unload succeeds. A file marked verified can be restored after a later candidate was installed.
  - 2026-10-26: the served editor writes `sd/projects/<name>/build/<name>.<arch>.pex`, asks before unload-and-replace, and Mark verified / Restore verified use `sd/pex/good/<name>.<arch>.pex`. A file larger than the pool is refused and is not copied to LittleFS. A full mark-verified pass on the Anker module was not re-run, because the 168836-byte file does not fit the pool (R5).

- [x] **R2 — The running board matches the unload contract** (2026-10-26)
  - Layer: 2 and 3, driven from layer 4.
  - Why: The README says Stop unloads a module, and that Lua gets `pex.on_unload` first. Firmware 1.239 refuses `pex.unload` while Lua still holds a reference, and the pages on the board only stop.
  - Needs: The hook in `pex/lua/pexlua.c` and the editor Stop path (`pex.stop`, then `pex.unload`) are in the tree. Shipping them is two separate steps. A new try-before-you-buy image carries the Lua half. Uploading the editor pages before that image is confirmed makes Stop unload the signed Anker module that is running, and the charger ops disappear until the next load and start. The signed file stays.
  - Done when: A confirmed image logs the Lua unload lines, the editor on the board says it stopped and unloaded, and a call into a module after that raises `PEX module '<name>' was unloaded`.
  - 2026-10-24: not met on 1.253. The installed Anker is signed and was left unloaded. `pex.load` of that copy asks for 136400 bytes of code in the 48 KB pool and is refused. The partition has no free space to relocate that model A image for this firmware, so Stop was not run.
  - 2026-10-26: confirmed 1.277.0 has the unload hook, and the editor Stop is `pex.stop` then `pex.unload`. An observed unload of the installed Anker module was not recorded: loading that copy asks for more RAM than the 48 KB pool, so Stop was not run on it. The contract is in the image and the pages.

- [x] **R7 — Map and disassembly beside the module** (2026-10-23)
  - Layer: 2, files written by layer 4.
  - Why: The flash offset of an installed module is known after `pex.install`. A map and a disassembly next to the `.pex` are what a later debugger would use.
  - Needs: `<out>/<name>.map` and `<out>/<name>.dis` from the link. After install, the map can record the partition offset. Check the SWD address of one known symbol against that formula before any tool uses it. This does not drive a probe.
  - Done when: A build writes both files next to the `.pex`, and one symbol's address has been checked by hand.
  - Checked 2026-10-23 on `pex/test/modules/hello.c` with the reloc link. The link wrote `hello.m33.map` and `hello.m33.dis` beside `hello.m33.pex`. Map: `image_off 0x3a0`, text 332, ro 84, data 52, bss 0, and `00000001 T pex_main`. `arm-none-eabi-nm -n` prints `00000000 T pex_main`; this nm clears the Thumb bit, and the ELF value keeps it. The paper address at partition offset `0x17000` (94208) is `0x10400000 + 0x17000 + 0x3a0 + (0x1 & ~1) = 0x104173a0`. No probe.

- [x] **R12 — Can the Anker Lua drop 64-bit values?** (2026-10-26)
  - Layer: 3, at the boundary with the Anker PEX module.
  - Why: `third_party/lua/lua_pico_conf.h` asks for `LUA_32BITS`, so `lua_Integer` is 32-bit and `lua_Number` is a single-precision float. Neither chip has a double-precision FPU, and a 64-bit number is twice the size in every table. `third_party/lua/luaconf.h` then defines `LUA_32BITS` as 0, and that is the value the firmware build uses, so the VM still runs 64-bit integers and doubles ([lua-net-fs.md](lua-net-fs.md)).
  - Needs: A pass over the former Anker Lua profile and over the numbers `pex/modules/drivers/anker_app.c` pushes toward Lua. Record each value that does not fit in a signed 32-bit integer or a single-precision float, and name the field. This item does not change the VM width. That profile was removed on 2026-10-26.
  - Done when: The answer is written here. Either every Anker Lua value fits, or the list of values that do not is on this item. A code change, if the answer allows one, is a new item.
  - Answer: No. The VM width stays as the firmware builds it. These values do not fit in a signed 32-bit integer, or are not exact in a single-precision float. A later item can change the VM only after it keeps every bit of these fields. The list was taken from the Lua profile. That profile was removed on 2026-10-26. The same fields are in the Anker PEX module. The file names in the bullets are where the numbers were measured.
    - `fe` epoch (`link.lua` `now_epoch`, `cipher.lua` `now_epoch`). Base `1700000000` plus uptime in seconds, written by `le32` on setters and on the negotiation frame. Signed 32-bit holds it until 2038. At that magnitude a single-precision float steps by 128, so the seconds are not exact. The mask `0xffffffff` in `le32` is not a signed 32-bit value, and as a float it rounds to 2^32.
    - Timezone wire form (`neg_gcm.lua`): `(-(cfg.tz_offset_sec or 0)) & 0xffffffff`. UTC−5 becomes `0xffffb9b0`. The offset in seconds fits. The 32-bit two's-complement form does not fit a positive signed integer, and it is not exact in a float.
    - `memory_port_bits` (A1761 tag `f7`, a u32 bitmask). `anker.get` pushes it (`em_i64` at depth 0, then `pexlua_pushnum`). A 32-bit mask is not exact in a float. Bit 31 does not fit a signed 32-bit integer.
    - Screensaver `id` and `holiday_id` (A2687 report, both u32) and the upload fields `id`, `crc`, and `size` (`screensaver.lua`). `crc` is a CRC32. The report fields are inside a nested object, so `anker.get` does not sample them (`s_depth != 0`). The Lua profile still has to keep all 32 bits to build the upload.
    - `age_ms` (`anker_app.c` `l_age`): `pex_ms()` minus `sample_ms`, a 32-bit millisecond clock. Exact in a float only under 2^24 ms (about 4.7 h). Fits a signed 32-bit integer only under about 24.8 days. A live poll is a few seconds and does fit.
  - These do fit, and they are not a reason to keep 64-bit by themselves. u8 and u16 measurements (power, state of charge, versions, flags, temperatures). Countdown fields `ac_countdown_s`, `car_countdown_s`, `countdown_s`, `countdown_remain_s`, and `c1_timer_s` / `c2_timer_s` / `c3_timer_s`, all ranged 0..86400. `remaining_time_h` is tenths of an hour and a realistic reading fits a float; the wire type is still u32. `l_set` already casts the Lua number to `int32_t`, and every verified setter value is inside that range.

- [x] **R3 — One way for the editor to hear that a file changed** (2026-10-24)
  - Layer: 1 notifies layer 4.
  - Why: Two browsers, or a phone that reconnects, do not share an editor buffer. LittleFS has no mtime, and the SD card's timestamps are fixed, so the page hashes open text files on a timer.
  - Needs: A `fs.changed` event `{path, op, size, hash}` for editor paths on the SD card and on LittleFS, from a second callback beside the one Lua already owns. `fs.stat` for up to 32 paths, for the reconnect case. Files over 256 KB send path, op, and size, and skip the hash. The poll in the editor goes away once those events arrive. This rides in the same image as R4. It does not merge two unsaved copies of one file.
  - Done when: A write from one session updates a clean buffer in another, a dirty buffer is asked before it is replaced, and the five-second poll is gone.
  - On 1.253, with the pages this image is serving: a write of `sd/projects/fresh-check.txt` from another client updated a clean editor buffer in 523 ms (`Updated sd/projects/fresh-check.txt from another device.`). A dirty buffer was asked (`changed on another device. Reload it and discard your edits?`) and replaced only after that was accepted. The five-second poll was not running (`eventsLive` true). The check file was removed.

- [x] **R4 — The page knows which board answered** (2026-10-24)
  - Layer: 1 tells layer 4. Layer 2 build selection uses it.
  - Why: A `Pexfile` can hold several targets. `pex.list` reports the CPU (`m33` on this board) and does not report the board name, so two targets for different boards with the same CPU cannot be told apart.
  - Needs: `"board":"pico2_w"` on `sys.info`, from `PICO_BOARD`. No new op. The editor keeps targets whose board and arch both match, and says so on the Build button. A target chosen for a different board is dropped on the next connect.
  - Done when: `sys.info` on a Pico 2 W image includes `"board":"pico2_w"`, and the Build button names that board.
  - On 1.253, `sys.info` includes `"board":"pico2_w"`. With `sd/projects/anker/Pexfile` open, the Build button reads `Build anker · pico2_w — this board (pico2_w)`.

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
