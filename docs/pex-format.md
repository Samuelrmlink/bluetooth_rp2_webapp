# PEX — portable executable modules for microcontrollers (DRAFT 0.7)

Status: **draft 0.7 -- format 1.0, the RAM-mode runtime and XIP are
implemented.** 0.7: modules are installed into the PEX flash partition and
run in place (§16: the index, placement, compaction, power-fail safety,
model A relocated to its flash address); the gateway's pool is static and
taken from the Lua arena's budget (Q10, §15); `sd:/` loading (Q12); the
changes are listed in §18.

Status of 0.6: **format 1.0 and the RAM-mode runtime are implemented**
(`pex/` in this repository: SDK, `pexlink`, runtime, Lua layer, tests; the
gateway integration in `src/pex/`). 0.4: two code models; 0.5: the memory
model, §7.2; **0.6: what the implementation settled -- section payloads
(§4.5), the CRC covering the whole file, INIT/FINI sections, reloc words as
segment offsets, imports through generated veneers, the runtime independent
of Lua (§7.5), the v1 host API with exact signatures (§7.4, appendix A), apps
(`PEX_APP_RUN` config, `PEX_APP_STOP`), the gateway integration (§15), and
what is left for XIP (§16); the full list of changes is §17.** All of Q1-Q9
decided; Q10-Q12 new. The PEX flash partition exists on the gateway (§14,
done 2026-10-21). Decisions marked **[D]**, open questions **[Q]** (collected
at the end).

**Design principle (0.6) [D]: native apps first; Lua is optional.** A PEX
module does the work -- an app on the host's main loop serving its own ops and
events (a BLE firmware-update app becomes `app.m33.pex` + `otap.m33.pex`). The runtime
needs no Lua: modules are loaded, started and stopped from C, from the
firmware's ops (`pex.*`) and from `cfg/pex_autostart.json`. Where a firmware
has Lua, it is a **launcher and glue** -- a three-line script can start an app
(`pex.load`, `pex.start`) -- and, at lower priority, `require 'x'` returns a
module's native functions (§9). Boards without the Lua VM use the same `.pex`
files.

## 1. Why

Lua on the gateway is out of RAM. One BLE firmware-update backend alone keeps ~55 KB of
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
- **Runtime**: the loader plus the trampoline, the memory regions, the
  scheduler and the core host-API groups -- `pex/runtime`, portable C11 and
  one small assembly file per target (§7.5).
- **Instance**: one load of a module: its own RW block, name and state; model B
  instances of one build may share one copy of the code (§6.2).
- **App**: a module started with `PEX_APP_RUN`; it runs while it has scheduled
  tasks (§8).

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
| 16 | 4 | `target_features` | bits required by the code (0.6): 0 FPU-SP, 1 FPU-DP, 2 DSP, 3 CMSE; the loader refuses a bit its CPU lacks. `pexlink` sets FPU-SP\|DSP for `m33`, 0 for `m0p` |
| 20 | 2 | `api_major` | host API major it was built against (§7) — must equal the firmware's |
| 22 | 2 | `api_minor` | must be <= the firmware's |
| 24 | 4 | `flags` | bit 0 `PIC` (code model B, §6.2: text+ro are position-independent, RW is r9-relative), 1 `XIP_OK` (code may run from flash; set for every PIC file), 2 `RAM_ONLY`, 3 `LUA_MODULE`, 4 `APP`, 5 `REENTRANT` (with `PIC` only: several instances may share one copy of the code), 6 `NEEDS_SIG`, 7 `UNLOAD_ON_EXIT` (0.6) |
| 28 | 4 | `module_version` | the module's own version, `major<<24 \| minor<<16 \| patch` |
| 32 | 4 | `image_size` | bytes of the image as loaded (text + rodata + data, contiguous) |
| 36 | 4 | `text_size` | executable part; starts at image offset 0 |
| 40 | 4 | `ro_size` | read-only data, after text |
| 44 | 4 | `data_size` | initialised RW data, after ro (copied to RAM even for XIP) |
| 48 | 4 | `bss_size` | zeroed RW data, after data, not stored |
| 52 | 4 | `stack_hint` | stack use, from `-fstack-usage`: in 1.0 the largest single frame (a lower bound -- `pexlink` has no call graph yet); a host may refuse a file above its limit |
| 56 | 4 | `entry` | image offset of `pex_main` (§8), Thumb bit set; **0 = no entry** (a library of exports) |
| 60 | 4 | `section_count` | entries in the section table |
| 64 | 4 | `section_table_off` | file offset |
| 68 | 4 | `image_off` | file offset of the image bytes |
| 72 | 4 | `file_crc32` | **0.6: CRC-32 (IEEE, zlib's) of every byte of the file before `SIG`** (to `file_size` if unsigned) **with these 4 bytes taken as zero** -- header and sections are covered too, not only the image. Quick integrity; the signature is the real one |
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

Unknown section types with the `OPTIONAL` flag (bit 0 of `flags`) are
skipped; unknown types without it make the loader refuse the file. 0.6 adds:

| Type | Name | Contents |
|---|---|---|
| 7 | `INIT` | constructors (`.init_array`), run in order before `PEX_INIT` |
| 8 | `FINI` | destructors (`.fini_array`), run in reverse after `PEX_DEINIT` |

They replace keeping `.init_array` inside the image: a table of code
addresses in the image would need relocating (and is not ROPI-clean);
`pexlink` reads the arrays at build time and stores image offsets instead.
`DEPS` stays reserved: a 1.0 loader refuses a file that has one.

Order in the file (as `pexlink` writes it; a loader must not depend on it,
except that `SIG` is last): prefix + header, section table, the section
payloads (each 4-aligned), the image (8-aligned), `SIG`.

### 4.4 Multi-target ("fat") files -- later **[D, Q8]**

v1 files hold one target. A later minor revision may add a container -- a
fixed prefix with `target = 0xFFFF` and a slice table (`target, offset, size`)
of complete single-target PEX files. A loader then reads only the slice for
the running architecture: **code for another architecture is never copied
into executable memory or the XIP partition**, and the other slices are
never read beyond their table entry.

### 4.5 Section payloads (0.6)

All little-endian, 4-aligned, and bounded by the section's `size`.

- **RELOC**: `u32 n_ro, u32 n_rw`, then `n_ro` + `n_rw` words. An RO entry is
  the image offset of a 32-bit word in text/ro; an RW entry the offset of a
  word inside the RW block (data). **Bit 31 of either kind says the word
  points into RW**, else into the code (text+ro). The stored word is the
  target's **offset within its own segment** (`pexlink` rewrites RW targets
  from their link address to RW offsets), and the loader adds that segment's
  base: `*w += bit31 ? rw_base : code_base`. (0.5 had the bit on RW entries
  only; model A code also holds literal-pool words that point at data.)
- **IMPORTS**: `u16 n_groups, u16 n_imports`; `n_groups` x `{u16 id, u8
  major, u8 minor}` (the newest minor of the group any import needs); then
  `n_imports` x `{u32 name_hash, u16 group, u16 flags, u32 slot}` -- `slot` is
  the RW offset of the import's GOT word, `flags` bit 0 `WEAK`.
- **EXPORTS**: `u32 n`; `n` x `{u32 name_hash, u32 value, u16 kind, u16
  name_len, u32 name_off}` (value = image offset, Thumb bit set; kind 1 =
  function); then the names (NUL-terminated) the `name_off`s point into.
- **INIT**, **FINI**: `u32 n`; `n` x `u32` image offsets (Thumb bit set).
- **META**: TLV records `{u16 tag, u16 len, len bytes}`, each padded to 4.
  Tags: 1 name, 2 description, 3 author, 4 url, 5 license, 6 Lua module name,
  7 min RAM (u32), 8 version string, 9 tool. Strings are not NUL-terminated.
- **DEBUG** (`OPTIONAL`): records `{u32 value, u16 kind, u16 len, len bytes}`
  padded to 4: kind 1 = an import name (value = its hash: the loader names a
  missing import from it), kind 2 = a function symbol (value = image offset;
  a host tool symbolicates a fault offset with it).
- **SIG**: `u16 alg` (1 = ECDSA P-256 over SHA-256), `u16 key_id`, 64 bytes
  raw `r || s`; 68 bytes, last in the file, covering every byte before it.

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
| restrictions | none beyond §6.1 | no absolute address in text/ro: **a table of pointers cannot be `const`** -- declare it `PEX_PTR_TABLE` (§6.2); pexlink refuses one in rodata, naming it |
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
    avoids it. (0.6 correction: `-mslow-flash-data` and `-mpure-code` are what
    *produce* absolute MOVW/MOVT pairs -- the message says to drop them;
    literal-pool words are what model A relocates. `-fPIC` GOT forms are
    refused with "use MODEL pic".)
- `RELOC` holds two lists of 32-bit words to which the loader adds a base:
  `RO` words (in text/ro; patched before the image is written to flash or RAM)
  and `RW` words (in data). **Either kind may point into the code or into RW**
  -- bit 31 says which, and the stored word is an offset within that segment
  (§4.5). Model A code is full of `RO` words pointing into RW: a literal-pool
  word holding the address of a global.
- In RAM mode the loader places code and RW **separately** (both from the
  pool's long-term side), not contiguously: the same rule then serves XIP,
  where they cannot be contiguous. (The link is contiguous; nothing in the
  code reaches data PC-relatively -- pexlink refuses it if something does.)
- Calls to libgcc helpers (division, 64-bit, soft float) are linked into the
  module from the toolchain's `libgcc.a` for the target: they are pure code,
  so they suit either model (measured: no absolute words, also under ROPI).

### 6.2 Model B: position-independent (ROPI + RWPI)

- Built with clang: `--target=thumbv8m.main-none-eabi -mcpu=cortex-m33
  -mfloat-abi=softfp -fropi -frwpi` (M33) or
  `--target=thumbv6m-none-eabi -mcpu=cortex-m0plus -fropi -frwpi` (M0+), same
  `-ffunction-sections -fdata-sections -fno-common`; linked with the PEX
  linker script for model B: text+ro from address 0, RW (data+bss+GOT) as a
  separate segment (0.6: linked at 0x40000000 -- any address works, the code
  never contains it; two segments at 0 would overlap in the ELF). The static
  base is the start of that segment (`__pex_rw_start`), which is how lld
  resolves `R_ARM_SBREL32`/`MOVW_BREL`.
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
- Pointers in initialised data: a pointer in RW to code/ro or to RW
  (`static void (*fn)(void) = handler;`, `int *p = &x;`) is an absolute word in
  RW; clang leaves it to the linker, pexlink lists it in the `RW` relocs, and
  the loader fixes it in the instance's RW block -- it never touches flash.
- **0.6, measured: a `const` table of pointers is an absolute word in
  rodata** (clang under `-fropi` keeps `static const char *const names[]` and
  function tables in `.rodata` with `R_ARM_ABS32`), which ROPI code cannot
  hold: pexlink refuses it and names the table. Worse, clang's optimiser
  promotes a *never-written* non-const static table to rodata too. The SDK's
  `PEX_PTR_TABLE` (`__attribute__((used))` under `__ARM_ROPI`, empty in model
  A) keeps such a table in RW: `static PEX_PTR_TABLE pexlua_reg_t fns[] = {...};`.
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
  (0.7: done and implemented -- §14, §16.)
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
  name) + `u16 group` + `u16 flags` (`WEAK`: may be absent -> GOT entry 0) +
  the slot (§4.5). The host table is sorted by hash per group; collisions are
  rejected when the tables are generated (0.6: `gen_host_api.py`, across all
  groups). Names are kept in `DEBUG` for tools/messages.
