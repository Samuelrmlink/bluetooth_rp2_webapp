# PEX — portable executable modules for microcontrollers (DRAFT 0.1)

Status: **planning draft 0.5** (0.4: two code models; 0.5: the memory model, §7.2) -- format and loader not implemented yet; all of Q1-Q9 decided; the PEX flash partition exists on the gateway (§14, done 2026-10-21). Decisions marked **[D]**,
open questions **[Q]** (collected at the end).

## 1. Why

Lua on the gateway is out of RAM. The iTi Updater backend alone keeps ~55 KB of
bytecode resident in a 176 KB arena, and its code is loaded and dropped per
job — which fragments the arena. The same logic as Thumb machine code would
be a fraction of the size, would run 20-100x faster, and could execute **in
place from flash**, costing no RAM at all for code.

A `.pex` file is a precompiled, relocatable code module — the microcontroller
equivalent of an `.exe`/`.so`, in the spirit of the Flipper Zero's `.fap` —
that a firmware loads at run time from LittleFS or the SD card, links against
the firmware's own exported API, and calls. Typical uses:

- **native Lua modules**: `require 'otap'` returns a table of C functions;
  the Lua scripts keep the policy, the module carries the byte-heavy work
  (codecs, OTAP, JSON shaping, crypto, parsers);
- **native scripts/apps** with no Lua at all (a protocol driver, a profile);
- **moving optional features out of the firmware image**: the SD build has
  ~22 KB left in its 864 KB slot.

Non-goals (v1): RISC-V targets (ids reserved, ARM only for now); sandboxing (a PEX runs with the firmware's full privilege —
trust comes from signatures, §10); C++ exceptions/RTTI; dynamic linking
between PEX files; running the same file on several architectures.

## 2. Prior art and what we take from it

- **Flipper `.fap`**: a relocatable ELF (`-r`) plus a `.fapmeta` manifest;
  the firmware relocates the ELF at load time and resolves imports against a
  hashed, versioned API table (`api_symbols.csv`, API version major.minor).
  We take: the **versioned firmware API table resolved by hash**, the
  manifest. We do **not** take the in-firmware ELF loader: parsing sections,
  symbols and a dozen relocation types on the device costs code, RAM and
  time. A PEX is **pre-linked on the host** into one image with a tiny
  relocation list, so the device loader is a few hundred lines.
- **Mach-O / PE**: a fixed header, then load commands/sections by table — so a
  loader can skip what it does not know (forward compatibility).
- The gateway's **OTA images**: ECDSA P-256 over SHA-256, key already baked
  into the firmware (`src/ota/ota_pubkey.h`). Reuse for PEX signing.

## 3. Terms

- **Image**: the loadable bytes of one module (code + read-only data + data).
- **Target**: an architecture/ABI combination (§5). One `.pex` = one target. **[D]**
- **Host API**: the functions and data the firmware exports to PEX modules,
  identified by name hash, grouped and versioned (§7).
- **Loader**: the firmware component that validates, places, relocates,
  links and starts a PEX.

## 4. File layout

All multi-byte fields little-endian. Offsets are from the start of the file.
Every structure is 4-byte aligned.

### 4.1 Fixed prefix (first 16 bytes) — never changes across format versions **[D]**

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | `'P' 'E' 'X' 0x1A` |
| 4 | 2 | `format_major` | **the version comes right after the magic** — a loader rejects an unknown major |
| 6 | 2 | `format_minor` | minor = backward-compatible additions; unknown fields/sections skipped |
| 8 | 2 | `header_size` | bytes of the full header (§4.2), so later minors can grow it |
| 10 | 2 | `target` | target id (§5) |
| 12 | 4 | `file_size` | total bytes incl. signature |

A loader reads these 16 bytes first and can refuse a file (wrong magic,
major, target, size) before reading anything else.

### 4.2 Header (format 1.0, after the prefix)

