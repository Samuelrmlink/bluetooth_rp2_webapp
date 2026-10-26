# Memory notes — Classic on the SD image

Measured again 2026-10-25 after the bench board moved to 1 MiB slots.
`/tmp/btgw_sdcl_1m` is `configs/pico2w_sd_classic_defconfig` with HTTPS off,
version 1.276. That signed image was confirmed on the bench in slot B.
1.277 is the same config with unsigned PEX modules allowed. It linked at the
same size and gap, and it is the confirmed image in slot A.
`btgw.bin` is 831,288 bytes. The signed image
is 831,352 and has 217,224 bytes spare in the 1 MiB slot. `__end__` is
`0x200761f4`, so the heap gap is 40,460. `/tmp/btgw_cl_1m` is the same tree
without SD or the W5500, HTTPS on: `btgw.bin` 829,952, gap 19,196. Both
clear the 8,192 floor. Wi-Fi and WireGuard stay on. NAP stays off.

The notes below are the earlier 864 KiB attempt, which did not link. They
are kept because they name the objects. The TLS pool is what the SD image
gives up.

Measured 2026-10-25 from the uncommitted `feature/classic-spp` tree
(worktree `bluetooth_rp2_webapp-classic-spp`, base `d81ecd6`). Two configures,
neither flashed:

| Build dir | Defconfig | Result |
|---|---|---|
| `/tmp/btgw_classic` | `configs/pico2w_classic_defconfig` | links. `btgw.bin` 841,288 bytes. Heap gap 17,560 |
| `/tmp/btgw_sdclassic` | `configs/pico2w_sd_classic_defconfig` | does not link. Map kept, no ELF |

Both have TLS on (`CONFIG_NET_HTTP_TLS`, `CONFIG_NET_TLS_POOL_KB=40`),
Classic on, NAP off, `CONFIG_BT_ACL_PAYLOAD=1021`, `CONFIG_BT_MAX_LINKS=4`,
`CONFIG_LUA_HEAP_KB=116`, `CONFIG_PEX_POOL_KB=48`. Unsigned stays off.
The only `.config` difference is the SD card (SDIO) and the W5500.
The running bench image is still 1.274. Nothing in this note was applied:
the defconfigs, the arena, the PEX pool, and `pool_mem` were left alone.

The numbers below are the failed SD Classic link unless a row says otherwise.
Input-section sums sit about 900 bytes under the linker total. That remainder
is alignment between output sections. Named objects are the map's own sizes.

## The two limits

Flash and SRAM are different ceilings.

The slot is 864 KiB (`884,736` bytes), `FLASH` at `0x10000000` in
`linker/pico_flash_region.ld`. Main A and Main B are the same size. Growing
one means moving Main B, moving the 256 KB CYW43 firmware partition, and
shrinking LittleFS on every board. It does not add SRAM. The CYW43 blob is
outside the slot and is shared by Wi-Fi and Bluetooth, so dropping one of
those interfaces does not free that partition.

SRAM is the chip's 512 KiB at `0x20000000`, length `0x80000`. `__HeapLimit`
is `0x20080000`. Scratch X/Y sit past that and are not the heap. `.heap` is
a 2,048-byte spacer (`PICO_HEAP_SIZE`) placed at `__end__`. The link succeeds
only when `__end__ + 2048 <= 0x20080000`. The project floor is a gap of at
least 8,192 bytes, which needs `__end__ <= 0x2007e000`.

Runtime `malloc` (a Classic session's TX buffer and RX ring, the Lua source
scratch) does not move `__end__`. It spends the gap the link already left.

## The miss

| | Failed SD Classic | Fitting non-SD Classic |
|---|---:|---:|
| Linked load | 930,076 (`0x100e311c`) | 841,188 load, bin 841,288 |
| Past the 864 KiB slot | 45,340 | 43,448 spare |
| `__end__` | `0x20080cf0` | `0x2007bb68` |
| Gap `__HeapLimit - __end__` | none. 3,312 bytes past RAM, then the 2,048 spacer. Linker reports RAM overflowed by 5,360 | 17,560 |
| Static RAM (data + bss + the 288 bytes of vectors before `.data`) | 527,600 | 506,728 |

`.text` is 788,084, `.rodata` 126,916, `.ARM.exidx` 144, `.binary_info` 32,
`.data` 14,896, `.bss` 512,416. Four bytes of pad sit between `.text` and
`.rodata`.

To clear the floor (gap ≥ 8,192) the failed image needs 11,504 bytes of
static RAM back (`0x20080cf0 - 0x2007e000`). The fitting image's whole gap
is 17,560, and the SD card's own BSS is 17,807, so the SD stack by itself
eats the margin the non-SD image had.