- **Single source (0.6) [D]**: `pex/host_api.json` lists every group and
  symbol (prototype, since-minor, implementation name). `pex/tools/
  gen_host_api.py` generates from it the SDK header `pex_api.h`, the import
  veneers a module links, and the host's lookup tables (`pex_host_<group>.c`,
  one per group so a host compiles only the groups it implements). It is the
  file to diff for API breaks. (0.5 had the firmware export it; it is the
  other way round: the firmware builds from it.)
- Linking (0.6, as implemented): the SDK links a generated **veneer per host
  symbol**, each in its own section so `--gc-sections` keeps only those a
  module calls, together with its GOT slot (`.pex.got.<name>`, placed at the
  start of RW). The veneer loads the slot and jumps -- model A M33: `ldr r12,
  =slot; ldr pc, [r12]`; model B: `ldr r12, =slot(sbrel); add r12, r9; ldr
  pc, [r12]`; M0+ (no `ldr` to r12): `push {r0,r1}; ldr r0, =slot; [add r0,
  r9;] ldr r0, [r0]; str r0, [sp, #4]; pop {r0, pc}` -- transparent to
  arguments (incl. stack ones) and returns; M33 clobbers only r12 (IP, as any
  veneer may). pexlink finds the slots by their `__pex_got_<name>` symbols. The
  loader fills them.
- **Weak imports are per symbol**: `PEX_WEAK_IMPORT(sym)` in the module; its
  slot stays 0 when the host lacks it and `pex_avail(sym)` tests the slot. A
  group the host lacks (or has with another major) is acceptable when every
  import from it is weak; otherwise the load is refused naming the group and
  the symbol. Per group the file records the newest minor any of its imports
  needs, so a module using only old symbols loads on an older host.
- Compatibility: a minor bump only **adds** symbols; a major bump may change
  or remove them. The firmware refuses `api_major` mismatches and
  `api_minor` > its own, and lists the missing symbol by name if `DEBUG` is
  present.
- The **Lua C API** raw group (`lua54_*`, §7.3 option C) is not in v1; the
  curated `lua` group (`pexlua_*`) is. (0.6 correction: the gateway's Lua is
  built with `LUA_32BITS=1` -- 32-bit integers and floats -- not 64-bit; the
  `pexlua` signatures use `int64_t`/`double` and the shim narrows, which is the
  point of option C.)

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
| `mem` | lifetime-scoped memory (§7.2): the module's long-term heap, per-call/per-job scratch arenas, fixed-size pools; `malloc/free` as a fallback; `memcpy`-family | never the Lua arena |
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

0.6: the groups of v1 and their ids are in §7.4; `gpio`/`i2c`/`spi`/`uart`/
`pio`/`dma`/`timer`/`pwm`/`adc`/`crypto` above are still planned. `sd` is not
a group of its own: `fs` covers `sd:/` paths (the Lua `file` semantics).

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
  exposed in `sys.info`. (0.6: in `pex.list`/`pex.info`, not `sys.info`, whose
  reply buffer is full.)

0.7, the gateway: the pool is a **static block of its own** (`CONFIG_PEX_POOL_KB`,
48 KB), apart from the Lua arena -- a VM restart never touches it -- and from
the C heap; the RAM came out of the Lua arena's budget (§15). Installed
modules need only their RW (and their own allocations) in it.

As implemented (0.6): the pool is one block given to `pex_init()`. The
long-term side holds, besides the modules' own blocks, each instance's
**record, metadata, RW block and (RAM mode) code** -- all tagged with the
instance, so unload frees them in one sweep with any leaks; a shared code
copy (model B, `REENTRANT`) is owned by the runtime and reference-counted.
Blocks are first-fit with coalescing, 8-aligned (`PEX_MEM_DMA`: 32-aligned,
carved fresh). The loader's temporaries (header, import list) are a scratch
arena that ends before any module code runs. Scratch arenas end in LIFO
order; an out-of-order end is refused with `PEX_EORDER` and logged with the
module's name; an arena left open at unload is released. A host may allocate
from the pool for itself (`pex_alloc_host`, e.g. an event queue shared by
several modules). Pools are one long-term block (header + slots, a free list
through the free slots).

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

### 7.4 The v1 groups (0.6) **[D]**

| id | group | in | covers |
|---|---|---|---|
| 1 | `core` | runtime | API/group versions (`pex_has`), target, time (`pex_ms`, `pex_us`), `snprintf`, panic-free assert, the libc and `__aeabi_mem*` helpers compilers call implicitly |
| 2 | `log` | runtime | the host's log, prefixed with the module's name; hex dumps |
| 3 | `mem` | runtime | §7.2: long-term, DMA, malloc family, scratch arenas, pools, stats |
| 4 | `sched` | runtime | cooperative tasks on the main loop: every / after / post / cancel |
| 5 | `pex` | runtime | the module's handle, `pex_ref`/`pex_unref`, `pex_app_exit` |
| 8 | `json` | runtime (portable) | path lookups on a JSON text (`"a.b.2.c"`) and a streaming writer |
| 6 | `board` | host, optional | board name, CPU clock, unique id |
| 7 | `sys` | gateway | wall clock (SNTP), host name, firmware version, memory (C heap, Lua arena, PEX pool, stack) |
| 9 | `bus` | gateway | register ops (request JSON in, reply JSON out, synchronous) and emit events |
| 10 | `fs` | gateway | `lfs:/` and `sd:/` files: open/read/write/seek/size/close/stat/list/mkdir/remove/rename/media |
| 11 | `net` | gateway | the HTTP(S) client: start (into a buffer or a `lfs:/` file), status, cancel, completion callback |
| 12 | `bt` | gateway | LE central: scan table (filter by name prefix / 128-bit UUID), connect, GATT lookup by service + characteristic UUID + instance, read, write with/without response with completion, notify/indicate, MTU; events through a listener |
| 16 | `lua` | Lua layer, optional | `pexlua` (§7.3, §9) |

Exact signatures: **appendix A** (generated from `host_api.json`).
Asynchronous results (the card, HTTP, BLE) arrive as **callbacks from the
host's main loop, through the trampoline** -- never from an interrupt and
never from inside a radio stack's callback: the host queues and dispatches.
Rule for calls that may complete later (fs on `sd:/`): the return value is
the result when the call completes at once (LittleFS always does);
`PEX_EPENDING` when the callback will deliver it; `PEX_EAGAIN` when it would
block and no callback was given. Callback argument structs (`pex_bt_event_t`,
`pex_http_res_t`, `pex_fs_entry_t`) are valid during the callback only.
Whatever a module registers with a group (ops, listeners, open files,
requests) is released when it unloads (the runtime's `on_unload` hook).

### 7.5 The runtime without Lua: what a host provides (0.6) **[D]**

The runtime (`pex/runtime`) has **no dependency on Lua, on the gateway or on
any firmware subsystem**: C11, `<string.h>`/`<stdio.h>` (`vsnprintf`), and
one assembly file (the trampoline). It builds into any pico-sdk firmware with
`pex_runtime_add(<target>)` (`pex/runtime/pex_runtime.cmake`). A host provides:

- **memory**: the pool, one 8-aligned block, in `pex_config_t`;
- **the port** (`port/pex_port.h`; `port/pico/pex_port_pico.c` for the
  pico-sdk): `pex_port_ms`, `pex_port_us`, `pex_port_log` (one line),
  `pex_port_code_sync` (barriers after writing code), `pex_port_stack_bounds`;
- **optionally**: a signature verifier (`port/pico/pex_sig_uecc.c` with
  micro-ecc and a key table), a trace word that survives a reset (hang
  attribution), an `on_unload` hook, more groups (`pex_register_group`);
- **a main-loop call** to `pex_poll()` (tasks, deferred exits);
- **a HardFault hook** calling `pex_fault_attribute(frame)` (§8).

From C: `pex_load()`/`pex_load_mem()` (a random-access reader: memory, flash,
a file), `pex_start(m, cfg_json)`, `pex_stop()`, `pex_unload()`, `pex_sym()` +
`pex_call()`, `pex_info()`/`pex_list_json()`, `pex_errmsg()` (why a load or
unload was refused, in words). The Lua layer (`pex/lua`, `pexlua_add()`) is
compiled only into firmware that has Lua.

## 8. Entry points and lifecycle

- Every call into a module goes through the host's **export trampoline**: it
  bumps the module's `active` count (§8 unload), and for a `PIC` module sets r9
  to that instance's RW base (§6.2) and restores the caller's r9 on return.
  (0.6, as implemented: `pex_call(m, fn, a0..a3)` in C keeps the books --
  `active`, the stack of executing modules that `pex_self()` and the
  allocators' owner tag come from, the trace word -- and `pex_tramp` (asm,
  M33 and M0+) swaps r9 and calls. Up to **four word arguments**; `r0:r1`
  come back, so 64-bit results work; more arguments go in a struct. Model A
  gets r9 = its RW base too, which it ignores.)
- `entry` points to `int pex_main(const pex_host_t *host, uint32_t reason, void *arg)`:
  - `reason = PEX_INIT` (1) after linking, once per load: set up, register;
    a negative return refuses the load (`PEX_EINIT`, the module is freed);
  - `PEX_LUA_OPEN` (2) for a Lua module: push the module table (like
    `luaopen_<name>`), `arg` = `lua_State *`, return 1;
  - `PEX_APP_RUN` (3) for an app (runs on the firmware's cooperative loop: must
    return or yield through the host API, same rules as a Lua script: no
    blocking, watchdog). **0.6: `arg` = the app's configuration as JSON text**
    (from `pex.start`, the autostart entry or `pex_start()`), or NULL. The app
    schedules its work (`sched`) and returns; it is **running while it has
    tasks**, and exits when the last one ends or it calls `pex_app_exit()`;
  - `PEX_DEINIT` (4) before unload: free everything, cancel callbacks;
  - **`PEX_APP_STOP` (5, new in 0.6)**: the host asks a running app to stop
    (`pex.stop`); the runtime then cancels whatever tasks are left.
- `.data` is copied and `.bss` zeroed by the loader before `PEX_INIT`; static
  constructors (0.6: the `INIT` section) run by the loader, in order;
  destructors (`FINI`) in reverse after `PEX_DEINIT`.