| Off | Size | Field | Notes |
|---|---|---|---|
| 16 | 4 | `target_features` | bits: FPU-SP, FPU-DP, DSP, CMSE-secure, Hazard3 Zcb/Zcmp, ... required by the code |
| 20 | 2 | `api_major` | host API major it was built against (§7) — must equal the firmware's |
| 22 | 2 | `api_minor` | must be <= the firmware's |
| 24 | 4 | `flags` | `PIC` (code model B, §6.2: text+ro are position-independent, RW is r9-relative), `XIP_OK` (code may run from flash), `RAM_ONLY`, `LUA_MODULE`, `APP`, `REENTRANT` (with `PIC`: several instances may share one copy of the code), `NEEDS_SIG` |
| 28 | 4 | `module_version` | the module's own version, `major<<24 \| minor<<16 \| patch` |
| 32 | 4 | `image_size` | bytes of the image as loaded (text + rodata + data, contiguous) |
| 36 | 4 | `text_size` | executable part; starts at image offset 0 |
| 40 | 4 | `ro_size` | read-only data, after text |
| 44 | 4 | `data_size` | initialised RW data, after ro (copied to RAM even for XIP) |
| 48 | 4 | `bss_size` | zeroed RW data, after data, not stored |
| 52 | 4 | `stack_hint` | worst-case stack use of any entry (from `-fstack-usage`), for the loader's checks |
| 56 | 4 | `entry` | image offset of the module entry (§8), Thumb bit set |
| 60 | 4 | `section_count` | entries in the section table |
| 64 | 4 | `section_table_off` | file offset |
| 68 | 4 | `image_off` | file offset of the image bytes |
| 72 | 4 | `image_crc32` | quick integrity check (signature is the real one) |
| 76 | 16 | `build_id` | first 16 bytes of SHA-256 of the image, for caches and logs |

### 4.3 Section table

Each entry: `u16 type, u16 flags, u32 offset, u32 size`. Types (v1):

| Type | Name | Contents |
|---|---|---|
| 1 | `RELOC` | relocations (§6) |
| 2 | `IMPORTS` | host API symbols this module needs (§7) |
| 3 | `EXPORTS` | symbols the module offers (Lua functions, app entry points) |
| 4 | `META` | TLV manifest: name, description, author, URL, license, icon, min RAM, Lua module name, ... |
| 5 | `DEPS` | (reserved for later) other PEX modules required |
| 6 | `DEBUG` | optional: symbol map / line table; never loaded on device, read by tools and by the crash reporter on a host |
| 0xF0 | `SIG` | signature block (§10); always last in the file |

Unknown section types with the `OPTIONAL` flag are skipped; unknown types
without it make the loader refuse the file.

### 4.4 Multi-target ("fat") files -- later **[D, Q8]**

v1 files hold one target. A later minor revision may add a container -- a
fixed prefix with `target = 0xFFFF` and a slice table (`target, offset, size`)
of complete single-target PEX files. A loader then reads only the slice for
the running architecture: **code for another architecture is never copied
into executable memory or the XIP partition**, and the other slices are
never read beyond their table entry.

## 5. Targets — one build per architecture **[D]**

| id | Target | CPU / ISA | ABI | Chips |
|---|---|---|---|---|
| 0x0001 | `armv6m` | Cortex-M0+ (Thumb-1 + some Thumb-2) | AAPCS, soft float | RP2040 |
| 0x0002 | `armv8m-main-fp` | Cortex-M33 (Thumb-2, DSP, FPv5-SP) | AAPCS, **softfp** (FP in core regs at call boundaries) | RP2350 (as the gateway builds today: `-mcpu=cortex-m33 -march=armv8-m.main+fp+dsp -mfloat-abi=softfp -mcmse`) |
| 0x0003 | `armv8m-base` | Cortex-M23 | AAPCS, soft float | (RP2350 has none; reserved) |
| 0x0004 | `armv7em-fp` | Cortex-M4F | AAPCS, softfp | HC32F460 (KM003C) |
| 0x0101 | `rv32imac` | Hazard3 (RP2350 RISC-V mode) | ilp32 | RP2350 |
| 0x0102 | `rv32imac-zcb-zcmp` | Hazard3 with Zcb/Zcmp | ilp32 | RP2350 |

Rules:

- **The target must match exactly** **[D, Q1]**: an M33 firmware loads only
  `armv8m-main-fp` modules, an RP2040 firmware only `armv6m` ones -- no
  "Thumb-1 runs on v8-M too" fallback.
- **v1 is ARM only** **[D, Q2]**: the RISC-V ids stay reserved.
- The short arch name is part of the file name **[D]**:
  `<module>.<arch>.pex`, e.g. `otap.m33.pex`, `otap.m0p.pex`.

| target id | short name (file name) |
|---|---|
| `armv6m` | `m0p` |
| `armv8m-main-fp` | `m33` |
| `armv8m-base` | `m23` |
| `armv7em-fp` | `m4f` |
| `rv32imac`, `rv32imac-zcb-zcmp` | `rv32`, `rv32zc` (reserved) |

  The header's `target` field is authoritative; the name only lets a loader
  pick the right file without opening the others.