Flash wanted by the SD image is 88,888 above the fitting image's load
address. SRAM is 20,872 above. By category the flash delta is SD+FatFs
49,666, the rest of the app 16,938 (HTTP, main, fs, Lua, and PEX files that
grew SD paths), W5500 7,780, HTTP 5,903, Lua 3,112, PEX 2,672, lwIP 1,660,
pico 1,128. The SRAM delta is SD+FatFs 17,807, HTTP 1,572, W5500 798, and a
few hundred bytes of alignment.

## Where the linked bytes go

Failed image, input sections. Flash is text + rodata + the loadable copy of
`.data`. SRAM is `.data` + `.bss`.

| | Flash | SRAM |
|---|---:|---:|
| Lua (VM, bindings, `pexlua.c`) | 164,428 | 191,852 |
| Rest of the app | 110,676 | 51,778 |
| BTstack core (shared by Classic and LE) | 98,597 | 16,049 |
| HTTP server + client | 68,276 | 39,274 |
| mbedTLS | 64,929 | 16 |
| lwIP | 51,260 | 68,410 |
| SD + FatFs | 49,666 | 17,807 |
| newlib + libgcc | 43,007 | 9,486 |
| Pico SDK + TinyUSB | 42,736 | 14,941 |
| PEX | 42,527 | 55,199 |
| WireGuard | 34,751 | 1,688 |
| CYW43 driver | 32,543 | 3,809 |
| BLE + BT core (app side) | 27,964 | 36,790 |
| mDNS | 27,364 | 5,338 |
| Classic RFCOMM | 25,018 | 3,677 |
| LittleFS | 14,616 | (in the app / fs rows) |
| Anker codec + crypto | 12,129 | 0 |
| OTA | 10,886 | 10,492 |
| W5500 | 7,780 | 798 |

Those rows sum to 929,153 of the 930,076 load bytes. mbedTLS is 16 bytes of
SRAM of its own. The 40,960-byte TLS pool is counted in the Lua row because
it is allocated in `lheap.c`. The PEX row includes the 49,152-byte
`s_static_pool`. The Lua row's SRAM breakdown is the next section but one.

Eight named objects account for 319,894 bytes, 61% of the 527,600. Each one already sits inside a row of the table above.

| Object | Bytes | What it is |
|---|---:|---|
| `arena` in `lheap.c` | 118,784 | Lua heap. `CONFIG_LUA_HEAP_KB` 116, exactly 116×1024 |
| `s_static_pool` in `pex_gw.c` | 49,152 | PEX RAM pool. `CONFIG_PEX_POOL_KB` 48 |
| `pool_mem` in `lheap.c` | 40,960 | HTTPS client sessions. `CONFIG_NET_TLS_POOL_KB` 40. Present only while TLS is on |
| `ram_heap` in lwIP `mem.c` | 32,787 | `CONFIG_LWIP_MEM_SIZE_KB` 32, plus the heap header |
| `memp_memory_PBUF_POOL_base` | 24,515 | `PBUF_POOL_SIZE` 16 |
| `conns` in `http.c` | 24,128 | `HTTP_CONNS` 8. 3,016 bytes each on this image |
| `links` in `central.c` | 20,208 | Four LE connections. 5,052 bytes each. See below |
| `hci_connection_storage` | 9,360 | Four BTstack ACL connections at 2,340 bytes, Classic payload 1021 |

Next tier, still static: SD `s_x[SD_XFERS]` 8,816 (two transfers, each a
4,096-byte buffer plus about 300 bytes of state), the advertisement registry
`devs[48]` 7,680, lwIP `TCP_PCB` pool 6,563 (40 PCBs), TinyUSB NCM endpoint
buffer 6,416, `usb_ncm.c` receive buffer 6,096, OTA `img_mgmt` upload state
4,824, the SD worker stack 4,096.

The map's `.rodata.main.str1.4` (39,573) and `.rodata.status_text.str1.1`
(43,510) are linker-merged string pools. The "size before relaxing" lines
are the original objects: 591 bytes in `main.c.o` and 159 bytes for
`status_text`. Later `.str1.4` and `.str1.1` contributions show size 0
because they were folded into those two pools. They are real flash, and
they are spread through the firmware. They are not two arrays to delete.

On the fitting image the same `conns` array is 23,168 (2,896 bytes each).
The SD-only fields in `http_conn_t` are 120 bytes a connection, 960 bytes
across the eight. The rest of `http.c`'s 1,572-byte SRAM growth is other
static buffers in that file.

## One interface removed, the others kept

Marginal cost on this image. Shared driver code stays while any user of it
remains. Wi-Fi and Classic are separate rows even though they share the
CYW43. These are not a second build. The Classic and BLE SRAM figures
include a sizeof of the pools that shrink only when that file is recompiled
without the matching `ENABLE_*` flag. `--gc-sections` does not drop a pool
that `btstack_memory_init` still names.