- States (0.6): `loaded` -> `running` (has tasks) -> `exited` / `stopped`;
  `faulted` from any. A faulted module is never entered again (calls return
  `PEX_EFAULT`); it can only be unloaded, and its `DEINIT`/`FINI` are skipped.
- **Unload is an explicit API** (`pex.unload(name)` from Lua, `pex_unload()`
  from C) **[D, Q6]**, and it is **refused while any of the module's code is
  on a call stack**, or while it still has live references:
  - *on a stack*: every call into a module goes through the host's export
    trampoline, which increments the module's `active` count on entry and
    decrements it on return; callbacks the module registered (timers, BLE,
    HTTP sinks) go through the same trampoline. **The count is the check.**
    (0.6: the proposed safety-net scan of the stacks for return addresses
    into the image is only a debug option, off by default -- live frames hold
    *stale* return addresses, and in the test suite a perfectly idle module
    was refused by it.) A `longjmp` out of module code (a Lua error raised
    inside it, a recovered fault) leaves frames behind; `pex_unwind(depth)`
    puts the books right, and the Lua layer does it for every call;
  - *references*: Lua closures/tables holding its C functions (a userdata
    anchor with `__gc` per module), anything else holding a pointer into it --
    counted through `pex_ref/pex_unref` in the host API. A manual unload
    (`pex.unload`, the gateway `pex.unload` op) asks Lua to release its
    anchor first: `pex.on_unload(name)` runs while the module is still
    loaded, then the ref is dropped, so a closure that is still reachable
    does not by itself refuse the unload. Calling that closure afterwards
    is an error, and the firmware logs it once. A module with tasks, or one
    on a call stack, is not detached. Any other ref still refuses;
  - *a running app* (0.6): its scheduled tasks count; `pex.stop` first.
  Ops, listeners, open files and requests it registered with host groups do
  **not** block unload: the host releases them (`on_unload`), and leaked
  long-term blocks, pools and open scratch arenas are reclaimed (and logged).
  A refused unload says why (which stack / which reference). A module loaded
  only to run once (an app) can be marked `UNLOAD_ON_EXIT`.
- Faults: a HardFault with the PC inside a PEX image is reported as
  `module name + build_id + offset` (symbolicated on a host with the `DEBUG`
  section), then the firmware's normal fault path. (0.6: also when the PC is
  in host code called from a module -- reported as "in host code called from
  it" with the caller's offset -- and a call through a NULL weak import is
  attributed by LR. A host that can resume (the test hosts do) unwinds and
  keeps running; the gateway records the fault and reboots, §15.)
- **Hangs**: modules are cooperative on the one main loop, so a callback
  that never returns blocks every other module. The host may set a slice
  (`pex_config_t.slice_ms`; the gateway uses `CONFIG_PEX_SLICE_MS`, 500 ms).
  The outermost `pex_call` arms it. If the call is still running when it
  fires, a timer interrupt returns that call early: the module whose code
  was running, and any module it had called into, is marked faulted, its
  tasks are cancelled, `on_slice` runs (the gateway releases what the module
  registered and appends the build to `var/pex_faults.json`), and `pex_poll`
  continues with the next task. The call comes back `PEX_EFAULT`. A faulted
  module is not entered again. The watchdog is the backstop, not the first
  response: while module code runs the trampoline still keeps the first word
  of its build id in a host word that survives a reset, and a watchdog reset
  with it set (the slice never got to run, because the module had masked
  interrupts) names the module on the next boot.

## 9. Lua integration (optional, 0.6)

Only for firmware with Lua (`pex/lua`, `CONFIG_PEX_LUA`). In priority order:

1. **The launcher** -- a global `pex` table:
   `pex.load(name [, path]) -> info | nil, err` (finds
   `lfs:/pex/<name>.<arch>.pex`; `PEX_INIT` runs), `pex.start(name [, cfg])`
   (cfg a table, encoded with `json.encode`, or a JSON string), `pex.stop`,
   `pex.unload` (refused while running, like the op), `pex.list()`,
   `pex.info(name)` (state, version, build id, model, code/RW sizes, sharing,
   active/refs/tasks, memory, exports, fault), `pex.call(name, export, ...)`.
   A module loaded this way **belongs to the firmware, not to the VM**: an app
   keeps running when the script that started it ends, and survives a VM
   restart. A three-line script starts an app:

   ```lua
   pex.load("app")
   pex.start("app", { poll_ms = 500 })
   ```
   0.7: `pex.install(name | path) -> info | nil, err` (into the flash
   partition, §16), `pex.installed() -> { info, ... }`, `pex.uninstall(name)`;
   **`pex.load(name)` and `require` prefer the installed copy**, which loads
   with its code in place (the host's `find` returns `xip:<name>`).
2. **`require 'name'`** (0.6 correction: the PEX is tried **first**, the 0.5
   order contradicted "prefers the PEX"): `lfs:/pex/<name>.<arch>.pex` if it
   exists and loads for this target/API, else the firmware's own `require`
   (`lua/name.lua` ...). `PEX_LUA_OPEN` builds the table with the `lua` group.
   The global `require` becomes a small **Lua** function that tail-calls the
   firmware's own: a C wrapper would hide the calling script's frame, which is
   how the gateway's `require` finds the caller's environment (sandbox).
   A module loaded by `require` **belongs to the VM**: every Lua function it
   registers holds a reference through one anchor userdata; when the last is
   collected (or the VM closes) it is unloaded. `pex.unload(name)` calls
   `pex.on_unload(name)` while the module is still loaded, drops the anchor
   and `require`'s cache entry, then unloads. A function used after that
   raises an error. `pex.stop` does not unload.
3. Every call from Lua into a module runs under `lua_pcall`: a Lua error
   raised inside module code (`pexlua_checkint` on a bad argument) unwinds to
   the bridge, which fixes the trampoline's books and re-raises from host
   code. Yielding across a module call is not supported.

- `sd:/pex/` is not searched by `require` (0.7: the `pex.load` and
  `pex.install` ops take `sd:/` paths, asynchronously -- §15).
- The Lua module's memory: its code is not in the arena at all (installed:
  flash; RAM mode: the PEX pool); its C data is in the PEX pool; only the
  table and closures it pushes live in the Lua arena.
- First candidates from a BLE firmware-update backend: `otap` (transfer engine + hex),
  image-header parsing, `json`-shaping of catalog listings, a registry index -- now as
  native apps (`app.m33.pex`, `otap.m33.pex`) serving their own ops.

## 10. Integrity and trust

- `file_crc32` (0.6: the whole file before `SIG`, §4.2) catches corruption
  cheaply; `SIG` is authoritative.
- `SIG`: `u16 alg` (1 = ECDSA P-256 over SHA-256), `u16 key_id`, then the
  signature over **every byte of the file before `SIG`** (raw `r||s`, 64
  bytes). Same key and tooling as OTA releases (`tools/ota_sign.py`: `pexlink
  link --sign` / `pexlink sign` use its key handling -- `--key`, else
  `$BTGW_OTA_KEY`, else `~/.config/btgw/ota-signing.pem`), so a firmware
  trusts what its owner signs. `key_id` n selects the host's n-th key (1 =
  the OTA key on the gateway). The CRC excludes `SIG`, so a damaged signature
  is caught by the signature check alone (tested).
- Policy (firmware config) **[D, Q7]**: `signed-only` for production builds,
  `allow-unsigned` for development builds (the UI says so loudly). An unsigned
  module is never installed into the XIP partition of a `signed-only` device.
  A file with `NEEDS_SIG` is refused unsigned under either policy. (0.6: the
  gateway defaults to **signed-only** (`CONFIG_PEX_ALLOW_UNSIGNED=n`);
  `pex.list` reports the policy.)
- No sandbox: a PEX can do anything the firmware can. Signing is the security
  boundary; say so in the UI.

## 11. Tooling

- **`pex/sdk/`** (0.6: in this repo, `pex/`, ready to become a submodule):
  `include/pex.h` (format constants, entry reasons, errors, `pex_host_t`,
  `PEX_EXPORT`, `PEX_WEAK_IMPORT`/`pex_avail`, `PEX_META_STR`,
  `PEX_PTR_TABLE`), the generated `pex_api.h` (the host API prototypes),
  linker scripts per target and model (`ld/pex_<m33|m0p>_<reloc|pic>.ld`), and
  **`cmake/pex.cmake`**:

  ```cmake
  pex_add_module(hello SOURCES hello.c TARGETS m33 m0p MODEL pic|reloc
                 [KIND lib|app|lua] [VERSION 1.2.3] [DESCRIPTION ".."]
                 [FLAGS reentrant,unload-on-exit,needs-sig] [OUTPUT_DIR d]
                 [SIGN [KEY pem] [KEY_ID n]] [CFLAGS ..] [INCLUDES ..])
  ```

  builds `<name>.<target>.pex` per target with custom commands that call the
  cross compilers directly -- so it works inside any CMake project, a
  pico-sdk firmware included, without touching its toolchain.