- FP across the host API is always passed in core registers (softfp/soft) so
  the API is identical for M0+, M33 and M4 builds. **[D]**
- Chip-specific code (PIO, DMA channels, ...) goes through the host API, not
  register pokes, so the same `armv8m-main-fp` module runs on any RP2350 board.

## 6. Code models: relocated or position-independent **[D]**

A PEX is **never built for a fixed address**. The format supports two code
models; both produce an image that can be placed at any (4-byte aligned)
address. The header's `PIC` flag says which one a file uses.

| | A. relocated at install | B. position-independent (ROPI + RWPI) |
|---|---|---|
| `flags.PIC` | 0 | 1 |
| what is patched at placement | every absolute word in the image (code/ro list + data list) and the GOT | **nothing in code/ro**; only the RW block (data list, GOT), which is in RAM |
| the same flash bytes valid elsewhere | no -- moving/compacting rewrites them | **yes** -- move, copy, compact freely |
| several instances, one copy of code | no | **yes** (one RW block per instance, `REENTRANT`) |
| signature / build id covers | the image before patching | exactly the bytes that run |
| register cost | none | r9 reserved as the RW base (static base) |
| code size / speed | best | ~3-8% larger, slightly slower (more on M0+: r9 is a high register) |
| toolchain | GCC or clang | clang (`-fropi -frwpi`); GCC only where it is proven (see §11) |
| restrictions | none beyond §6.1 | no static initialisers in RW that point into code/ro (pexlink reports them; initialise at run time) |
| preferred for | RAM loading, GCC builds, hot code | modules installed into the PEX flash partition |

### 6.1 Model A: relocated at install

- Built with the normal toolchain (`arm-none-eabi-gcc -mthumb -mcpu=... -Os
  -ffunction-sections -fdata-sections -fno-common`), linked with a PEX linker
  script that places text, rodata, data, bss **contiguously from address 0**
  and keeps relocations (`ld -q`/`--emit-relocs`).
- `pexlink` (host tool, §11) then:
  - resolves everything internal at link time (all `BL`/`B` and PC-relative
    loads inside the image are already final -- they move with the image);
  - turns the remaining **absolute 32-bit references to the image itself**
    (`R_ARM_ABS32` from rodata/data pointer tables, vtables, function pointers)
    into the `RELOC` lists;
  - turns references to host symbols into **GOT** entries (§7);
  - **refuses** anything else (`MOVW/MOVT` absolute pairs, `R_ARM_THM_CALL` to
    an undefined symbol, TLS, ...), with a message saying which compiler flag
    avoids it (e.g. `-mslow-flash-data`).
- `RELOC` holds two lists of `u32 image_offset` of 32-bit words to which the
  loader adds a base: `RO` words (in text/ro, base = where the code is placed;
  patched before the image is written to flash or RAM) and `RW` words (in
  data, base = code base for pointers into code/ro, RW base for pointers into
  data -- each entry carries which: bit 31 of the offset = "points into RW").

### 6.2 Model B: position-independent (ROPI + RWPI)

- Built with clang: `--target=thumbv8m.main-none-eabi -mcpu=cortex-m33
  -mfloat-abi=softfp -fropi -frwpi` (M33) or
  `--target=thumbv6m-none-eabi -mcpu=cortex-m0plus -fropi -frwpi` (M0+), same
  `-ffunction-sections -fdata-sections -fno-common`; linked with the PEX
  linker script for model B: text+ro from address 0, RW (data+bss+GOT) as a
  separate segment also from address 0.
- **ROPI**: code and read-only data reach each other PC-relative, so text+ro
  is valid at any address with **no patching** -- the `RO` reloc list must be
  empty (pexlink refuses the file otherwise).
- **RWPI**: all writable data, and the GOT of host imports (§7), is reached
  relative to **r9** (the static base). The loader allocates one RW block per
  instance, copies `.data`, zeroes `.bss`, applies the `RW` reloc list (only
  words in RW; pointers into code/ro get the code base) and fills the GOT.
- **r9 rule**: every entry into a model-B module -- `pex_main`, exported Lua
  functions, callbacks the module registered -- goes through the host's export
  trampoline (§8), which saves the caller's r9, loads the instance's RW base
  into r9, calls, and restores r9. The firmware itself is built normally (r9 is
  an ordinary callee-saved register in AAPCS, so saving/restoring it is
  enough); host API functions called *from* a module must not depend on r9 and
  must preserve it (they are ordinary AAPCS functions: they do).