| Interface removed | Flash | SRAM | What actually leaves |
|---|---:|---:|---|
| Wi-Fi | 6,476 | 3,430 | `wifi.c` 5,476/1,382 and `wifi_ops.c` 1,000/2,048. The CYW43 driver stays |
| Classic | 26,498 | 12,915 | The Classic translation units (3,689 SRAM) plus 9,226 of pools that shrink on a recompile |
| BLE | 47,651 | 29,487 | App LE files 20,276/27,255 plus the BLE BTstack files 27,375/2,232 |
| USB-NCM | 10,541 | 17,183 | `usb_ncm.c` + descriptors 1,484/6,451, TinyUSB 9,057/10,732 |
| W5500 | 13,172 | 798 | Driver 7,780/798, lwIP `acd.c`+`autoip.c` 1,296/0, and 4,096 of SPI flash that the non-ethernet image does not carry. SPI SRAM stays (`IO_SPI` is on either way). SD is SDIO and does not select SPI |
| WireGuard | 34,751 | 1,688 | The whole cipher and netif. Not shared with the Anker codec |

CYW43 is 32,543 flash and 3,809 SRAM while any of Wi-Fi, Classic, or BLE
remains. The largest pieces are `cyw43_ll.c` 12,693, `cybt_shared_bus.c`
7,714, the PIO SPI bus 4,020, `cyw43_ctrl.c` 3,232/2,528. It drops only when
all three are off. CMake links `pico_cyw43_arch_lwip_poll` unconditionally.
`CONFIG_NET_WIFI` only adds the two app files. A BLE-only build over USB-NCM
is what that Kconfig help already describes.

Of the 9,226 bytes that a Classic-off recompile gives back:

| Pool | Classic size | LE-only size | Saved |
|---|---:|---:|---:|
| `hci_connection_t` × 4 | 2,340 → 9,360 | 768 → 3,072 | 6,288 |
| `hci_stack_t` | 1,344 | 544 | 800 |
| CYW43 HCI receive buffer | 1,033 | 271 | 762 |
| L2CAP channels (8×188 vs 6×96) | 1,504 | 576 | 928 |
| L2CAP services (5×16 vs 3×16) | 80 | 48 | 32 |
| RFCOMM channels, multiplexers, services, SDP record items | 416 | 0 | 416 |

8,464 of that is inside BTstack pools that stay allocated for BLE, just
smaller. 762 is the CYW43 HCI buffer (the driver stays; the buffer shrinks
because `HCI_ACL_PAYLOAD_SIZE` goes from 1021 back to 259). The name-based
split of `hci.c` and `l2cap.c` text was not added on top. Those files stay
for LE, and a guess at how much of their text would die is not a linked size.

`CONFIG_BT_CLASSIC` depends on `CONFIG_BT_LE`, so this tree will not build
Classic without BLE without a Kconfig change. `ENABLE_BLE` in
`include/btstack_config.h` is unconditional. Removing BLE does not shrink
the Classic-sized ACL connection pool, because Classic keeps payload 1021.
uECC (22,104 flash, 4 SRAM) stays if BLE is removed: OTA compiles it when
LE is off, and `pico_btstack_ble` carries it when LE is on.

`src/net/cyw43_wait.c` calls `tud_task()` with no `#if CONFIG_NET_USB_NCM`.
CMake only links TinyUSB when NCM is on, so turning NCM off does not compile
until that pump is stubbed. The 10,541/17,183 figure is the drop once the
pump goes with the interface. The DHCP server (1,884 flash, no SRAM) stays
for any single removal, because Wi-Fi, USB-NCM, and Ethernet server mode
all use it. The lwIP heap and the pbuf pools are not per interface.

## Bluetooth LE

### The 20,208 bytes are four connections

`static le_link_t links[CONFIG_BT_MAX_LINKS]` in `src/bt/le/central.c`.
`CONFIG_BT_MAX_LINKS` is 4. The map's `.bss.links` is `0x4ef0` = 20,208, so
each slot is 5,052 bytes. `sizeof(le_link_t)` compiled against this image's
`autoconf.h` is 5,052, and the field offsets below add up to that.

This is the live ATT connection, one slot per peer that is connected. It is
the table `BT_MAX_LINKS` sizes. The Kconfig help says each link costs roughly
1 KB. That sentence describes a typical filled GATT tree (the `gattdb.h`
comment: 8 services, 40 characteristics, 20 descriptors). The bytes reserved
at link time are the caps, and every slot carries the caps even when the
peer is small.

`GATT_MAX_CHRS` is 160 because a Pinecil V2 exposes over a hundred
characteristics. A lower cap returns a partial tree. The other caps are
`GATT_MAX_SVCS` 16, `GATT_MAX_DSCS` 48, `GATT_MAX_UUID128` 96.

One `le_link_t`:

| Piece | Bytes |
|---|---:|
| GATT mirror `gattdb_t` | 3,920 |
| services, 16 × 10 | 160 |
| characteristics, 160 × 12 | 1,920 |
| descriptors, 48 × 6 | 288 |
| interned 128-bit UUIDs, 96 × 16 | 1,536 |
| flags, counts, MTU, padding | 16 |
| ATT op queue, 4 × 264 | 1,056 |
| BTstack discovery cursors (`gatt_client_service_t` 22 + `gatt_client_characteristic_t` 26) | 48 |
| Address, handles, discovery stage, queue indexes, negotiate-until | 28 |
| Slot total | 5,052 |

Each queued op is a 12-byte header plus `OP_DATA_MAX` 252 bytes of payload.
The queue is 4 deep so a subscribe, a write, and a read can be waiting while
one procedure is in flight. The 252-byte payload is the LE ATT ceiling (255
in `btstack_config.h`, minus the 3-byte header).

Four slots: 15,680 of GATT mirrors, 4,224 of op queues, 192 of discovery
cursors, 112 of the rest.

BTstack still has its own per-ACL record beside this. That is
`hci_connection_storage`, 9,360 bytes for four connections, in the shared
BTstack pool. A Classic session and an LE link both count against
`BT_MAX_LINKS`.

### The scan list is a different object

`devs[BT_REGISTRY_MAX]` in `src/bt/registry.c`, default 48. `.bss.devs` is
7,680. `sizeof(bt_device_t)` is 160. Each entry is one peer the scanner has
seen: name (31), manufacturer data (31), up to four 16-bit advertised UUIDs,
the raw advertisement (31), the raw scan response (31), RSSI, and the
connection handle. The transport field is LE or Classic. Classic inquiry
also keeps its own 16-entry result table inside the Classic session code
(`CL_DEV_MAX`), which is part of the 3,677 bytes of Classic SRAM, not this
array.

### Other LE SRAM in the same files

| Object | Bytes | What it is |
|---|---:|---|
| `s_inbox[8]` in `central.c` | 2,432 | Notify/read slots for Lua. 304 bytes each, value buffer 256. Compiled with `CONFIG_LUA_ENABLE`. Overflow drops the oldest |
| `ev` in `central.c` | 1,600 | One event JSON buffer, function-local static |
| two `json` buffers in `central.c` | 768 each | Device-event JSON. Same shape as the 768-byte buffers next to `emit_device_event` |
| `entries[24]` in `gattserver.c` | 1,104 | The local GATT database record (what a peer reads from us). 24 entries, 46 bytes, uuid string included. Separate from the per-link remote cache |

`central.c` as a whole is 12,964 flash and 25,888 SRAM. The link table and
the inbox are most of that RAM.

## Lua

The interpreter is flash. The RAM that shows up beside it is the arena, the
TLS pool that happens to live in the same file, and binding scratch.

### The VM, `third_party/lua/liblua.a`

107,875 bytes of flash. 4 bytes of SRAM, in `lstate.c`.

Scripts are compiled on the device, so the parser and the code generator are
linked. They are not a host-only tool.

| Group | Flash | Files |
|---|---:|---|
| VM, GC, API, tables, strings, debug | 61,307 | `lvm` 17,332, `lgc` 9,791, `lapi` 7,972, `ldo` 7,116, `ltable` 4,344, `lobject` 3,980, `ldebug` 2,984, `ltm` 2,765, `lstring` 1,612, `lstate` 1,564, `lfunc` 920, `lmem` 688, `lzio` 156, `lopcodes` 83 |
| On-device compiler | 19,678 | `lparser` 7,814, `lcode` 6,056, `llex` 3,468, `lundump` 1,530, `ldump` 810 |
| Standard libraries | 26,890 | `lstrlib` 12,844, `lauxlib` 3,784, `lbaselib` 2,912, `lmathlib` 2,444, `ltablib` 2,264, `lutf8lib` 1,541, `lcorolib` 844, `lctype` 257 |

61,307 + 19,678 + 26,890 = 107,875. There is no separate Lua SRAM heap inside
liblua. Every string, table, coroutine, and prototype is allocated from the
arena through `lheap.c`.

`LUA_32BITS` in `lua_pico_conf.h` does not take effect. `luaconf.h` redefines
it to 0, so integers are 64-bit and numbers are doubles. The RP2350 has no
double FPU. That width is recorded in `docs/lua-net-fs.md`. It is not a
change this note proposes.

### The arena, and the TLS pool next to it

`src/lua/lheap.c` is 5,436 bytes of flash and 159,853 of SRAM.