- Compilers: model A with the pico-sdk's arm-none-eabi-gcc (15.2); model B
  with clang + ld.lld (22; `-fropi -frwpi`, `--target=thumbv8m.main-none-eabi`
  / `thumbv6m-none-eabi`); both link the toolchain's `libgcc.a` for the
  target. pic is the default. GCC's `-msingle-pic-base -mpic-register=r9
  -mno-pic-data-is-text-relative` may be accepted for model B once a test
  proves it yields the same ROPI/RWPI shape.
- `pexlink` (Python, no device code): `link` (ELF -> `.pex`: classifies every
  relocation the linker kept and refuses what the model cannot carry, with the
  symbol and the flag to change; builds IMPORTS from the kept GOT slots,
  EXPORTS, INIT/FINI, META, DEBUG; CRC; `--sign`), `sign` (sign an existing
  file), `dump` (`--dump`; `--relocs`, `--verify-key`).
- `pex/host_api.json` is the single source (§7); `pex/tools/gen_host_api.py`
  makes the header, veneers, host tables and appendix A.
- Tests: `pex/test/run_sim.py` (both test hosts in mcusim + pexlink refusals +
  host unit tests), `pex/test/gw_sim.py` (the gateway firmware in mcusim),
  `pex/tools/pex_remote_test.py` (a real gateway over HTTP).

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
- ~~[Q10]~~ **[D, 0.7: (a) -- a static pool of 48 KB, the Lua arena 164 ->
  116 KB, sized from measurements (§15)]** (0.6) Where the gateway's PEX pool comes from. The SD build has
  ~18 KB of C heap once booted and ~10 KB of its 864 KiB slot left. v1 takes
  the pool lazily from the C heap (default up to 12 KB, keeping 8 KB free --
  pico_malloc panics on any failed allocation, so the pool is taken with
  newlib's own `malloc` and shrinks to the contiguous room, found in mcusim) --
  enough for the test modules, not for a full native app. The Lua arena cannot
  lend it on this branch: a VM restart re-initialises the whole arena. Options:
  (a) shrink `CONFIG_LUA_HEAP_KB` and give that RAM to a static pool once the
  large backend is native (the arena's own users shrink with it); (b) an
  `lheap` region that survives a VM restart (needs `lheap.c`, which another
  branch is reworking -- its TLS pool is the same idea).
- **[Q11] (0.6, still open in 0.7) Late op replies.** The firmware's ops reply synchronously (a
  WebSocket reply context has no generation check; the HTTP one is gone when
  the handler returns), so a native op replies at once and reports completion
  with an event -- the pattern the built-in ops use. Add a reply token with a
  connection generation if a module needs true async replies?
- ~~[Q12]~~ **[D, 0.7: read the whole file into the pool asynchronously, then
  load or install it; the op replies `{pending:true}` and the event
  `pex.sd_done` reports the outcome (§15)]** (0.6) Loading from the SD card. v1 loads from LittleFS (and memory/
  flash); a card read is an asynchronous job, so `sd:/pex/` needs either a
  load that completes through a callback, or a copy to LittleFS first. Which?

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

## 15. Gateway integration (0.6, implemented; 0.7 in §15.1)

The runtime and the groups are in the gateway firmware behind `CONFIG_PEX`
(default y): `src/pex/` holds everything new; existing files get one hook each.

**Hooks** (for merging with other branches -- each is a few lines, guarded):

| file | hook |
|---|---|
| `CMakeLists.txt` | the `if(CONFIG_PEX)` block (runtime, groups, pexlua, -Os for Lua's compile-time half and the HTTP server: below) |
| `Kconfig` | `source "src/pex/Kconfig"` |
| `src/api/ops.c` | `&op_module_pex` in `modules[]` |
| `src/api/api.c` | `pex_gw_dispatch()` tried after the built-in ops, before `lbind_dispatch` (native modules' ops) |
| `src/main.c` | `crumb(CRUMB_LOOP_PEX); pex_gw_poll();` in the main loop |
| `src/util/crumb.c/.h` | `CRUMB_LOOP_PEX`; a weak `crumb_fault_hook(frame)` called from the HardFault handler |
| `src/bt/le/central.c` | a weak `le_pex_event()` called where values, write completions and link changes are already reported (no change without PEX) |
| `src/lua/lfile.c` | `pex_gw_lua_open(L)` at the end of `lfile_open` (Lua builds with `CONFIG_PEX_LUA`) |

Not touched: `src/lua/lheap.*`, `lrt.*`, `lbind.c`, `lhttp.c`. (0.7: `configs/*` -- the arena/pool split, §15.1 -- and compile options for `src/lua/*.c` in `CMakeLists.txt`, not the files.)

**Ops** (WebSocket and `POST /api/op`): `pex.list` (runtime, policy, pool,
C-heap headroom, the previous boot's fault, modules), `pex.info {name}`,
`pex.load {name | path [, as]}`, `pex.start`/`pex.run {name [, cfg]}`
(`cfg` an object or a JSON string), `pex.stop`, `pex.unload`, `pex.call {name,
fn, args:[..4]}`, `pex.files` (lfs:/pex/), `pex.autostart [{set:{...}}]`,
`pex.faults`, `pex.faults_clear`, `pex.ops` (ops registered by modules).
Events `pex.*`.

**Autostart**: `cfg/pex_autostart.json`, read 3 s after boot:
`{"modules":[{"name":"app","path":"pex/app.m33.pex","start":true,"cfg":{..}}]}`
(`path` defaults to `pex/<name>.<arch>.pex`; `start` applies to apps).
**Nothing PEX runs at boot unless this file lists it** -- no pool is taken
either (the pool is allocated on first use).

**Remote safety** (the board is reached over WireGuard only):
- a module fault is attributed (`PEX FAULT: module X build B offset O`) and
  persisted in a 64-byte record next to the crumb record (SCRATCH_X, not
  cleared by a reset); the board then takes its normal fault path -- the
  watchdog reboots it into the **same** image;
- a hang inside module code with interrupts still enabled is stopped by the
  slice (§8): that module is faulted, its tasks cancelled, the build id is
  appended to `var/pex_faults.json` immediately, and the main loop keeps
  running. The trace word is still kept, so a hang that masks interrupts
  (the slice cannot preempt it) still ends in a watchdog reset and the next
  boot names it;
- either way the build id is in `var/pex_faults.json`, and autostart
  **skips** every listed build until `pex.faults_clear`;
- a TBYB image that faults before it is confirmed reverts to the previous slot,
  as any image does.
Proven in mcusim (`pex/test/gw_sim.py`, scenarios `fault`, `hang`, `hangmask`, and the host suite's `slice.*`).

**Measured in mcusim** (0.6; for 0.7 see §15.1) (the deploy image, SD + Wi-Fi + WireGuard + BLE up):
the pool came out at 10 KB (contiguous room), leaving 8 KB of C heap.
Testing on a real board: `pex/test/remote/mkremote.py` makes the upload set
(signed with the OTA key; the fault modules are never in it) and
`pex/tools/pex_remote_test.py --base http://<gateway> --files <set> [--sd]
[--http-url URL --http-file-url URL] [--bt [UUID]]` uploads it, runs the ops,
the refusals and the Lua launcher/require checks, prints a table and removes
everything it uploaded. It never writes the autostart file, never reboots.

Observation (not PEX): after any watchdog-driven reboot in mcusim the OTA
group prints "no A/B partition table -- image group disabled (boot type 0)"
(the ROM's partition table is not reloaded on mcusim's watchdog reset); worth
checking on hardware, since SMP image commands would be disabled until a
normal boot if that happened there too. (0.7: the same in mcusim for a
SECOND flash-update boot -- slot B -> slot A, as the next OTA of the real
board will be: the image comes up with "no A/B partition table ... boot type
4" and cannot be bought; with the deployed 1.212 as the new image just the
same, so it is not the new firmware. The real gateway has taken B -> A
updates before (1.163 -> 1.180). `gw_sim.py --via` reproduces it.) Also
in mcusim, after a reboot of a TBYB-booted image some HTTP uploads (PUT
/api/fs) stall until the client's timeout while ops keep answering: 4 of 20
with the deployed 1.212, 6 of 20 with 1.228 (`gw_sim.py --scenarios
ota,putstress`); `gw_sim.py` retries an upload, as it retries SMP uploads.

**Signing**: signed-only by default; the OTA public key (`src/ota/ota_pubkey.h`)
verifies modules signed by `pexlink --sign` with the OTA key.

**Budget** (0.6; for 0.7 see §15.1) (pico2w_sd, 2026-10-21): the runtime ~15 KB, the gateway groups and
ops ~11 KB, pexlua ~4 KB of flash; RAM ~3 KB of tables + the pool. The SD
image had ~8 KB of its slot left, so code off the hot path went `-Os`: Lua's
compile-time half and cold libraries (code generator, parser, lexer,
(un)dump, auxlib, base/math/table/utf8/coroutine libs, ldebug: -31 KB; the
interpreter core and the string library stay -O3) and the HTTP server (-6 KB,
bound by TCP and flash). ~10 KB of the slot are left. Pool: see Q10.

### 15.1 The gateway in 0.7: the pool, XIP, `sd:/`

**RAM budget** (pico2w_sd, `.bss` unchanged in total; the TLS pool and the
lwIP trim came with feature/lua-net-fs):

| region | 0.6 (fw 1.212) | 0.7 | what decides it |
|---|---|---|---|
| Lua arena (`CONFIG_LUA_HEAP_KB`) | 176 KB | **116 KB** | the launcher + a BLE driver script + a few scripts, measured below |
| PEX pool (`CONFIG_PEX_POOL_KB`) | 10 KB from the C heap, on first use | **48 KB static** | module RW, their memory, RAM-loaded code (a 41 KB module loads into RAM) |
| TLS pool (`CONFIG_NET_TLS_POOL_KB`) | -- (in the arena) | 40 KB | one TLS session, measured 31-33 KB (lua-net-fs) |
| lwIP `MEM_SIZE` / pbuf pool | 48 KB / 24 | 32 KB / 16 | measured peaks 16.8 KB / 6 (lua-net-fs) |
| C heap | ~19 KB, 8 KB after the v1 pool | ~17 KB, none of it PEX's | pico_malloc panics when it runs out |

Measured in mcusim (`pex/test/gw_sim.py --scenarios budget`, the SD build,
Wi-Fi, WireGuard, SD and BLE up; `script.heap` after a full collection):

| state | Lua live | Lua peak | largest free (116 KB arena) |
|---|---|---|---|
| after boot (VM + bindings, no script) | 19.7 KB | 38 KB | 91 KB |
| a BLE driver script uploaded, compiled into the bytecode cache | 19.7 KB | **93 KB** | 53 KB |
| the BLE driver script running (scanning) | 73.5 KB | 93 KB | 21 KB |
| + a PEX launcher script (`pex.load`/`pex.start`, kept alive) | 75 KB | 93 KB | 21 KB |
| after a VM restart, both started again from the cache | 75 KB | 83 KB | 27 KB |

No allocation failed. The compile peak is what needs the room: a changed
module is compiled from an (almost) empty arena -- at boot, or after a VM
restart -- not next to a running driver. Moving RAM between the two is
`CONFIG_LUA_HEAP_KB` / `CONFIG_PEX_POOL_KB` with a constant sum.

**Flash**: XIP and the ops took the SD slot past full (-4.7 KB); the
firmware's Lua glue (bindings, the runtime, the file/HTTP/JSON bridges -- not
the interpreter core and not the allocator `lheap.c`), OTA/SMP, the
one-time LittleFS migration and littlefs went `-Os`. With the lua-net-fs
merge the SD image is 861.6 KB of the 864 KiB slot: **~22.6 KB left**.

**Ops** (0.7): `pex.install {name | path}` (a name means
`lfs:/pex/<name>.<arch>.pex`; `sd:/...` is read asynchronously), 
`pex.installed` (the partition: size, free, largest run, each module with
offset, size, model, signed, loaded, xip_ready), `pex.uninstall {name}`
(refused while it runs from flash), `pex.compact`, `pex.verify {name}` (CRC
and signature of the installed copy, in place). `pex.load {name}` prefers
the installed copy (`xip:<name>`; `{ram:true}` loads the lfs file instead);
`pex.load {path:"sd:/..."}` and `pex.install {path:"sd:/..."}` reply
`{pending:true}` and report through the event `pex.sd_done {op, path, ok,
name | error, msg}` -- the file is read into the pool through the storage
worker's job slot (a long-term block, freed afterwards), then loaded or
installed. Autostart and `require` find installed modules first.
`pex.list` adds `pool.static` and `xip {size, free, largest, modules,
rw_area, flash_ops}`.

**Where the partition is seen.** Booted from an A/B slot, the ROM maps the
slot at 0x10000000 through QMI ATRANS0 and leaves the rest of the first 4 MB
window unmapped -- 0x10000000 + 0x3BC000 faults (`src/fs/fs.h`). Reads (the
index, verification) use the untranslated uncached alias, as LittleFS does;
**code runs through ATRANS1**: the window at 0x10400000 is pointed at the
partition (BASE 0x3BC, SIZE 0x40 pages), cached like the firmware's own
code. The ROM uses ATRANS1 only for a slot larger than 4 MB and nothing in
the firmware or the SDK's flash paths touches ATRANS; it is set again after
every flash operation. (The RP2350 test host uses the same window.) No XIP
while the LittleFS migration is pending: the partition is then still inside
the old filesystem.

**Flash writes** go the way of LittleFS and OTA: `flash_range_erase/program`
inside `flash_safe_execute` (core 1 parked), the watchdog fed per operation.
An install blocks the main loop for its erase/program time: on the real
part ~45 ms per 4 KB sector and ~0.8 ms per page, so ~0.6 s for 41 KB plus
the index update, and the signature checks (before and after, in place);
compacting the whole partition is several seconds.

**Remote test** (0.7): `pex_remote_test.py` adds the pool checks, a 41 KB
module loaded into RAM, and the XIP part -- install hello/arith/big/counter,
run big in place from the ATRANS1 window (and its pool cost: RW only),
shared flash code, arith (every relocation kind) in place, compaction, model
A replacing B (RAM until a reboot), `sd:/` install and load (with `--sd`),
the Lua `pex.install`/`installed`/`uninstall` and `require` in place, and
with `--allow-reboot` (last, once) persistence and model A in place. It
uninstalls what it installed and never touches modules others installed
(compacting moves them, safely, so it needs `--allow-compact` when there are
any).

## 16. XIP (0.7, implemented)

`pex/runtime/pex_xip.c`, portable: the host describes the partition with a
`pex_flash_t` in `pex_config_t` (size, the address code is executed at, the
address it is read at, erase-sector and program-pages functions).

**Layout.** Sectors 0 and 1 hold the index, two copies; files start at
sector 2, each whole `.pex` file from a sector boundary (the image is
8-aligned in the file, so code is placed as the spec requires).

**The index** (one copy: a 64-byte header + up to 32 entries of 88 bytes,
3 KB): magic `PXI1`, a sequence number, the entry count, a move journal and a
CRC-32 over header and entries. Per entry: build id, name, first sector, file
size, version, flags, target, image offset, code and RW size, for model A the
bases its RO words carry and its RW slot, and the state (signed, verified at
install). **An update writes the copy that does not hold the current index**
(erase + program + read back); at mount the valid copy with the newer
sequence number wins. A power cut during any update leaves the previous index
in force. A RAM copy of the index lives in the pool (runtime-owned).

**Every change is copy-then-commit**: a file is written into free sectors,
verified in place, and only then entered in the index (replacing an older
build of the same name in the same commit); an uninstall is an index update.
Space no entry covers is free -- what a cut left half-written is free again.

**Code models.**
- **B**: stored exactly as signed; the build id and the signature cover the
  bytes that run; moved by a copy; any number of instances share it (RW per
  instance from the pool).
- **A**: the RO words are relocated while the file is written -- code words to
  its flash address, RW words to a **fixed RW address at the top of the
  pool** (`pool end - aoff`): at mount the runtime reserves the area every
  installed model-A module's RW needs (the first block of an empty pool lands
  at its very top). The entry records the bases, so a reader that subtracts
  them gives back the signed file: that is how the copy is verified in place,
  loaded into RAM when it cannot run in place, and read as the source of a
  move. Model A installed after the mount loads into RAM until the next boot
  (`xip_ready` false); a second instance loads into RAM (the flash copy is
  bound to one RW); a firmware whose pool moved re-relocates a fresh copy on
  the next load. The RO list must be sorted (pexlink always wrote it so; the
  install checks). Model B is what to install; model A works.
- `RAM_ONLY` files are refused; `XIP_OK` is informational.

**Compaction** (on demand, and by an install that finds enough free space
but not in one run): entries in order of position slide down into the free
space below them; one that runs from flash stays where it is. A slide by
`d` sectors of a file of `n` may overlap its old place: sector `j` goes to
`dst + j` in ascending order, which destroys source sector `j - d`, copied at
step `j - d`. **Before such a step the index records a journal** (the entry,
the destination, steps done = `j`, the new bases), so a resumed move
(`pex_xip_mount`) redoes only steps whose source is intact; a move without
overlap needs no journal. Model A is re-relocated as it is copied. The move
ends with an index commit that points the entry at its new place.

**Loading.** `pex_load_installed(name)` runs the normal loader over a reader
of the flash copy, with the code pointer in place (nothing copied, RO
relocations already applied) and RW from the pool; the CRC is checked over
the original bytes on every load; the signature was checked at install.

**Trust.** Installing verifies the file (CRC, signature, policy: an unsigned
file is never installed on a signed-only device), then verifies the written
copy again in place; `pex_xip_verify` repeats that at any time. An index
copy damaged other than by a cut (a flipped bit) falls back to the older
copy, which may name files whose sectors were reused since: such a module
fails its CRC at load and is refused (uninstall it).

**Tested.** On the build host (`pex/test/unit/pex_xip_unit.c`, ASan/UBSan):
synthetic modules of any size, a power cut (a torn erase or program) at
**every** flash operation of installs, replacements, uninstalls and an
overlapping compaction, a second cut during the recovery, damaged index
copies, a moved pool, and 3000 random operations with random cuts -- after
each cut and reboot the index is the state before or after and every file
verifies and loads (24,592 checks). In mcusim: the test hosts install and
run in place on RP2350 (through ATRANS1) and RP2040, both code models
(`run_sim.py`); the gateway firmware (`gw_sim.py`, scenario `xip`) with the
simulator SIGKILLed during installs and during an overlapping compaction --
every restart consistent, interrupted moves resumed.

**Left for later**: `stack_hint` from a call graph; `DEPS`; fat files (§4.4);
the raw `lua54_*` group; the planned HAL groups (gpio, i2c, spi, uart, pio,
dma, timer, pwm, adc, crypto); async op replies (Q11); an install that does
not block the loop (a state machine in `pex_poll`); growing the model-A RW
area without a reboot; `require` searching `sd:/`.

### 16.1 What 0.6 listed as left for XIP (for the record; all of it done in 0.7 except the last item)

- `pex_install()` (stubbed: `PEX_ENOTSUP`): write a module into the PEX flash
  partition (0x3BC000, 256 KB) and keep the index there (build id -> offset,
  size, flags, signature state); boot-time lookup so an installed module
  loads with no copy; reinstall/remove; compaction (a plain copy for model B;
  model A is re-relocated to its new address). Model A: apply the `RO` list
  for the flash address *before* programming; model B: program as-is.
- Flash writes through the firmware's flash-safe path (core 1 parked, XIP
  paused), erase granularity 4 KB, power-fail safety of the index (two copies
  + sequence numbers), and re-verifying the signature in place (model B: the
  bytes that run are the signed bytes).
- `pex_load` from an installed entry: code pointer into XIP, RW from the pool.
- Loading from `sd:/` (Q12), a larger pool (Q10), async op replies (Q11).
- `stack_hint` from a call graph; `DEPS`; fat files (§4.4); the raw `lua54_*`
  group if a module needs it; the planned HAL groups (gpio, i2c, spi, uart,
  pio, dma, timer, pwm, adc, crypto).

## 17. Changes in 0.6 (implementation feedback)

- §1: design principle -- native apps first, Lua optional (launcher/glue).
- §4.2: `image_crc32` -> `file_crc32`, covering the whole file before `SIG`
  with its own 4 bytes as zero (0.5's CRC left the header and sections
  unprotected); `entry` 0 = none; `stack_hint` is a lower bound in 1.0;
  feature and flag bits numbered; `UNLOAD_ON_EXIT` flag.
- §4.3/§4.5: `INIT`/`FINI` sections instead of `.init_array` in the image;
  exact payload layouts; IMPORTS carries a per-group minor and each slot's
  offset; EXPORTS a name table; DEBUG names imports for messages.
- §6: reloc entries of BOTH lists carry the "points into RW" bit, and the
  stored word is an offset within its target segment; model A code and RW are
  placed separately; libgcc in modules; the MOVW/MOVT advice was backwards;
  const pointer tables are refused under ROPI -- `PEX_PTR_TABLE`.
- §7: imports through generated per-symbol veneers + GOT slots; per-symbol
  weak imports; `host_api.json` in `pex/` is the source the firmware builds
  from; the v1 groups (§7.4, appendix A); the runtime without Lua (§7.5);
  the gateway's Lua is 32-bit (`LUA_32BITS`).
- §8: `pex_call` + asm trampoline, four word arguments; `PEX_APP_RUN` takes
  the config JSON; `PEX_APP_STOP`; app = running while it has tasks; unload
  rules (tasks block, host registrations are released, leaks reclaimed); the
  stack scan demoted to a debug option; hang attribution.
- §9: the launcher API first; `require` tries the PEX first; ownership by
  firmware vs VM; protected calls.
- §10, §11: key handling and tool paths as built.
- §13: Q10-Q12. §15: the gateway. §16: the XIP version.

## 18. Changes in 0.7

- §16 XIP implemented: the index (two copies, sequence numbers, a move
  journal), copy-then-commit, model B as signed, model A relocated to its
  flash address and a fixed RW slot at the pool's top, verification in place,
  compaction with journalled overlapping moves; the runtime API
  `pex_install(_mem)`, `pex_uninstall`, `pex_installed(_find/_json)`,
  `pex_load_installed`, `pex_xip_verify/compact/stats/mount`, `pex_flash_t`;
  `PEX_ENOSPC`.
- §15.1 the gateway: the static 48 KB pool (Q10, the Lua arena 116 KB, from
  measurements), the ATRANS1 window, the ops, `sd:/` (Q12), the budget and
  the flash headroom.
- §9 the Lua launcher: `pex.install`, `pex.installed`, `pex.uninstall`;
  `pex.load` and `require` prefer the installed copy.
- §8 a slice (`slice_ms`) aborts one cooperative call that runs too long and
  leaves the rest of the loop running. The watchdog remains the backstop
  when that call has interrupts masked. The gateway's budget is
  `CONFIG_PEX_SLICE_MS`.
- Q10 and Q12 decided.

## Appendix A. The v1 host API (generated)

API 1.0. Generated from `pex/host_api.json` by `pex/tools/gen_host_api.py markdown`; the JSON is authoritative.

### A.1 `core` -- group 1, v1.0

version and capability queries, time, libc string/memory helpers the compilers call implicitly

| symbol | FNV-1a | since | |
|---|---|---|---|
| `uint32_t pex_api_version(void)` | `a52df75a` | 1.0 | api_major << 16 \| api_minor of the host |
| `uint32_t pex_has(uint32_t group)` | `fc1e99b5` | 1.0 | group major << 16 \| minor if the host provides it, else 0 |
| `uint32_t pex_target(void)` | `4edbd95a` | 1.0 | the host's target id (spec section 5) |
| `uint32_t pex_ms(void)` | `fc9c0847` | 1.0 | milliseconds since boot (wraps) |
| `uint64_t pex_us(void)` | `ec5fc4ef` | 1.0 | microseconds since boot |
| `int pex_snprintf(char *dst, size_t cap, const char *fmt, ...)` | `91c115e5` | 1.0 |  |
| `int pex_vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap)` | `1bf013d1` | 1.0 |  |
| `void pex_assert_fail(const char *expr, const char *file, int line)` | `67bfca02` | 1.0 | logs and returns: asserts never stop the host |
| `void *memcpy(void *dst, const void *src, size_t n)` | `a45cec64` | 1.0 |  |
| `void *memmove(void *dst, const void *src, size_t n)` | `01081b8b` | 1.0 |  |
| `void *memset(void *dst, int c, size_t n)` | `cb80cc06` | 1.0 |  |
| `int memcmp(const void *a, const void *b, size_t n)` | `af3caa0a` | 1.0 |  |
| `size_t strlen(const char *s)` | `58ba3d97` | 1.0 |  |
| `int strcmp(const char *a, const char *b)` | `9d367094` | 1.0 |  |
| `int strncmp(const char *a, const char *b, size_t n)` | `9de3c256` | 1.0 |  |
| `char *strchr(const char *s, int c)` | `892f9553` | 1.0 |  |
| `void __aeabi_memcpy(void *dst, const void *src, size_t n)` | `19dfcf93` | 1.0 |  |
| `void __aeabi_memcpy4(void *dst, const void *src, size_t n)` | `6253e3e5` | 1.0 |  |
| `void __aeabi_memcpy8(void *dst, const void *src, size_t n)` | `6653ea31` | 1.0 |  |
| `void __aeabi_memmove(void *dst, const void *src, size_t n)` | `d34e9bd2` | 1.0 |  |
| `void __aeabi_memmove4(void *dst, const void *src, size_t n)` | `8abf6b12` | 1.0 |  |
| `void __aeabi_memmove8(void *dst, const void *src, size_t n)` | `8ebf715e` | 1.0 |  |
| `void __aeabi_memset(void *dst, size_t n, int c)` | `eabd7659` | 1.0 |  |
| `void __aeabi_memset4(void *dst, size_t n, int c)` | `f5416d97` | 1.0 |  |
| `void __aeabi_memset8(void *dst, size_t n, int c)` | `e9415ab3` | 1.0 |  |
| `void __aeabi_memclr(void *dst, size_t n)` | `2afdff6a` | 1.0 |  |
| `void __aeabi_memclr4(void *dst, size_t n)` | `0bd900fa` | 1.0 |  |
| `void __aeabi_memclr8(void *dst, size_t n)` | `ffd8ee16` | 1.0 |  |

### A.2 `log` -- group 2, v1.0

the host's log (UART/RTT/console), prefixed with the module's name

```c
#define PEX_LOG_ERR 0
#define PEX_LOG_WARN 1
#define PEX_LOG_INFO 2
#define PEX_LOG_DEBUG 3
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `void pex_log(int level, const char *fmt, ...)` | `45e3fe57` | 1.0 |  |
| `void pex_logv(int level, const char *fmt, va_list ap)` | `26e90df3` | 1.0 |  |
| `void pex_log_hex(int level, const char *what, const void *data, size_t len)` | `2e1204b5` | 1.0 |  |

### A.3 `mem` -- group 3, v1.0

lifetime-separated memory (spec 7.2): long-term from the top of the PEX pool, LIFO scratch arenas from the bottom, fixed-size pools; all of a module's long-term blocks are freed at unload

```c
typedef struct pex_arena pex_arena_t;
typedef struct pex_pool pex_pool_t;
#define PEX_MEM_DMA 1u
typedef struct {
    uint32_t long_bytes;     /* long-term bytes held (incl. pools, malloc) */
    uint32_t long_blocks;
    uint32_t scratch_bytes;  /* scratch bytes reserved right now */
    uint32_t scratch_peak;   /* high-water mark of scratch_bytes */
    uint32_t pool_free;      /* whole PEX pool: bytes free */
    uint32_t pool_largest;   /* whole pool: largest free run between the two sides */
    uint32_t pool_size;
} pex_mem_stats_t;
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `void *pex_alloc_long(size_t size)` | `b1274ac7` | 1.0 |  |
| `void *pex_alloc_ex(size_t size, uint32_t flags)` | `9055bf54` | 1.0 | PEX_MEM_DMA: 32-byte aligned, never scratch |
| `void pex_free(void *p)` | `ebbbcf95` | 1.0 |  |
| `void *pex_malloc(size_t size)` | `35714a2f` | 1.0 |  |
| `void *pex_calloc(size_t n, size_t size)` | `54556321` | 1.0 |  |
| `void *pex_realloc(void *p, size_t size)` | `efd54c0f` | 1.0 |  |
| `pex_arena_t *pex_scratch_begin(size_t cap)` | `4025d489` | 1.0 | reserve cap bytes from the scratch side; NULL if they do not fit |
| `void *pex_scratch_alloc(pex_arena_t *a, size_t size)` | `83da6339` | 1.0 | bump allocation, 8-aligned; NULL when the arena is full |
| `int pex_scratch_end(pex_arena_t *a)` | `d35d4129` | 1.0 | frees the whole arena; arenas end in reverse order (PEX_EORDER otherwise) |
| `size_t pex_scratch_avail(void)` | `9bc2eaab` | 1.0 |  |
| `pex_pool_t *pex_pool_new(size_t obj_size, uint32_t count)` | `9ab1cdb0` | 1.0 |  |
| `void *pex_pool_get(pex_pool_t *p)` | `a15e0322` | 1.0 |  |
| `void pex_pool_put(pex_pool_t *p, void *obj)` | `8dd9a8b3` | 1.0 |  |
| `void pex_pool_delete(pex_pool_t *p)` | `dd1f0bc1` | 1.0 |  |
| `int pex_mem_stats(pex_mem_stats_t *out)` | `77e73e20` | 1.0 | the calling module's accounting plus the pool's |