- Restriction: a static initialiser in RW pointing into text/ro
  (`static const char *names[] = {...}` that is not `const` itself,
  `static void (*fn)(void) = handler;`) needs a run-time fix-up under ROPI;
  clang emits it into an init routine or pexlink lists it in `RW` relocs --
  either way it never touches flash.
- The image's `build_id` and signature cover exactly the bytes that run, so an
  installed model-B module can be re-verified in place at any time.

### 6.3 Shared rules

- For both models the image starts at offset 0 of its placement and must be
  placed 4-byte aligned (8 if it contains `ldrd`/`strd` on 8-byte data); the
  header's `entry` and every export are image offsets.
- For RISC-V targets (later): model A with `R_RISCV_32` only; model B with
  `-fPIC`-style GP/`auipc` addressing. **[Q2: later]**

### 6.4 Where the image runs

| Mode | Code (text+ro) | RW (data+bss) | RAM cost | When |
|---|---|---|---|---|
| **XIP** (default) | written, relocated, into a reserved flash area and executed in place through XIP cache | RAM pool | data+bss only | installed modules |
| **RAM** | RAM pool | RAM pool | whole image | `RAM_ONLY` modules, timing-critical code, quick tests, chips without spare flash |

- XIP needs a flash region the loader owns: on the gateway's 4 MB part there
  is none free today (Main A/B 864 KB each, Wi-Fi 256 KB, LittleFS 2032 KB), so
  **carve a `PEX` partition out of LittleFS** (e.g. 256 KB) in
  `partitions.json` **[D, Q3]** -- after a full backup, see §14. The loader keeps an index there (build id ->
  placement), so a module installed once boots from flash with no copy.
- Model A code is relocated to its flash address when installed; moving it
  (reinstall, compaction) rewrites it. Model B code is copied as-is and can be
  moved by a plain copy; only its RW block (RAM) is set up per load. Flash
  writes stop XIP briefly (the firmware already handles this for LittleFS and
  OTA).
- RP2350: **PEX code runs in the Secure world, like the rest of the firmware**
  **[D, Q4]** (a later format revision may move it to Non-secure behind CMSE
  veneers -- the `target_features` CMSE bit is there for that). Code in SRAM
  needs the region executable (ACCESSCTRL/MPU allow it by default for Secure);
  keep XN on the Lua arena regardless.
- The RW pool is separate from the Lua arena (so PEX data never fragments
  Lua) and sized in the firmware config.

## 7. The host API: imports, versioning, the GOT

- The firmware exports a table of **host symbols**: functions and data
  (`lua_pushinteger`, `bt_write`, `fs_read`, `malloc`/`free` of the PEX pool,
  `memcpy`, `printf` to the log, `sys_ms`, ...), grouped into **API groups**
  (`core`, `lua`, `bt`, `net`, `fs`, `sd`, `crypto`, `io`), each with its own
  `major.minor`, plus an overall `api_major.api_minor` in the PEX header.
- Identification: `IMPORTS` lists `u32 name_hash` (FNV-1a 32 of the symbol
  name) + `u16 group` + `u16 flags` (`WEAK`: may be absent -> GOT entry 0).
  The host table is sorted by hash; collisions are rejected at firmware build
  time. Names are kept in `DEBUG` for tools/messages.
- Linking: each import gets a GOT slot in the module's RW data; code calls
  through the slot (`pexlink` emits a 2-instruction veneer per import:
  `ldr r12, [pc, #got]` ... `bx r12`, or the compiler's
  `-mlong-calls`/`__attribute__((pex_import))` form). The loader fills the slots.
- Compatibility: a minor bump only **adds** symbols; a major bump may change
  or remove them. The firmware refuses `api_major` mismatches and
  `api_minor` > its own, and lists the missing symbol by name if `DEBUG` is
  present.
- The **Lua C API** is the biggest group and the reason PEX exists; it is
  exported as-is (Lua 5.4.8, the firmware's `luaconf.h`: 64-bit integers
  today), so the `lua` group's major follows the Lua version and number
  configuration. **[Q5]**

### 7.1 Thin abstraction layers (the "HAL groups") **[D]**

As with the Flipper's `furi`/`furi_hal`, a module never calls the pico-sdk or
pokes registers: it calls **thin, chip-neutral wrappers** the firmware
exports, each a small versioned group. Thin = a direct call into the existing
firmware code with stable, plain-C types (fixed-width ints, `const uint8_t *`
+ length, opaque handles), no allocation behind the module's back, errors as
negative `PEX_E*` codes. Groups planned for v1:

| group | covers | notes |
|---|---|---|
| `core` | version info, `sys_ms`/`us`, `sleep`/yield, log, panic-free asserts | |
| `mem` | lifetime-scoped memory (§7.3): the module's long-term heap, per-call/per-job scratch arenas, fixed-size pools; `malloc/free` as a fallback; `memcpy`-family | never the Lua arena |
| `sched` | cooperative tasks/timers on the main loop, deferred calls, events | the firmware is single-loop; no RTOS assumed |
| `gpio` | pin claim/release (the existing `io` pin registry), dir, read/write, IRQ callback | refuses pins owned by SD/W5500/CYW43 |
| `i2c`, `spi`, `uart` | bus claim, transfers (blocking-with-yield and async+callback) | shares the firmware's bus arbitration (`spibus`) |
| `pio`, `dma` | claim a state machine/channel, load a program, FIFOs, IRQs | RP2040/RP2350 only; absent on other chips (weak imports) |
| `timer`, `pwm`, `adc` | | |
| `fs` | LittleFS + SD with the `file` semantics of the Lua API (`lfs:/`, `sd:/`) | |
| `net` | the HTTP(S) client, sockets later | |
| `bt` | the same operations as Lua's `bt.*` (scan table, connect, GATT, notify) | |
| `crypto` | SHA-256, AES, ECDSA verify, RNG | |
| `bus` | publish `profile.*` events, register ops (the Lua `bus`/`ops` equivalents) | so a native module can serve the web UI directly |
| `lua` | see 7.2 | |
| `pex` | load/unload/ref, introspection | |

Each group is its own `major.minor`; a module imports only the groups it uses,
and a firmware may omit groups (a build without SD has no `sd_*` symbols: weak
imports become 0 and the module checks `pex_has(group)`).

### 7.2 Memory: separate by lifetime, no handles **[D]**

Fragmentation on the gateway came from short-lived allocations landing between
long-lived ones in one first-fit heap. The memory model therefore separates
allocations **by lifetime** instead of making blocks movable.