| Symbol | Bytes | Role |
|---|---:|---|
| `arena` | 118,784 | The Lua heap. Address-ordered first fit over one static block, so a runaway script hits `out of memory` instead of starving lwIP or BTstack. Link-time visible on purpose |
| `pool_mem` | 40,960 | The HTTP client's TLS sessions. `LHEAP_POOL_KB` is `CONFIG_NET_TLS_POOL_KB` when TLS is on, and 0 otherwise. The comment in `lheap.c` calls it out as apart from the arena |
| bookkeeping (`g_main`, `g_pool`, the rest) | 109 | Allocator state |

The 32 KB source buffer the comment near `DRAFT_MAX` in `lrt.c` still
describes is gone. `src_buf` is a pointer. `src_reserve()` takes the bytes
from the top of the arena for the duration of a load and releases them on
the next reservation or the next tick. A static 32 KB there had left the SD
build with about 7 KB of C heap, and the CYW43 driver's boot malloc then
panicked. There is no 32 KB BSS object to remove.

### Bindings, `src/lua`

51,924 bytes of flash. 31,982 bytes of SRAM once the arena and `pool_mem`
are set aside. `pex/lua/pexlua.c` is another 4,629 flash and 13 SRAM. The
164,428 / 191,852 Lua bucket is exactly those three: liblua 107,875/4,
`src/lua` 51,924/191,835, `pexlua.c` 4,629/13.

| File | Flash | SRAM | The SRAM is |
|---|---:|---:|---|
| `lrt.c` | 12,545 | 8,770 | Eight script slots, the module-use table, the cache queue, one directory walk |
| `lbind.c` | 12,246 | 12,340 | Websocket/JSON scratch and the registered-op table |
| `lheap.c` | 5,436 | 159,853 | Arena + TLS pool, above |
| `lfile.c` | 5,528 | 584 | |
| `lhttp.c` | 4,988 | 936 | |
| `lbind_proto.c` | 4,529 | 5,068 | Anker TLV parse scratch and two reassembly channels |
| `lops.c` | 2,806 | 3,920 | Four static chunk/escape buffers, summing to the whole file |
| `ljson.c` | 2,806 | 84 | |
| `lwait.c` | 704 | 280 | Includes a 32-byte `links` of its own, unrelated to the LE table |
| `lfile_path.c` | 336 | 0 | |

Named binding objects:

| Object | Bytes | |
|---|---:|---|
| `scripts[8]` | 2,144 | 268 bytes a slot: name, 160-byte error, coroutine and env refs, and the BLE addresses that slot opened (`owned[BT_MAX_LINKS][6]`) |
| `s_mod_use[24]` | 1,440 | Which script slots still require a cached module. Name 48, users bitmask, tier, hash |
| `s_cache_q[32]` | 1,536 | Module names waiting to be compiled into `cache/` |
| `w` in `cache_scan` | 2,572 | `walk_t`: 8 directory names and 32 file names. Function-local static so the walk is off the stack |
| `out` in `lbind.c` | 4,096 | `WS_TX_MAX`. One call result, consumed before the call returns |
| `enc_buf` | 3,840 | `WS_TX_MAX - 256`. The JSON value inside that frame |
| `lua_ops[24]` | 1,152 | Registered Lua ops. 48 bytes each (name 40, ref, owners) |
| `s_items[96]` | 3,072 | `anker_tlv_item_t` is 32 bytes linked. The comment in `lbind_proto.c` still says ~40 and nearly 4 KB |
| `s_reasm[2]` | 1,292 | Two `anker_reasm_t`. 646 bytes each, a 640-byte payload plus the opcode and the fragment mask |
| `s_scratch` | 704 | `ANKER_REASM_MAX + 64` |

`lops.c`'s four buffers (`esc` 1,400, `chunk` 1,024, `src` 896, `chunk` 600)
sum to its entire 3,920 bytes of SRAM.

The Anker TLV and reassembly buffers are binding scratch for the Lua codec
helpers. They are not the interpreter, and they are not the PEX pool.

## What to switch

The non-SD Classic image already links with TLS on: 43,448 bytes of slot
spare, heap gap 17,560. The image that misses is SD + W5500 + Classic, and
only that defconfig has to give something up. The product defconfig can
keep every option below. None of this has been applied, and none of it has
been rebuilt. Savings are the measured marginal costs in this note.
A combination that lands within 2 KB of either hole is not a fit. The next
small feature, or the alignment the row sums leave out, would miss again.

The hole to clear, with daylight:

| | Minimum | Comfortable |
|---|---:|---:|
| Flash back | 45,340 (just links) | about 55,000, so the slot still has ~10 KB |
| SRAM back | 11,504 (gap of 8,192) | about 16,000, so the gap is ~12 KB |

Growing the slot does not create SRAM. Removing SD and FatFs (49,666 flash,
17,807 SRAM) clears both holes and removes the feature that defconfig exists
to build. The W5500 alone (13,172 flash, 798 SRAM) does not.

### Use the switches that already exist