### A.4 `sched` -- group 4, v1.0

cooperative tasks on the host's main loop: no RTOS; callbacks must return quickly. A callback that runs longer than the host's slice (`pex_config_t.slice_ms`) is faulted and the loop continues; see §8

```c
/* Return PEX_TASK_AGAIN to stay scheduled, PEX_TASK_DONE to end the task. */
typedef int (*pex_task_fn)(void *arg);
#define PEX_TASK_AGAIN 0
#define PEX_TASK_DONE 1
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `int pex_sched_every(uint32_t period_ms, pex_task_fn fn, void *arg)` | `22f945e6` | 1.0 | task id > 0, or a negative PEX_E* code |
| `int pex_sched_after(uint32_t delay_ms, pex_task_fn fn, void *arg)` | `1c6b07ab` | 1.0 | one shot |
| `int pex_sched_post(pex_task_fn fn, void *arg)` | `b10ddd65` | 1.0 | run on the next main-loop pass |
| `int pex_sched_cancel(int id)` | `a2a03565` | 1.0 |  |
| `uint32_t pex_sched_count(void)` | `79d75482` | 1.0 | tasks the calling module has scheduled |

### A.5 `pex` -- group 5, v1.1

the module's own handle, references (spec 8) and app control; 1.1: calling another loaded module (a driver module such as otap.m33.pex) by name, through the trampoline

```c
typedef struct pex_mod pex_mod_t;
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `pex_mod_t *pex_self(void)` | `1bb7ea19` | 1.0 |  |
| `void pex_ref(pex_mod_t *m)` | `e7a65318` | 1.0 | a live reference blocks unload |
| `void pex_unref(pex_mod_t *m)` | `226345e5` | 1.0 |  |
| `const char *pex_name(pex_mod_t *m)` | `66d85c54` | 1.0 |  |
| `void pex_app_exit(int code)` | `823fd857` | 1.0 | an app ends: its tasks are cancelled after the current callback returns; UNLOAD_ON_EXIT apps are then unloaded |
| `pex_mod_t *pex_find(const char *name)` | `ece0fb7c` | 1.1 | a loaded module by instance name, or NULL (pex_ref it while using it) |
| `uint32_t pex_sym(pex_mod_t *m, const char *name)` | `75e374c6` | 1.1 | the address of one of m's EXPORTS (Thumb bit set), or 0 |
| `uint64_t pex_call(pex_mod_t *m, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)` | `4492d467` | 1.1 | call fn of module m through the trampoline (r9 = m's RW base, active count, executing-module stack); up to four word arguments, r0:r1 back; PEX_EFAULT if m faulted |

### A.6 `board` -- group 6, v1.0

board identity; optional (a host may leave it out: import it WEAK)

| symbol | FNV-1a | since | |
|---|---|---|---|
| `const char *pex_board_name(void)` | `85c90f1d` | 1.0 |  |
| `uint32_t pex_board_cpu_hz(void)` | `6c793b1f` | 1.0 |  |
| `int pex_board_uid(uint8_t *out, size_t cap)` | `83d497e4` | 1.0 | bytes written |

### A.7 `sys` -- group 7, v1.1

system facts: wall clock, host name, firmware version, memory, credential files

```c
typedef struct {
    uint32_t c_heap_free;     /* C heap (malloc) headroom, bytes */
    uint32_t lua_used, lua_size; /* the Lua arena, 0 without Lua */
    uint32_t pex_free, pex_size; /* the PEX pool */
    uint32_t stack_max;       /* deepest main stack seen */
} pex_sys_mem_t;
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `int64_t pex_sys_time(void)` | `65eb6e08` | 1.0 | Unix seconds, 0 until the clock is set (SNTP) |
| `const char *pex_sys_hostname(void)` | `33f8f0d0` | 1.0 |  |
| `const char *pex_sys_version(void)` | `8251eb9b` | 1.0 | firmware version, e.g. "1.201" |
| `int pex_sys_mem(pex_sys_mem_t *out)` | `bdaabc0a` | 1.0 |  |
| `int pex_sys_secret(const char *name, const char *key, char *out, size_t cap)` | `403510b5` | 1.1 | one top-level string from cfg/<name>.json (no slash or dot in name); length, or a negative PEX_E*. The HTTP filesystem does not serve cfg/. Same rule as Lua sys.secret |

### A.8 `json` -- group 8, v1.0

small JSON helpers so modules need no parser of their own: path lookups ("a.b.0.c") on a JSON text, and a streaming writer into a caller buffer. Portable: part of the runtime, any host can register it.

```c
typedef struct {
    char *buf;
    size_t cap, len;
    uint32_t depth, first, arr; /* per depth: first element pending, is an array */
    int err;                 /* PEX_ENOMEM once it overflowed */
} pex_jw_t;
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `const char *pex_json_find(const char *json, const char *path, size_t *len)` | `83267749` | 1.0 | the raw value at path (object, array, string with quotes, number, literal), or NULL |
| `int pex_json_str(const char *json, const char *path, char *out, size_t cap)` | `726e0d21` | 1.0 | 1 = a string (unescaped into out), 0 = absent or not a string |
| `int pex_json_int(const char *json, const char *path, int64_t *out)` | `06aefebf` | 1.0 |  |
| `int pex_json_num(const char *json, const char *path, double *out)` | `07860bc6` | 1.0 |  |
| `int pex_json_bool(const char *json, const char *path, int *out)` | `a0c7b922` | 1.0 |  |
| `int pex_json_len(const char *json, const char *path)` | `01f30f15` | 1.0 | elements of the array/object at path, -1 if none |
| `void pex_jw_init(pex_jw_t *w, char *buf, size_t cap)` | `c618b37b` | 1.0 |  |
| `void pex_jw_obj(pex_jw_t *w, const char *key)` | `7a7a4662` | 1.0 | open an object (key NULL inside an array or at the top) |
| `void pex_jw_arr(pex_jw_t *w, const char *key)` | `9a217980` | 1.0 |  |
| `void pex_jw_end(pex_jw_t *w)` | `fde289e2` | 1.0 |  |
| `void pex_jw_str(pex_jw_t *w, const char *key, const char *v)` | `b5d8b6d8` | 1.0 |  |
| `void pex_jw_strn(pex_jw_t *w, const char *key, const char *v, size_t n)` | `fa27a082` | 1.0 |  |
| `void pex_jw_int(pex_jw_t *w, const char *key, int64_t v)` | `39952966` | 1.0 |  |
| `void pex_jw_num(pex_jw_t *w, const char *key, double v)` | `e1f5b1db` | 1.0 |  |
| `void pex_jw_bool(pex_jw_t *w, const char *key, int v)` | `7d78f7c5` | 1.0 |  |
| `void pex_jw_null(pex_jw_t *w, const char *key)` | `46c448ac` | 1.0 |  |
| `void pex_jw_raw(pex_jw_t *w, const char *key, const char *json)` | `178bc5d9` | 1.0 | a value that is JSON already |
| `void pex_jw_hex(pex_jw_t *w, const char *key, const void *data, size_t n)` | `b1e9a032` | 1.0 | bytes as a hex string |
| `int pex_jw_finish(pex_jw_t *w)` | `3388bc0a` | 1.0 | length, or PEX_ENOMEM if it did not fit |

### A.9 `bus` -- group 9, v1.0

serve the web UI directly: register ops (request JSON in, reply JSON out, synchronous) and emit events; a module's ops go away at unload

```c
/* Write the reply (a JSON value) into reply[0..cap) and return its length;
   or return a negative PEX_E* with an error message in reply. */
typedef int (*pex_op_fn)(void *arg, const char *params_json, char *reply, size_t cap);
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `int pex_bus_op(const char *name, pex_op_fn fn, void *arg)` | `cc8e80ef` | 1.0 | register op "ns.name"; built-in and Lua op names are refused |
| `int pex_bus_op_remove(const char *name)` | `6b573198` | 1.0 |  |
| `int pex_bus_emit(const char *topic, const char *json)` | `56ffcf23` | 1.0 | broadcast {"evt":topic,"data":json} to every WebSocket client |

### A.10 `fs` -- group 10, v1.0

files on lfs:/ (LittleFS) and sd:/ (the SD card) with the Lua file module's semantics. LittleFS calls complete synchronously; the card is asynchronous: pass a callback (it runs from the main loop later). With cb == NULL a call is synchronous and sd:/ answers PEX_EAGAIN.

```c
#define PEX_FS_READ 0
#define PEX_FS_WRITE 1   /* create/truncate, parents created */
#define PEX_FS_APPEND 2
typedef void (*pex_fs_cb)(void *arg, int rc);  /* rc: result (>= 0) or PEX_E* */
typedef struct { char name[65]; uint8_t is_dir; uint32_t size; } pex_fs_entry_t;
typedef void (*pex_fs_list_cb)(void *arg, const pex_fs_entry_t *e);  /* e == NULL: the end */
typedef struct { int lfs_ok; uint32_t lfs_free, lfs_total; char sd_state[12]; uint64_t sd_free, sd_total; } pex_fs_media_t;
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `int pex_fs_open(const char *path, int mode, pex_fs_cb cb, void *arg)` | `f52ecee1` | 1.0 | a file handle (>= 0) |
| `int pex_fs_read(int fd, void *buf, size_t n, pex_fs_cb cb, void *arg)` | `ea91349d` | 1.0 | bytes read (0 = end) |
| `int pex_fs_write(int fd, const void *buf, size_t n, pex_fs_cb cb, void *arg)` | `77a32654` | 1.0 | bytes written; buf is copied before the call returns |
| `int pex_fs_seek(int fd, uint32_t pos, pex_fs_cb cb, void *arg)` | `611c3a19` | 1.0 |  |
| `int32_t pex_fs_size(int fd)` | `71fd8cf4` | 1.0 |  |
| `int pex_fs_close(int fd)` | `b1874acb` | 1.0 |  |
| `int pex_fs_stat(const char *path, pex_fs_entry_t *out, pex_fs_cb cb, void *arg)` | `cf8bf4cf` | 1.0 |  |
| `int pex_fs_list(const char *dir, pex_fs_list_cb each, void *arg)` | `ec48c919` | 1.0 | entries, then each(arg, NULL); sync on lfs:/, from the main loop on sd:/ |
| `int pex_fs_mkdir(const char *path, pex_fs_cb cb, void *arg)` | `d4324570` | 1.0 |  |
| `int pex_fs_remove(const char *path, pex_fs_cb cb, void *arg)` | `09a95415` | 1.0 |  |
| `int pex_fs_rename(const char *from, const char *to, pex_fs_cb cb, void *arg)` | `453fb04b` | 1.0 |  |
| `int pex_fs_media(pex_fs_media_t *out)` | `0f1a5e0b` | 1.0 |  |

### A.11 `net` -- group 11, v1.0

the firmware's HTTP(S) client: start, status, cancel; the body into a module buffer or straight into a file (lfs:/)

```c
typedef struct {
    const char *method;      /* NULL = GET */
    const char *url;
    const char *headers;     /* "Name: value\r\n" lines, or NULL */
    const void *body; size_t body_len;
    uint32_t timeout_ms;     /* 0 = 30 s */
    uint8_t follow;          /* redirects to follow */
    uint8_t insecure;        /* skip certificate checks */
    void *buf; size_t buf_cap; /* the body goes here (truncated past cap) ... */
    const char *to_file;     /* ... or into this file (lfs:/...) */
} pex_http_req_t;
typedef struct { int status; int err; uint32_t bytes; int64_t total; uint8_t done, truncated; char msg[48]; } pex_http_res_t;
typedef void (*pex_http_cb)(void *arg, const pex_http_res_t *res);
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `int pex_http_start(const pex_http_req_t *req, pex_http_cb done, void *arg)` | `1cb6ef04` | 1.0 | request id (> 0); done runs once from the main loop. A User-Agent line in headers replaces the firmware's own (fw 1.231+) |
| `int pex_http_status(int id, pex_http_res_t *out)` | `48b3c23a` | 1.0 |  |
| `int pex_http_cancel(int id)` | `49e97eb2` | 1.0 |  |

### A.12 `bt` -- group 12, v1.0

Bluetooth LE central, the operations of Lua's bt.*: the scan table, connect, discovered GATT, read/write/subscribe; values, write completions and link changes arrive as events through a listener (from the main loop)

```c
typedef struct {
    uint8_t addr[6]; uint8_t addr_type; int8_t rssi;
    char name[31]; uint8_t connectable, connected, n_uuid16;
    uint16_t uuid16[4];
    uint8_t mfg_len; uint8_t mfg[31];
    uint32_t age_ms;
} pex_bt_dev_t;
#define PEX_BT_NOTIFY 1
#define PEX_BT_INDICATE 2
#define PEX_BT_READ 3
#define PEX_BT_WRITE 4      /* a write completed: status = att_error | reason << 8 (0 = ok) */
#define PEX_BT_LINK_UP 16
#define PEX_BT_LINK_DOWN 17
#define PEX_BT_READY 18     /* GATT discovered: pex_bt_find works */
#define PEX_BT_FAILED 19    /* connect or discovery failed */
typedef struct {
    uint8_t addr[6]; uint8_t kind; uint8_t pad;
    int16_t chr; int16_t status;
    uint16_t len; const uint8_t *data;
} pex_bt_event_t;
typedef void (*pex_bt_fn)(void *arg, const pex_bt_event_t *ev);
#define PEX_BT_DISCONNECTED 0
#define PEX_BT_CONNECTING 1
#define PEX_BT_CONNECTED 2
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `int pex_bt_scan(uint32_t duration_ms, int active)` | `28bd958f` | 1.0 | 0 = until pex_bt_scan_stop |
| `void pex_bt_scan_stop(void)` | `55276ce8` | 1.0 |  |
| `int pex_bt_scanning(void)` | `97771765` | 1.0 |  |
| `int pex_bt_devices(pex_bt_dev_t *out, uint32_t max, const char *name_prefix, const char *uuid128)` | `faf0eced` | 1.0 | the scan table, filtered (either rule matches; NULLs = all); count |
| `int pex_bt_uuids128(const uint8_t *addr, char (*out)[37], int max)` | `587ac1fb` | 1.0 |  |
| `int pex_bt_addr(const char *str, uint8_t *addr)` | `572b4ac9` | 1.0 | parse "AA:BB:.." |
| `void pex_bt_addr_str(const uint8_t *addr, char *out)` | `ba7cf7e5` | 1.0 | 18 bytes |
| `int pex_bt_connect(const uint8_t *addr)` | `8395a478` | 1.0 | asynchronous: PEX_BT_LINK_UP then PEX_BT_READY (or PEX_BT_FAILED: also a connect the controller reports as failed or cancelled -- no link is made, fw 1.234+) |
| `int pex_bt_disconnect(const uint8_t *addr)` | `cd6cf5fc` | 1.0 |  |
| `int pex_bt_state(const uint8_t *addr, int *ready)` | `8610e32f` | 1.0 |  |
| `int pex_bt_mtu(const uint8_t *addr)` | `a11b2112` | 1.0 |  |
| `int pex_bt_find(const uint8_t *addr, const char *svc_uuid, const char *chr_uuid, int instance)` | `38866cfd` | 1.0 | a characteristic id (>= 0) or PEX_ENOENT |
| `int pex_bt_props(const uint8_t *addr, int chr)` | `2368de80` | 1.0 | ATT property bits |
| `int pex_bt_read(const uint8_t *addr, int chr)` | `07700cc2` | 1.0 | the value comes as a PEX_BT_READ event |
| `int pex_bt_write(const uint8_t *addr, int chr, const void *data, size_t len, int with_response, int want_done)` | `7c740ca5` | 1.0 | want_done: a PEX_BT_WRITE event reports completion; len at most MTU - 3 and 252 (PEX_EINVAL beyond; 244 before fw 1.233) |
| `int pex_bt_subscribe(const uint8_t *addr, int chr, int mode)` | `ee8e0976` | 1.0 | 0 off, 1 notify, 2 indicate |
| `int pex_bt_listen(const uint8_t *addr, pex_bt_fn fn, void *arg)` | `ffc11b85` | 1.0 | events of one device (addr NULL: all); id |
| `int pex_bt_unlisten(int id)` | `1f584f1a` | 1.0 |  |

### A.13 `codec` -- group 13, v1.1

The firmware's Anker frame, TLV and session crypto, the same C Lua's frame, tlv and crypto tables call. Present only when the firmware is built with PROTO_ANKER_CRYPTO. A module must not link uECC or tiny-AES. GCM decrypt does not check the tag.

```c
#define PEX_CODEC_REASM_SIZE 680
#define PEX_CODEC_TLV_STRING 0
#define PEX_CODEC_TLV_U8 1
#define PEX_CODEC_TLV_U16 2
#define PEX_CODEC_TLV_U32 3
#define PEX_CODEC_TLV_F32 4
#define PEX_CODEC_TLV_BYTES 5
#define PEX_CODEC_TLV_UNKNOWN 6
#define PEX_CODEC_ENC_STRING 0
#define PEX_CODEC_ENC_U8 1
#define PEX_CODEC_ENC_U16 2
#define PEX_CODEC_ENC_U32 3
#define PEX_CODEC_ENC_F32 4
#define PEX_CODEC_ENC_BYTES 5
typedef struct {
    uint8_t version, slave_id, function_type;
    uint8_t encrypted, fragmented, is_response;
    uint8_t frag_index, frag_count, checksum_ok, length_ok;
    uint16_t opcode, payload_len;
    const uint8_t *payload; /* into the caller's buffer, after a frag header */
} pex_codec_frame_t;
typedef struct {
    uint8_t tag, type;       /* type is PEX_CODEC_TLV_* */
    int16_t type_byte;       /* -1 when the item has none */
    uint16_t raw_len, val_len;
    int64_t ival;
    const uint8_t *raw;      /* charging: includes the type byte; negotiation: the value */
    const uint8_t *val;
} pex_codec_tlv_t;
typedef struct {
    uint8_t tag, enc;        /* enc is PEX_CODEC_ENC_*; not the type byte */
    int16_t type_byte;       /* -2 omit, -1 default for enc, >= 0 explicit */
    int64_t ival;
    const uint8_t *data;
    uint16_t data_len;
} pex_codec_spec_t;
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `int pex_codec_frame_parse(const uint8_t *buf, size_t len, pex_codec_frame_t *out)` | `43877059` | 1.0 | 1 if the buffer is an FF09 frame (checksum_ok is separate), 0 if not, or a negative PEX_E* |
| `int pex_codec_frame_build(uint8_t function_type, uint16_t opcode, int encrypted, uint8_t slave_id, const uint8_t *payload, size_t n, uint8_t *out, size_t cap)` | `830f03de` | 1.0 | frame length, or a negative PEX_E* |
| `void pex_codec_reasm_reset(void *st)` | `c3f3c6cc` | 1.0 | st is PEX_CODEC_REASM_SIZE bytes, owned by the caller |
| `int pex_codec_reasm_push(void *st, const pex_codec_frame_t *f, const uint8_t **out, size_t *outlen)` | `44484751` | 1.0 | 0 while a fragmented frame is incomplete; else the payload length. *out points at the caller's frame or into st: copy it before returning |
| `int pex_codec_tlv_next(const uint8_t **body, size_t *len, pex_codec_tlv_t *out)` | `ebc17dc6` | 1.0 | one charging-dialect item; advances *body. 1, or 0 at the end. raw includes the type byte |
| `int pex_codec_tlv_neg(const uint8_t **body, size_t *len, pex_codec_tlv_t *out)` | `c137b6b7` | 1.0 | one negotiation-dialect item (no type byte). raw is the value |
| `int pex_codec_tlv_build(const pex_codec_spec_t *specs, size_t n, uint8_t *out, size_t cap)` | `8e8587ed` | 1.0 | bytes written. type_byte -2 omits the type byte, so a u8 0x21 is tag, len 1, 0x21 |
| `int pex_codec_pubkey(const uint8_t *d, uint8_t *xy)` | `e4e7a22e` | 1.0 | d is 32 bytes, xy is X\|\|Y 64 bytes; 64 or a negative PEX_E*. The private scalar is the caller's |
| `int pex_codec_ecdh(const uint8_t *d, const uint8_t *peer, size_t peer_len, uint8_t *Z)` | `c6564958` | 1.0 | Z is the 32-byte X coordinate. peer is 04\|\|X\|\|Y or bare X\|\|Y. About 40 ms. 32 or a negative PEX_E* |
| `int pex_codec_gcm_encrypt(const uint8_t *key, size_t keylen, const uint8_t *iv12, const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t n, uint8_t *out, size_t cap)` | `32734c6f` | 1.0 | ciphertext \|\| tag; returns n + 16 |
| `int pex_codec_gcm_decrypt(const uint8_t *key, size_t keylen, const uint8_t *iv12, const uint8_t *body, size_t n, uint8_t *out, size_t cap)` | `10fe8c83` | 1.0 | does NOT check the tag. body is ciphertext \|\| tag. plaintext length |
| `void pex_codec_gcm_tag(const uint8_t *key, size_t keylen, const uint8_t *iv12, const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *tag)` | `cc46bff6` | 1.1 | the 16-byte tag a conforming implementation would produce. Decrypt never checks one. Same C as Lua crypto.gcm_tag |
| `int pex_codec_cbc(int encrypt, const uint8_t *key, size_t keylen, const uint8_t *iv, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *outlen)` | `2ebc3536` | 1.0 | AES-CBC. Encrypt PKCS7-pads. Decrypt unpads leniently. iv is 16 bytes. byte count or a negative PEX_E* |
| `int pex_codec_kdf(const uint8_t *Z, uint8_t *out, size_t outlen)` | `acd2235b` | 1.0 | Anker ECIES KDF. outlen bytes, or a negative PEX_E* |
| `int pex_codec_account_key(const char *id, uint8_t *key, uint8_t *iv)` | `be91f92f` | 1.0 | AesConferV1: key is the id padded to 32, iv is 16 zero bytes. 32 or a negative PEX_E* |
| `void pex_codec_ghash(const uint8_t *H, const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *out)` | `c62e1321` | 1.0 | GHASH(H, aad, ct) -> 16 bytes. The negotiation tag is this XOR a recovered E(J0); the constants stay in the module |

### A.16 `lua` -- group 16, v1.0

pexlua: the curated, Lua-version-independent bridge (spec 7.3 option C); only present on hosts built with Lua, so import it WEAK unless the module is Lua-only. Errors raised here unwind the module call safely (the bridge calls modules under protection).

```c
typedef struct lua_State lua_State;
typedef int (*pexlua_fn)(lua_State *L);
typedef struct { const char *name; pexlua_fn fn; } pexlua_reg_t;
#define PEXLUA_TNONE (-1)
#define PEXLUA_TNIL 0
#define PEXLUA_TBOOLEAN 1
#define PEXLUA_TLIGHTUSERDATA 2
#define PEXLUA_TNUMBER 3
#define PEXLUA_TSTRING 4
#define PEXLUA_TTABLE 5
#define PEXLUA_TFUNCTION 6
```

| symbol | FNV-1a | since | |
|---|---|---|---|
| `int pexlua_gettop(lua_State *L)` | `8f6df23a` | 1.0 |  |
| `void pexlua_settop(lua_State *L, int idx)` | `9e0a5ed6` | 1.0 |  |
| `void pexlua_pushvalue(lua_State *L, int idx)` | `8ca85f2e` | 1.0 |  |
| `int pexlua_type(lua_State *L, int idx)` | `bd239d59` | 1.0 |  |
| `int64_t pexlua_checkint(lua_State *L, int arg)` | `1ae1135c` | 1.0 |  |
| `int64_t pexlua_optint(lua_State *L, int arg, int64_t def)` | `dd7beeaf` | 1.0 |  |
| `double pexlua_checknum(lua_State *L, int arg)` | `7d95d561` | 1.0 |  |
| `const char *pexlua_checkstr(lua_State *L, int arg, size_t *len)` | `21284fea` | 1.0 |  |
| `const char *pexlua_optstr(lua_State *L, int arg, const char *def, size_t *len)` | `ab744fd1` | 1.0 |  |
| `int pexlua_toboolean(lua_State *L, int idx)` | `d014ab2c` | 1.0 |  |
| `void pexlua_checktype(lua_State *L, int arg, int type)` | `1abbda8b` | 1.0 |  |
| `void pexlua_pushint(lua_State *L, int64_t v)` | `6071fae2` | 1.0 |  |
| `void pexlua_pushnum(lua_State *L, double v)` | `6f920caf` | 1.0 |  |
| `void pexlua_pushstr(lua_State *L, const char *s)` | `a484c1fc` | 1.0 |  |
| `void pexlua_pushbytes(lua_State *L, const void *p, size_t len)` | `894d2d38` | 1.0 |  |
| `void pexlua_pushbool(lua_State *L, int b)` | `1ffb6761` | 1.0 |  |
| `void pexlua_pushnil(lua_State *L)` | `6eae8d30` | 1.0 |  |
| `void pexlua_newtable(lua_State *L, int narr, int nrec)` | `610cd697` | 1.0 |  |
| `void pexlua_setfield(lua_State *L, int idx, const char *k)` | `5f43e0a9` | 1.0 | t[k] = top, pops it |
| `int pexlua_getfield(lua_State *L, int idx, const char *k)` | `01dc255d` | 1.0 | pushes t[k], returns its type |
| `void pexlua_seti(lua_State *L, int idx, int64_t i)` | `4f894c3a` | 1.0 |  |
| `int pexlua_geti(lua_State *L, int idx, int64_t i)` | `1b0eb17e` | 1.0 |  |
| `size_t pexlua_rawlen(lua_State *L, int idx)` | `d4ce40c4` | 1.0 |  |
| `void pexlua_register(lua_State *L, const pexlua_reg_t *fns, uint32_t n)` | `f63d32fc` | 1.0 | adds n functions to the table on top; each call enters the module through the trampoline and holds a reference until collected |
| `int pexlua_error(lua_State *L, const char *msg)` | `b1415ad5` | 1.0 | raises a Lua error; does not return |
| `int pexlua_argerror(lua_State *L, int arg, const char *msg)` | `9334d3fb` | 1.0 |  |