**Handles (pointer-to-pointer, movable blocks) are deliberately not part of
v1.** They cannot help the Lua arena (Lua's collector never moves objects),
everything DMA or an interrupt touches would have to stay pinned, and a raw
pointer kept past an unlock corrupts memory silently at the next compaction --
a worse failure than an honest "out of memory". A later `mem` minor version may
add them for the rare large, long-lived, resizing buffer if a real module needs
it.

Regions (on the RP2350 gateway, all outside the Lua arena):

| region | lifetime | how it is allocated and freed | who uses it |
|---|---|---|---|
| **module image RW** | the module's load | one block per instance (data+bss+GOT), freed whole at unload | every module (§6) |
| **long-term heap** | until the module unloads (or forever for the firmware) | `pex_alloc_long(size)` / `pex_free`; allocated from the **top** of the PEX RW pool, growing down | tables, caches, buffers kept between calls |
| **scratch arenas** | one call, one job, one request | `pex_scratch_begin(cap)` -> arena; `pex_scratch_alloc(a, size)` (bump pointer, no individual free); `pex_scratch_end(a)` frees all of it at once; allocated from the **bottom**, growing up | parsing, encoding, one transfer, one HTTP response |
| **fixed-size pools** | per object | `pex_pool_new(obj_size, count)` then `pex_pool_get/put`; equal slots cannot fragment | records, list nodes, chunk buffers |
| **malloc fallback** | any | `pex_malloc/free` from the long-term side | ported code that cannot be changed |

Rules:

- Long-term and short-term never interleave: long-term blocks come from one end
  of the RW pool, scratch from the other, and scratch arenas are LIFO (nested
  `begin`/`end` must end in reverse order -- checked, a violation is reported
  with the module's name). A finished job leaves the scratch side empty again.
- A module's long-term blocks are tagged with its id and all freed at unload,
  so a leak cannot outlive the module.
- Scratch arenas are also what a module hands to the host for temporary results
  (`fs_read_into`, HTTP bodies), so host and module never trade ownership of
  heap blocks.
- DMA/IRQ buffers are long-term or pool blocks (never scratch that may end while
  the hardware still writes) and are declared as such (`PEX_MEM_DMA`), which
  also places them where the cache/bus rules require.
- The same split applies inside the firmware: buffers kept for the system's life
  (TLS record buffers, the module cache, radio and SD buffers) are reserved
  early and kept; per-request buffers (HTTP bodies, TLS session state, SD job
  buffers, module load buffers) come from a separate short-term region; the Lua
  arena keeps long-lived objects (modules) and short-lived ones apart internally.
- Accounting: `pex_mem_stats()` per module (long-term bytes, pool use, scratch
  high-water mark) and for the pool as a whole (largest free run on each side),
  exposed in `sys.info`.

### 7.3 Lua from native code: full C API, curated API, or both? **[D, Q5: option C]**

**A. Export the whole Lua C API** (`lua_*`, `luaL_*`, as `lua.h`/`lauxlib.h`)

+ any existing C Lua library builds with no change; nothing to design;
+ zero overhead: a module calls straight into Lua;
- the module's binary compiles in **this firmware's `luaconf.h`**: the size of
  `lua_Integer` (64-bit today), `lua_Number` (double), `LUAL_BUFFERSIZE` and
  the layout of `luaL_Buffer` (lives on the module's stack), the stack limits,
  and every macro in `lua.h` (`lua_tonumber`, `lua_pop`, `lua_newtable`, ...
  expand into calls with this Lua's internals). So:
  - **switching the firmware to 32-bit Lua numbers** (the biggest single memory
    saving left: roughly a third of the arena) breaks every module -> new
    `lua` major, rebuild all;
  - **a Lua upgrade** (5.4 -> 5.5) breaks every module the same way;
- ~150 symbols in the host table (~1.2 KB of flash for the table; the
  functions are in the firmware anyway);
- a module can corrupt the Lua stack/state as easily as any C code can.

**B. A curated `pexlua` API** (~30-40 functions)

+ fixed-width types in the signatures (`int64_t`, `double` explicitly, or
  `int32_t` where that suffices), no macros that bake in internals, no
  `luaL_Buffer` on the module's stack -> **survives a switch to 32-bit numbers
  and a Lua upgrade** unchanged (the firmware's shim adapts);
+ can do what plain Lua C API cannot, cheaply: hand a Lua string's bytes to the
  module without a copy, build a byte string straight from a module buffer,
  register a whole function table in one call, check arguments with one call;
+ a smaller surface to keep stable and to document;
- has to be designed and maintained; each call is one more indirection (a few
  cycles -- noise next to what the module does);
- existing C Lua libraries need porting to it.

**C. Both** (recommended): `pexlua` as the normal, stable path that every
module should use, and the raw API as a separate group `lua54_<numbers>` (e.g.
`lua54_i64d`) for the rare module that needs something `pexlua` lacks -- a
module importing it is tied to that exact Lua build and is refused (with a
clear message) by any other. v1 ships `pexlua` first; the raw group can come
later if a real module needs it. This keeps the door open to 32-bit Lua numbers
without stranding modules.

## 8. Entry points and lifecycle

- Every call into a module goes through the host's **export trampoline**: it
  bumps the module's `active` count (§8 unload), and for a `PIC` module sets r9
  to that instance's RW base (§6.2) and restores the caller's r9 on return.
- `entry` points to `int pex_main(const pex_host_t *host, uint32_t reason, void *arg)`:
  - `reason = PEX_INIT` after linking, once per load: set up, register;
  - `PEX_LUA_OPEN` for a Lua module: push the module table (like
    `luaopen_<name>`), `arg` = `lua_State *`;
  - `PEX_APP_RUN` for an app (runs on the firmware's cooperative loop: must
    return or yield through the host API, same rules as a Lua script: no
    blocking, watchdog);
  - `PEX_DEINIT` before unload: free everything, cancel callbacks.
- `.data` is copied and `.bss` zeroed by the loader before `PEX_INIT`; static
  constructors (`.init_array`) run by the loader, in order, if present.
- **Unload is an explicit API** (`pex.unload(name)` from Lua, `pex_unload()`
  from C) **[D, Q6]**, and it is **refused while any of the module's code is
  on a call stack**, or while it still has live references:
  - *on a stack*: every call into a module goes through the host's export
    trampoline, which increments the module's `active` count on entry and
    decrements it on return; callbacks the module registered (timers, BLE,
    HTTP sinks) go through the same trampoline. As a safety net the loader
    also scans the stacks it knows (core0 main stack, each Lua coroutine's C
    stack, core1 if it runs PEX code) for return addresses inside the image
    and refuses if it finds one;
  - *references*: Lua closures/tables holding its C functions (a userdata
    anchor with `__gc` per module), registered callbacks, a running app --
    counted through `pex_ref/pex_unref` in the host API.
  A refused unload says why (which stack / which reference). A module loaded
  only to run once (an app) can be marked `UNLOAD_ON_EXIT`.
- Faults: a HardFault with the PC inside a PEX image is reported as
  `module name + build_id + offset` (symbolicated on a host with the `DEBUG`
  section), then the firmware's normal fault path.

## 9. Lua integration

- `require 'name'` searches, in order: `lua/name.lua`, `pex/name.pex`
  (LittleFS), `sd:/pex/name.pex`; a PEX hit is loaded (or found already
  installed by build id), linked, and `PEX_LUA_OPEN` returns the table.
- The Lua module's memory: its code is not in the arena at all (XIP); its C
  data is in the PEX pool; only the table and closures it pushes live in the
  Lua arena.
- Scripts keep working unchanged if a module ships as both `.lua` and `.pex`;
  the loader prefers the PEX for the running target, and falls back to the
  Lua source when no PEX for this target/API exists.
- First candidates from the iTi backend: `otap` (transfer engine + hex),
  `bleota`, `json`-shaping of catalog listings, `registry` index.

## 10. Integrity and trust

- `image_crc32` catches corruption cheaply; `SIG` is authoritative.
- `SIG`: `u16 alg` (1 = ECDSA P-256 over SHA-256), `u16 key_id`, then the
  signature over **every byte of the file before `SIG`**. Same key and tooling
  as OTA releases (`tools/ota_sign.py`), so a firmware trusts what its owner
  signs.
- Policy (firmware config) **[D, Q7]**: `signed-only` for production builds,
  `allow-unsigned` for development builds (the UI says so loudly). An unsigned
  module is never installed into the XIP partition of a `signed-only` device.
- No sandbox: a PEX can do anything the firmware can. Signing is the security
  boundary; say so in the UI.

## 11. Tooling

- `pex-sdk/`: headers (`pex.h` with the host API prototypes generated from the
  firmware's export table, `pex_host_t`, entry reasons), a linker script per
  target, a CMake function `pex_add_module(name SOURCES ... TARGETS armv6m
  armv8m-main-fp)` that builds one `.pex` per target.
- Compilers: model A with the pico-sdk's arm-none-eabi-gcc (as the firmware);
  model B with clang (`-fropi -frwpi`; the pico-sdk already ships clang
  toolchain files). `pex_add_module(... MODEL pic|reloc)` picks one; pic is the
  default for modules meant for the flash partition. GCC's
  `-msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative` may be
  accepted for model B once a test proves it yields the same ROPI/RWPI shape.
- `pexlink` (Python, no device code): ELF in -> `.pex` out; checks relocation
  types, emits GOT/veneers, writes header, sections, CRC, optional signature;
  `pexlink --dump file.pex` prints everything.
- The firmware build exports `host_api.json` (symbol, group, version, hash)
  — the single source for the SDK header and the on-device table, and the
  file CI diffs to catch accidental API breaks.
- mcusim: a `.pex` loader test per target (load, relocate, call, unload, fault
  attribution) before any device.

## 12. Versioning summary

| Version | Where | Changes when |
|---|---|---|
| `format_major.minor` | bytes 4-7 of every file | this spec changes (major: incompatible layout) |
| `api_major.minor` | header | the firmware's exported host API changes |
| per-group versions | `IMPORTS` / host table | one API group changes |
| `module_version` | header | the module's author releases |
| `build_id` | header | any byte of the image changes |

## 13. Open questions

- ~~[Q1]~~ **[D: exact target match]** Accept `armv6m` modules on M33 firmware (one build for both chips,
  slower code), or always require the exact target?
- ~~[Q2]~~ **[D: ARM only for now]** RISC-V (Hazard3) as a first-class target now, or reserve the ids
  and do ARM only in v1?
- ~~[Q3]~~ **[D: carve a PEX partition after a full backup (§14)]** Size of the `PEX` flash partition (carved from LittleFS: 256 KB?),
  and whether XIP is the default from v1 or RAM-only first (simpler: no flash
  writes, but costs RAM — the thing we are short of).
- ~~[Q4]~~ **[D: Secure world for now]** RP2350 Secure/Non-secure: run PEX code Secure like the firmware, or
  Non-secure behind the host API (real isolation, more work, CMSE veneers)?
- ~~[Q5]~~ **[D: option C -- curated `pexlua` first, the raw API later as its own exact-match group]** The Lua C API is large and tied to `luaconf.h` (64-bit integers
  today). Export it whole, or a smaller curated `pexlua` API that survives a
  later switch to 32-bit Lua numbers? -> trade-offs in §7.3; recommendation C
  (curated `pexlua` first, raw API later as its own exact-match group).
- ~~[Q6]~~ **[D: explicit unload, refused when on a stack (§8)]** Reference counting for unload: automatic via the host API, or
  modules are only unloaded at VM restart/reboot in v1?
- ~~[Q7]~~ **[D: unsigned allowed in development builds]** Signing required always, or `allow-unsigned` in development builds?
- ~~[Q8]~~ **[D: fat files later; only the running slice is copied (§4.4)]** Fat files (several targets in one `.pex`, Mach-O-universal style)
  later, or always one file per target with a naming rule
  (`otap.armv8m-main-fp.pex`)?
- ~~[Q9]~~ **[D]** The format, SDK and `pexlink` live in this firmware repo for
  now (`pex/`), structured so they can become a separate submodule later.

## 14. Making room: the PEX flash partition (done 2026-10-21)

Flash today (4 MB): table 64 KB | Main A 864 KB | Main B 864 KB | Wi-Fi 256 KB |
LittleFS 2032 KB | BTstack NVM 16 KB. The PEX partition comes out of LittleFS.

Why this is risky: the gateway's **WireGuard keys and Wi-Fi/Ethernet
config live in LittleFS** (`cfg/`), and today WireGuard is the only way to
reach the board. littlefs cannot shrink a mounted filesystem (it can only grow
it), so resizing means reformatting -- if that goes wrong remotely, the board
comes up without network config and is unreachable.

Steps:

0. **Backups** (done 2026-10-20, over HTTP, read-only):
   `~/Work/btgw-backups/2026-10-20_2018-10.255.255.23/` -- `littlefs.tar`
   (whole LittleFS via `/api/tar`: `cfg/` incl. `wireguard.json`,
   `anker.json`, `wifi.json`, `eth.json`, `bt.json`; `lua/`, `cache/`, web
   assets; 223 entries) + its SHA-256, and `sd/` (all SD files, 139 files,
   16.8 MB). **Not covered yet**: the BTstack NVM (bonds, 16 KB at the end of
   flash) and a raw image of the whole flash -- take both over SWD
   (`picotool save -a` / OpenOCD `dump_image`) once the board is on the
   debugger, right before step 3.
1. Decide the size (e.g. 256 KB) and where: at the **end** of the LittleFS
   region (before the BTstack NVM), so LittleFS keeps its start offset.
2. Firmware support, in this order, each its own OTA-able release:
   a. a **migration** in the firmware: when the partition table says LittleFS
      got smaller, mount the old LittleFS, copy every file to the SD card
      (`sd:/.lfs-migrate/`) and keep `cfg/` also in RAM, format the new smaller
      LittleFS, copy everything back, verify (sizes + hashes), and only then
      write a "migrated" marker; on any failure, keep running from the copy and
      say so at `/api/fs`. With a card present this makes the resize
      recoverable without a person;
   b. `partitions.json` + `fs.h` + linker check updated together (as
      `tools/run_tests.py` already enforces), the `PEX` partition declared.
3. **First time: with the debugger attached** (the owner's call for this
   board, 2026-10-20): flash the new partition table + firmware over SWD, watch
   the migration over RTT, check WireGuard comes back, then restore anything
   missing from `littlefs.tar` if needed.
4. Afterwards, other boards can take the same release over OTA, because the
   migration in 2a is proven.

**Done on the gateway, 2026-10-21** (firmware 1.195, `feature/lfs-migrate`;
details in `docs/flash-layout.md`): over SWD with the debugger attached, after a
raw 4 MB dump incl. the BTstack NVM. The migration copied 193 files (681,571 B)
to the SD card, verified them, formatted LittleFS to 1776 KB and restored all
223 entries in 32 s; 222 byte-identical to the pre-migration backup (the 223rd,
`var/wg_epoch.bin`, is WireGuard's boot counter), `cfg/` identical, WireGuard
came back with the restored keys. Slot B was erased (pre-change firmware would
format the new geometry). The layout now:

| offset | size | partition |
|---|---|---|
| 0x000000 | 64 KB | partition table |
| 0x010000 | 864 KB | Main A |
| 0x0E8000 | 864 KB | Main B |
| 0x1C0000 | 256 KB | Wi-Fi firmware |
| 0x200000 | 1776 KB | LittleFS |
| 0x3BC000 | **256 KB** | **PEX** (id `0x7065780000000000`, family `data`; reserved, reported in `sys.info.fs.pex`) |
| 0x3FC000 | 16 KB | BTstack NVM (unpartitioned) |