| Kconfig, set to n | Flash | SRAM | What that image loses |
|---|---:|---:|---|
| `NET_HTTP_TLS` | 64,929 | 40,960 | `https://` from Lua. Plain `http.start` stays. The web app is already HTTP. mbedTLS here is already a TLS 1.2 client only (ECDHE, AES-GCM, P-256/P-384). The help's "~70 KB" is this flash number. The SRAM is `NET_TLS_POOL_KB`, whose measured session peak is 31–33 KB |
| `NET_WIREGUARD` | 34,751 | 1,688 | The tunnel. Help text says ~25 KB and a few KB of RAM; this map is 34,751 and 1,688 |
| `NET_MDNS` | 27,364 | 5,338 | `<hostname>.local`. The five mDNS sources are already behind the option. `include/lwipopts.h` still defines `LWIP_MDNS_RESPONDER` as 1 with no `#if`, so confirm that a build with the option off still links before counting on it |
| `NET_USB_NCM` | 10,541 | 17,183 | The USB gadget. This is the recovery path when Wi-Fi is wrong. `src/net/cyw43_wait.c` calls `tud_task()` with no `#if`, and CMake only links TinyUSB when NCM is on, so the option does not compile until that pump is stubbed |
| `NET_ETH_W5500` | 13,172 | 798 | The jack. Help text says ~12 KB and ~1 KB; the driver files are 7,780/798, and `acd`/`autoip` plus the extra SPI code make the flash 13,172 |
| `PROTO_ANKER` | 16,658 | 5,068 | Lua TLV scratch (`lbind_proto.c` 4,529/5,068) and the C codec (12,129/0). `pico2w_noanker_defconfig` already does this. The Anker board needs it |
| `NET_HTTP_CLIENT` | client 16,705, plus TLS if it was on | client ~2,550, plus the 40,960 pool | `http.start` entirely. TLS depends on this option, so turning the client off drops mbedTLS too |

`NET_WIFI` only removes `wifi.c` and `wifi_ops.c` (6,476/3,430). The CYW43
driver stays for Classic and BLE. `BT_CLASSIC` depends on `BT_LE`, and
`ENABLE_BLE` in `include/btstack_config.h` is unconditional, so Classic
without BLE is not a switch this tree has.

Two defconfigs cover the feature, both leaving SD, Wi-Fi, BLE, Classic,
Lua source, the 116 KB arena, and the 48 KB PEX pool in place.

**SD + Classic, HTTPS off.** One line on `configs/pico2w_sd_classic_defconfig`:
`# CONFIG_NET_HTTP_TLS is not set`. Same tradeoff `pico2w_sd_bnep_defconfig`
already documents. From this map, not a rebuild: load becomes 865,147
(19,589 spare) and `__end__` moves to `0x20076cf0` (gap 37,648). That is
the combination with daylight, and it keeps USB-NCM, WireGuard, mDNS, and
the W5500.

**SD + Classic, HTTPS kept.** Turn off USB-NCM, WireGuard, and mDNS, after
the `tud_task` stub and the lwipopts check. Flash back is 10,541 + 34,751 +
27,364 = 72,656 (27,316 spare). SRAM back is 17,183 + 1,688 + 5,338 = 24,209,
which puts the gap near 21 KB. The W5500 stays. USB-NCM plus WireGuard
without mDNS is 45,292 bytes of flash against a 45,340 hole. That is the
dead heat to avoid.

### One new switch worth adding

`CONFIG_LUA_COMPILER`, default y. Off, stop compiling `lparser`, `lcode`,
`llex`, and `ldump`: 7,814 + 6,056 + 3,468 + 810 = 18,148 flash, no SRAM.
Keep `lundump` so `cache/*.luac` still loads. On that image `script.start`
of source, the editor's Run button, and `require` of a `.lua` file stop
working. A host would compile to bytecode first. It does not close this
hole by itself. It is the Lua cut that pays. The coroutine library, the
string library, the base library, and `ldebug` stay: the scheduler's
instruction hook lives in the debug code, and the profiles are coroutines.

The other libraries are scraps (math 2,444, utf8 1,541). A Kconfig per
library is more machinery than the bytes.

### The Lua arena, once the drivers are PEX

`CONFIG_LUA_HEAP_KB` is already the switch. Each step is 1,024 bytes of
`.bss` and no flash. The Kconfig default is still 160. The SD image sets
116. The help text says the cost "is bss, which this part has to spare."
On the SD + Classic link that sentence is no longer true.

The 116 KB number pays for the Lua Anker profile, not for a launcher.
`src/lua/Kconfig` records it: compiling the 28 KB A2687 profile peaks at
82 KB and sits at about 14 KB once resident; 96 KB fails as soon as another
script is also loaded; the live stack (driver, link, schema, one model
table) is about 74 KB; 128 KB still fragments because a report builds ~68
TLV tables a few times a second. `profiles/anker/run.lua` is 28,575 bytes
and is what those numbers describe.

