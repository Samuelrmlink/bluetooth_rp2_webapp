# PEX — portable executable modules for microcontrollers (DRAFT 0.1)

Status: **planning draft, nothing implemented.** Decisions marked **[D]**,
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

Non-goals (v1): sandboxing (a PEX runs with the firmware's full privilege —
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
| 24 | 4 | `flags` | `XIP_OK` (code may run from flash), `RAM_ONLY`, `LUA_MODULE`, `APP`, `REENTRANT`, `NEEDS_SIG` |
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

- A module targets the **lowest** ISA it needs: an `armv6m` module is accepted
  by an M33 firmware too (Thumb-1 runs on v8-M Mainline) if the firmware opts
  in; the reverse is refused. **[Q1]**
- FP across the host API is always passed in core registers (softfp/soft) so
  the API is identical for M0+, M33 and M4 builds. **[D]**
- Chip-specific code (PIO, DMA channels, ...) goes through the host API, not
  register pokes, so the same `armv8m-main-fp` module runs on any RP2350 board.

## 6. Code model and relocations

**[D] Pre-linked on the host, one image, minimal relocations.**

- Built with the normal toolchain (`arm-none-eabi-gcc -mthumb -mcpu=... -Os
  -ffunction-sections -fdata-sections -fno-common`), linked with a PEX linker
  script that places text, rodata, data, bss **contiguously from address 0**
  and keeps relocations (`ld -q`/`--emit-relocs`).
- `pexlink` (host tool, §11) then:
  - resolves everything internal at link time (all `BL`/`B` and PC-relative
    loads inside the image are already final — they move with the image);
  - turns the remaining **absolute 32-bit references to the image itself**
    (`R_ARM_ABS32` from rodata/data pointer tables, vtables, function pointers)
    into the `RELOC` list;
  - turns references to host symbols into **GOT** entries (§7);
  - **refuses** anything else (`MOVW/MOVT` absolute pairs, `R_ARM_THM_CALL` to
    an undefined symbol, TLS, ...), with a message saying which compiler flag
    avoids it (e.g. `-mslow-flash-data`, `-mno-pic-data-is-text-relative`).
- `RELOC` entries are `u32 image_offset` of a 32-bit word to which the loader
  adds the image's run-time base. Split in two lists: words in the code/ro
  part (patched before the image is written to flash) and words in data.
- For RISC-V targets the same model with `R_RISCV_32` only; calls via `auipc`
  are PC-relative and need nothing. **[Q2]**

Why not position-independent code (`-fPIC`/`-msingle-pic-base`): it costs a
register and an indirection on every global access on M0+, toolchain support
for v6-M PIC is patchy, and a pre-linked image with a relocation list is
just as relocatable for a loader that places it once.

### 6.1 Where the image runs

| Mode | Code (text+ro) | RW (data+bss) | RAM cost | When |
|---|---|---|---|---|
| **XIP** (default) | written, relocated, into a reserved flash area and executed in place through XIP cache | RAM pool | data+bss only | installed modules |
| **RAM** | RAM pool | RAM pool | whole image | `RAM_ONLY` modules, timing-critical code, quick tests, chips without spare flash |

- XIP needs a flash region the loader owns: on the gateway's 4 MB part there
  is none free today (Main A/B 864 KB each, Wi-Fi 256 KB, LittleFS 2032 KB), so
  **carve a `PEX` partition out of LittleFS** (e.g. 256 KB) in
  `partitions.json`. **[Q3]** The loader keeps an index there (build id ->
  placement), so a module installed once boots from flash with no copy.
- Code is linked for address 0 and relocated to its flash address when
  installed; reinstall/compaction rewrites it. Flash writes stop XIP briefly
  (the firmware already handles this for LittleFS and OTA).
- RP2350: code in SRAM needs the region executable (ACCESSCTRL/MPU allow it by
  default for Secure); keep XN on the Lua arena regardless. **[Q4]**
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

## 8. Entry points and lifecycle

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
- Unload is refused while the module has live references: a Lua table/closure
  holding its functions, a registered callback, a running app. The loader
  counts these through the host API (`pex_ref/pex_unref`). **[Q6]**
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
- Policy (firmware config): `signed-only` (default for production),
  `allow-unsigned` (development; the UI says so loudly). **[Q7]**
- No sandbox: a PEX can do anything the firmware can. Signing is the security
  boundary; say so in the UI.

## 11. Tooling

- `pex-sdk/`: headers (`pex.h` with the host API prototypes generated from the
  firmware's export table, `pex_host_t`, entry reasons), a linker script per
  target, a CMake function `pex_add_module(name SOURCES ... TARGETS armv6m
  armv8m-main-fp)` that builds one `.pex` per target.
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

- **[Q1]** Accept `armv6m` modules on M33 firmware (one build for both chips,
  slower code), or always require the exact target?
- **[Q2]** RISC-V (Hazard3) as a first-class target now, or reserve the ids
  and do ARM only in v1?
- **[Q3]** Size of the `PEX` flash partition (carved from LittleFS: 256 KB?),
  and whether XIP is the default from v1 or RAM-only first (simpler: no flash
  writes, but costs RAM — the thing we are short of).
- **[Q4]** RP2350 Secure/Non-secure: run PEX code Secure like the firmware, or
  Non-secure behind the host API (real isolation, more work, CMSE veneers)?
- **[Q5]** The Lua C API is large and tied to `luaconf.h` (64-bit integers
  today). Export it whole, or a smaller curated `pexlua` API that survives a
  later switch to 32-bit Lua numbers?
- **[Q6]** Reference counting for unload: automatic via the host API, or
  modules are only unloaded at VM restart/reboot in v1?
- **[Q7]** Signing required always, or `allow-unsigned` in development builds?
- **[Q8]** Fat files (several targets in one `.pex`, Mach-O-universal style)
  later, or always one file per target with a naming rule
  (`otap.armv8m-main-fp.pex`)?
- **[Q9]** Where the format lives: this firmware repo, or a small separate
  repo (`pex`) shared by the gateway, km003c and the mcusim loader tests?