The launchers that start the PEX drivers do not load that profile:

| Script | Bytes | What it does |
|---|---:|---|
| `lua/pinecil.lua` | 397 | `pex.load` / `pex.start` of `pinecil` |
| `lua/anker2.lua` | 553 | same `pex.start('anker')`; the module owns both slots |
| `lua/anker.lua` | 557 | same, for `anker` |
| `lua/ac_pulse.lua` | 5,556 | still a Lua script |
| `lua/profiles/pinecil.lua` | 7,231 | still a Lua profile |
| `lua/profiles/anker/run.lua` | 28,575 | the profile the 82 KB compile peak was measured on |

`anker.lua` and `pinecil.lua` never `require` the profile tree. The TLV
tables that fragmented the arena move into the PEX module, whose RW is the
48 KB `PEX_POOL_KB` block. That pool stays. Shrinking the arena does not
shrink it, and setting the pool to 0 puts those bytes on the C heap.

On the failed image the arena is the whole SRAM lever this workload can
give. `__end__` is 3,312 bytes past RAM, and the 8,192-byte gap floor needs
11,504 bytes back. Flash stays 45,340 over either way.

| `LUA_HEAP_KB` | Freed vs 116 | Gap on the failed image |
|---:|---:|---:|
| 116 | 0 | none |
| 104 | 12,288 | 8,976, just over the floor |
| 100 | 16,384 | 13,072 |
| 96 | 20,480 | 17,168 |
| 64 | 53,248 | 49,936 |

104 KB is the SRAM floor and no more. 100 KB is the first step with the
same daylight asked of the other cuts. 96 KB is the size that already
failed for the Anker profile and has not been measured for the launchers.
A still-smaller arena is plausible for two half-kilobyte starters and is
not a number this map can pick. Compiler overhead is not linear in file
size, and first-fit fails on the largest hole, not on bytes used.

Pick the number from a running image, not from the file sizes. `script.stats`
reports arena use and the largest free block without a collection.
`script.heap` reports the same after a full collection. Start `anker.lua`
and `pinecil.lua` together, and any other launcher that image still runs,
and read both ops at the compile and again once they are resident. Set the
arena a step above the peak, not at it. The image that must still Run
`profiles/anker/run.lua` in the editor keeps 116 KB or the 160 KB default.
The SD + Classic defconfig, if its Lua is only the launchers, takes the cut.

The arena does not close the flash hole. Pair it with one of the flash
switches above. TLS off already leaves a 37,648-byte gap, so the arena cut
is optional there. Keeping HTTPS, the arena at 100 KB plus WireGuard and
mDNS off is 62,115 bytes of flash back (16,775 spare) and the SRAM gap
from the arena alone is already 13,072. USB-NCM and the W5500 stay. That
recipe still wants the `LWIP_MDNS_RESPONDER` check before it is trusted.

### Other flash, after the interface switches

The rows above are the cuts that can clear 45,340 bytes. What follows is
everything else in the failed SD + Classic link that is still large enough
to talk about. Sizes are placed bytes in that map (`.text` + `.rodata` +
initialized `.data`). A file's own log strings can add a little more,
because identical literals fold into the pools named at the top of this
note. None of this has been rebuilt, and none of it is applied.

| Piece | Flash | Switch today | What goes with it |
|---|---:|---|---|
| `CONFIG_OTA` | 10,886 | Already exists. Default n. The SD defconfig sets it y | Field update over UDP 1337, the A/B try-before-you-buy path, and the MCUmgr os/stat/fs groups. Also 10,492 bytes of `.bss` (`img_mgmt` 5,030, `smp_udp` 3,220, `fs_mgmt` 1,680, the rest small). That SRAM lands a gap of 7,180, about 1 KB under the 8,192 floor, so it is not a fit on either hole. `uECC` (22,104) stays: BLE already links it, and it checks module signatures |
| `CONFIG_NET_FORWARD` | 7,356 | Exists. The measured image linked it because the default was y. The default is now n | `net.forward{}`: a script's TCP proxy and UDP relay. The launchers that only `pex.start` do not call it. This is not Share internet |
| `src/util/inflate.c` | 6,970 | None. `CMakeLists.txt` always compiles it | Gzip decode of the web assets. Static GET bodies are gzip. Storing them uncompressed would drop this and spend the bytes on LittleFS instead, which is the other partition |
| `CONFIG_FS_MIGRATE` | 6,700 | Added after this measurement, default n. The measured image compiled the file unconditionally | The one-time pre-PEX filesystem move. An old geometry still mounts, and `sys.info` says it is not in this build. Set the option for the boot that moves it |
| I/O bring-up | 3,396 | `CONFIG_IO_PINS` and the GPIO/ADC/I2C children. The SD defconfig sets them y. The Kconfig comment already says a deployed build can drop this | `pins.c` 1,092 and `io_ops.c` 2,304. The SPI bus (`spibus.c` 3,604 + `spibus_pico.c` 1,148) stays while the W5500 stays |
| `sscanf` | 6,814 | None | `svfscanf` is 6,326 of it. Call sites run through the filesystem, HTTP, the Bluetooth ops, and the Lua bindings |
| `pico_printf` | 6,140 | None | Formatted output is everywhere. Replacing the implementation is its own project, and formatted output would keep part of the 6 KB |

A fresh configure of the SD + Classic defconfig now leaves forwarding and
the migration out, 7,356 + 6,700 = 14,056 bytes against the 45,340 hole.
The image that produced this map still has both. An existing build
directory keeps the `NET_FORWARD=y` it saved in `.config` until that file
is regenerated.

Two compiler experiments are unmeasured, so they are not budget lines.
The interpreter core left at `-O3` (`lvm`, `ldo`, `lgc`, `ltable`, `lapi`,
`lstring`, `lobject`, `ltm`, `lstrlib`) is 67,756 bytes placed. CMake keeps
that path at `-O3` on purpose; the compiler half and the cold libraries are
already `-Os`. Moving the core would save some fraction of 67,756, and only
a rebuild would say how much. Link-time optimization is not turned on.

The double helpers actually in this image sum to 20,076: Pico's
double-coprocessor glue (`double_sci` 4,562, `double_math` 3,168,
`double_aeabi_dcp` 922, `double_conv` 684), newlib's decimal conversion
(`strtod` 4,468, `mprec` 2,908, `gdtoa-gethex` 1,944, `gdtoa-hexnan` 532),
and `_arm_addsubdf3` 888. There is no larger libgcc soft-double pile beside
those. `LUA_32BITS` is set in the Lua cmake and then forced back to 0 by
`luaconf.h`, so it is not a switch, and integer width is the wrong lever
for a slot.

Left in place on purpose:

| Piece | Flash | Why it stays |
|---|---:|---|
| LittleFS | 14,616 | `cfg`, scripts, web assets, and PEX modules live there |
| rijndael | 7,980 | Classic and LE pairing. 5,120 of it is the AES tables. The crypto files are the ones CMake refuses to drop to `-Os` |
| CYW43439 Bluetooth firmware | 6,970 | `cyw43_btfw_43439` in `cybt_shared_bus.c`, downloaded to the radio at boot. The 256 KB Wi-Fi blob is already outside the slot |
| Uplink | 6,232 | `CONFIG_NET_UPLINK` has no prompt. It defaults on with Wi-Fi or the W5500, and it is the only caller of `netif_set_default()` |
| SDP server | 1,688 | Compiled with Classic. A client-only image could drop local records later. Too small to move the hole |
| DHCP server | 1,884 | Stays while Wi-Fi, USB-NCM, or the W5500 stays |
| SNTP | 1,332 | `CONFIG_NET_SNTP`, default y. `sys.time`, and certificate dates while TLS stays |

### Leave these at the measured sizes

| Knob | Why it stays |
|---|---|
| `PEX_POOL_KB` 48 | Installed modules execute in place and keep their RW here. The drivers that left Lua land here. `0` takes the pool from the C heap instead, and the heap gap is what `pico_malloc` panics on. Moving the 48 KB off `.bss` makes the link succeed and the boot fail |
| `NET_TLS_POOL_KB` 40, if TLS stays | Measured session peak 31–33 KB. The pool is the reason HTTPS survived a fragmented arena |
| `LWIP_MEM_SIZE_KB` 32 and `PBUF_POOL_SIZE` 16 | Already the measured cut. `include/lwipopts.h` records peaks of 16.8 KB and 6 pbufs, and records an earlier `.bss` growth that panicked in `km_ipc_init` with no network frame in the backtrace |
| `BT_MAX_LINKS` 4 | The feature is several Classic sessions at once. Each LE slot is 5,052 bytes, not the ~1 KB the help text describes |
| `GATT_MAX_CHRS` 160 and the other GATT caps | A Pinecil V2 needs the characteristic cap. The bytes are per live link |
| `BT_ACL_PAYLOAD` 1021 | Classic RFCOMM. NAP is the build that raises it to 1695, and NAP is off here |
| `HTTP_CONNS` 8 | A `#define` in `http.c`, not Kconfig. Each connection is 3,016 bytes of SRAM on this image, so 8→4 would free about 12 KB of SRAM and no flash. The web UI holds a socket open beside asset fetches. It does not close the flash hole |

`SD_XFER_BUF`, `SD_XFERS`, and `SD_WORKER_STACK` are already Kconfig. The
two transfer rings are 8,816 bytes and the worker stack is 4,096. Halving
them is a few KB of SRAM and none of the flash. They are a tuning knob for
an image that is already close, not the way across a 45 KB gap.
