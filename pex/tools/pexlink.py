#!/usr/bin/env python3
"""pexlink -- ELF (linked with a PEX linker script) -> .pex  (spec docs/pex-format.md)

  pexlink.py link  <module.elf> -o <out.pex> --target m33|m0p|c5 --model reloc|pic
                   --api pex/host_api.json [--name N] [--version 1.2.3]
                   [--kind lib|app|lua] [--desc TEXT] [--flags reentrant,unload-on-exit,
                   needs-sig,ram-only,xip-ok] [--su FILE.su ...] [--stack-hint N]
                   [--sign] [--key PEM] [--key-id 1] [--no-debug]
  pexlink.py sign  <in.pex> -o <out.pex> [--key PEM] [--key-id 1]
  pexlink.py dump  <file.pex> [--api host_api.json] [--verify-key PEM] [--relocs]
  pexlink.py --dump <file.pex>                (same as dump)

The ELF must come from pex_add_module() (pex/sdk/cmake/pex.cmake): the PEX
linker script, --emit-relocs, --gc-sections, the import veneers generated
from host_api.json. pexlink then classifies EVERY relocation the linker kept:

  model A (reloc): absolute words in text/rodata -> RELOC.RO; in data -> RELOC.RW;
                   PC-relative references inside the code are final already.
  model B (pic):   text/rodata must hold NO absolute word (ROPI): any is refused
                   with the offending symbol; data words -> RELOC.RW; r9-relative
                   (SBREL) references are final already.

Anything else (absolute MOVW/MOVT, GOT/TLS forms, calls to undefined symbols,
PC-relative references between the code and RW segments) is refused with
the flag that avoids it. The signature reuses tools/ota_sign.py (same key
handling: --key, else $BTGW_OTA_KEY, else ~/.config/btgw/ota-signing.pem).
"""
import argparse, hashlib, json, os, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = "pexlink 1.1"

MAGIC = b"PEX\x1a"
FORMAT_MAJOR, FORMAT_MINOR = 1, 1
HEADER_SIZE = 92
CRC_OFF = 72

TARGETS = {"m0p": 0x0001, "m33": 0x0002, "m23": 0x0003, "m4f": 0x0004, "c5": 0x0103}
TARGET_NAMES = {v: k for k, v in TARGETS.items()}
FEATURES = {"m33": 0b101, "m0p": 0, "c5": 0}   # FPU_SP | DSP for the M33 build; c5 has no FPU
FEAT_NAMES = ["fpu-sp", "fpu-dp", "dsp", "cmse"]

F = dict(pic=1 << 0, xip_ok=1 << 1, ram_only=1 << 2, lua_module=1 << 3, app=1 << 4,
         reentrant=1 << 5, needs_sig=1 << 6, unload_on_exit=1 << 7)

SEC = dict(RELOC=1, IMPORTS=2, EXPORTS=3, META=4, DEPS=5, DEBUG=6, INIT=7, FINI=8, SIG=0xF0)
SEC_NAMES = {v: k for k, v in SEC.items()}
OPTIONAL = 1
TO_RW = 0x80000000
WEAK = 1

META_TAGS = dict(name=1, description=2, author=3, url=4, license=5, lua_name=6, min_ram=7,
                 version=8, tool=9)
META_NAMES = {v: k for k, v in META_TAGS.items()}

# ---- ARM relocation types (ELF for the Arm Architecture, table 5-6) --------
R = dict(NONE=0, ABS32=2, REL32=3, THM_ABS5=7, ABS8=8, SBREL32=9, THM_CALL=10, THM_PC8=11,
         TARGET1=38, V4BX=40, TARGET2=41, PREL31=42, MOVW_ABS_NC=43, MOVT_ABS=44,
         THM_MOVW_ABS_NC=47, THM_MOVT_ABS=48, THM_MOVW_PREL_NC=49, THM_MOVT_PREL=50,
         THM_JUMP19=51, THM_JUMP6=52, THM_ALU_PREL_11_0=53, THM_PC12=54,
         THM_JUMP24=30, GOTOFF32=24, BASE_PREL=25, GOT_BREL=26, THM_MOVW_BREL_NC=87,
         THM_MOVT_BREL=88, THM_MOVW_BREL=89, GOT_PREL=96, THM_JUMP11=102, THM_JUMP8=103,
         THM_ALU_ABS_G0_NC=132, THM_ALU_ABS_G1_NC=133, THM_ALU_ABS_G2_NC=134, THM_ALU_ABS_G3=135)
RNAME = {v: k for k, v in R.items()}
PCREL = {R["REL32"], R["THM_CALL"], R["THM_PC8"], R["THM_MOVW_PREL_NC"], R["THM_MOVT_PREL"],
         R["THM_JUMP19"], R["THM_JUMP6"], R["THM_ALU_PREL_11_0"], R["THM_PC12"], R["THM_JUMP24"],
         R["THM_JUMP11"], R["THM_JUMP8"]}
SBREL = {R["SBREL32"], R["THM_MOVW_BREL_NC"], R["THM_MOVT_BREL"], R["THM_MOVW_BREL"]}
ABS = {R["ABS32"], R["TARGET1"]}
IGNORE = {R["NONE"], R["V4BX"]}
WHY = {
    R["THM_MOVW_ABS_NC"]: "an absolute MOVW/MOVT address pair; drop -mslow-flash-data/-mpure-code "
                          "(literal-pool words are what model A relocates), or build MODEL pic",
    R["THM_MOVT_ABS"]: "an absolute MOVW/MOVT address pair; drop -mslow-flash-data/-mpure-code, or build MODEL pic",
    R["MOVW_ABS_NC"]: "ARM-state code; PEX is Thumb only (-mthumb)",
    R["MOVT_ABS"]: "ARM-state code; PEX is Thumb only (-mthumb)",
    R["GOT_BREL"]: "a GOT reference; do not build with -fPIC/-fpic (model B is -fropi -frwpi, MODEL pic)",
    R["GOT_PREL"]: "a GOT reference; do not build with -fPIC/-fpic (model B is -fropi -frwpi, MODEL pic)",
    R["GOTOFF32"]: "a GOT-relative reference; do not build with -fPIC/-fpic",
    R["BASE_PREL"]: "a GOT base reference; do not build with -fPIC/-fpic",
    R["TARGET2"]: "C++ exception tables; build with -fno-exceptions -fno-unwind-tables",
    R["PREL31"]: "unwind tables; build with -fno-unwind-tables -fno-asynchronous-unwind-tables",
    R["THM_ALU_ABS_G0_NC"]: "Armv6-M execute-only absolute addressing; drop -mpure-code",
    R["THM_ALU_ABS_G1_NC"]: "Armv6-M execute-only absolute addressing; drop -mpure-code",
    R["THM_ALU_ABS_G2_NC"]: "Armv6-M execute-only absolute addressing; drop -mpure-code",
    R["THM_ALU_ABS_G3"]: "Armv6-M execute-only absolute addressing; drop -mpure-code",
}


class LinkError(Exception):
    pass


def fnv1a(s):
    h = 0x811C9DC5
    for b in s.encode():
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def crc32_file(buf):
    """CRC-32 over the file before SIG with the crc field taken as zero (spec 4.2, 0.6)."""
    b = bytearray(buf)
    b[CRC_OFF:CRC_OFF + 4] = b"\0\0\0\0"
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def al(x, a):
    return (x + a - 1) & ~(a - 1)


# ---------------------------------------------------------------- ELF32 reader
class Elf:
    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise LinkError(f"{path}: not a little-endian ELF32 file")
        (self.type, self.machine, _, self.entry, _, shoff, self.flags, _, _, _, shentsize, shnum,
         shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        if self.machine not in (40, 243):
            raise LinkError(f"{path}: not an ARM (40) or RISC-V (243) ELF, machine {self.machine}")
        self.sh = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
                "<IIIIIIIIII", d, shoff + i * shentsize)
            self.sh.append(dict(i=i, name_off=name, type=typ, flags=flags, addr=addr, off=off,
                                size=size, link=link, info=info, align=align, entsize=entsize))
        strtab = self.sh[shstrndx]
        for s in self.sh:
            s["name"] = self.cstr(strtab["off"] + s["name_off"])
        self.by_name = {s["name"]: s for s in self.sh}
        self.syms = []
        for s in self.sh:
            if s["type"] == 2:      # SHT_SYMTAB
                st = self.sh[s["link"]]
                for k in range(s["size"] // 16):
                    nm, val, size, info, other, shndx = struct.unpack_from("<IIIBBH", d, s["off"] + k * 16)
                    self.syms.append(dict(name=self.cstr(st["off"] + nm), value=val, size=size,
                                          bind=info >> 4, type=info & 15, shndx=shndx, other=other))

    def cstr(self, off):
        e = self.d.index(b"\0", off)
        return self.d[off:e].decode(errors="replace")

    def data(self, s):
        if s["type"] == 8:          # NOBITS
            return b"\0" * s["size"]
        return self.d[s["off"]:s["off"] + s["size"]]

    def sym(self, name):
        for s in self.syms:
            if s["name"] == name:
                return s
        return None


# RISC-V ELF32. Calls and auipc/lo12 pairs are already applied by ld; the
# loader only fixes R_RISCV_32 words. HI20/LO12 are absolute and would be
# wrong once the span moves, so a medlow or relaxing build fails here.
# gcc emits R_RISCV_CALL_PLT (19) for every call to a global symbol. With
# --emit-relocs that type survives even after ld rewrites the auipc+jalr
# into a direct call inside this image. There is no PLT in that case.
RV_PCREL = {16, 17, 18, 23, 24, 25}          # BRANCH JAL CALL PCREL_HI20 LO12_I LO12_S
RV_IGNORE = {0, 51}                           # NONE, RELAX (bytes are final with -mno-relax)
RV_BAD = {
    26: "R_RISCV_HI20; build -mcmodel=medany so addresses are R_RISCV_32 words",
    27: "R_RISCV_LO12_I; build -mcmodel=medany",
    28: "R_RISCV_LO12_S; build -mcmodel=medany",
    43: "R_RISCV_ALIGN; relaxation is on. Pass -mno-relax",
}


def collect_riscv(elf, img, code_end, rw_vma, rw_size, ro_list, rw_list):
    errors = []

    def seg_of_shndx(ndx):
        if ndx == 0:
            return "undef"
        if ndx >= 0xFF00:
            return "abs"
        n = elf.sh[ndx]["name"]
        if n in (".text", ".rodata"):
            return "code"
        if n in (".data", ".bss", ".sdata", ".sbss"):
            return "rw"
        if n.startswith(".pex."):
            return "meta"
        return "other:" + n

    for rs in elf.sh:
        if rs["type"] == 9:
            errors.append(f"{rs['name']}: REL relocations are not expected on RISC-V (want RELA)")
            continue
        if rs["type"] != 4:
            continue
        tgt = elf.sh[rs["info"]]
        tname = tgt["name"]
        if tname.startswith(".pex.") or not (tgt["flags"] & 2):
            continue
        place_seg = "code" if tname in (".text", ".rodata") else "rw" if tname in (".data", ".sdata") else None
        if place_seg is None:
            errors.append(f"relocations in unexpected section {tname}")
            continue
        symtab = elf.sh[rs["link"]]
        for k in range(rs["size"] // 12):
            r_off, r_info, r_addend = struct.unpack_from("<IIi", elf.d, rs["off"] + k * 12)
            typ, si = r_info & 0xFF, r_info >> 8
            nm, val, size, info, other, shndx = struct.unpack_from("<IIIBBH", elf.d, symtab["off"] + si * 16)
            sname = elf.cstr(elf.sh[symtab["link"]]["off"] + nm) or (
                elf.sh[shndx]["name"] if 0 < shndx < 0xFF00 else "?")
            bind = info >> 4
            sseg = seg_of_shndx(shndx)
            at = f"{tname}+0x{r_off:x}"
            if typ in RV_IGNORE or typ in RV_PCREL:
                continue
            if typ == 19:  # CALL_PLT: ignore a resolved in-image call; reject a real PLT
                plt = 0 < shndx < 0xFF00 and elf.sh[shndx]["name"].startswith(".plt")
                if sseg == "undef" or plt:
                    errors.append(
                        f"{at}: R_RISCV_CALL_PLT to '{sname}' needs a PLT this loader does not copy")
                continue
            if typ in RV_BAD:
                errors.append(f"{at}: {RV_BAD[typ]} (against '{sname}')")
                continue
            if sseg == "undef":
                if bind == 2 and typ == 1:
                    continue
                errors.append(f"{at}: relocation {typ} to undefined symbol '{sname}'")
                continue
            if typ != 1:
                errors.append(f"{at}: unsupported relocation R_RISCV type {typ} against '{sname}'")
                continue
            if sseg == "abs":
                errors.append(f"{at}: absolute word refers to the absolute symbol '{sname}'")
                continue
            if sseg not in ("code", "rw"):
                errors.append(f"{at}: absolute word refers to '{sname}' in {sseg}")
                continue
            w_off = r_off if place_seg == "code" else code_end + (r_off - rw_vma)
            v = struct.unpack_from("<I", img, w_off)[0]
            if v == 0 and r_addend:
                v = r_addend & 0xFFFFFFFF
                struct.pack_into("<I", img, w_off, v)
            if sseg == "rw":
                nv = v - rw_vma
                if not (0 <= nv <= rw_size):
                    errors.append(f"{at}: word 0x{v:08x} to '{sname}' lies outside RW")
                    continue
                struct.pack_into("<I", img, w_off, nv)
            elif not (0 <= v <= code_end):
                errors.append(f"{at}: word 0x{v:08x} to '{sname}' lies outside the code")
                continue
            bit = TO_RW if sseg == "rw" else 0
            if place_seg == "code":
                ro_list.append(r_off | bit)
            else:
                rw_list.append((r_off - rw_vma) | bit)
    return errors


# ---------------------------------------------------------------- linking
def load_api(path):
    api = json.load(open(path))
    by_name = {}
    for g in api["groups"]:
        for f in g.get("functions", []):
            by_name[f["name"]] = (g, f)
    return api, by_name


def build(a):
    elf = Elf(a.elf)
    model = a.model
    api, api_syms = load_api(a.api)
    S = elf.by_name
    alloc = [s for s in elf.sh if s["flags"] & 2 and s["size"]]
    known = {".text", ".rodata", ".data", ".bss", ".pex.exports", ".pex.weak", ".pex.meta",
             ".pex.init", ".pex.fini"}
    for s in alloc:
        if s["name"] not in known:
            raise LinkError(f"unexpected allocated section {s['name']} ({s['size']} bytes): the PEX "
                            f"linker script only places .text/.rodata/.data/.bss -- link with "
                            f"pex/sdk/ld/pex_{a.target}_{model}.ld")

    def sec(name):
        return S.get(name) if S.get(name) and S[name]["size"] else None

    text = sec(".text")
    if text is None or text["addr"] != 0:
        raise LinkError(".text must exist and start at address 0")
    ro = sec(".rodata")
    data, bss = sec(".data"), sec(".bss")
    rw_sym = elf.sym("__pex_rw_start")
    if rw_sym is None:
        raise LinkError("no __pex_rw_start: not linked with a PEX linker script")
    rw_vma = rw_sym["value"]
    text_size = ro["addr"] if ro else al(text["size"], 4)
    code_end = (ro["addr"] + ro["size"]) if ro else text_size
    code_end = al(code_end, 4)
    ro_size = code_end - text_size
    data_size = data["size"] if data else 0
    if data and data["addr"] != rw_vma:
        raise LinkError(".data does not start at __pex_rw_start")
    end_sym = elf.sym("__pex_rw_end")
    rw_end = end_sym["value"] if end_sym else rw_vma + data_size
    bss_size = rw_end - rw_vma - data_size
    if model == "reloc" and rw_vma < code_end:
        raise LinkError("model A: RW overlaps the code")
    if a.target == "c5" and rw_vma != code_end:
        raise LinkError(f"c5: .data must start where the code ends "
                        f"(0x{rw_vma:x} != 0x{code_end:x}); the load is one contiguous span")
    rw_size = data_size + bss_size

    # The image: text | rodata | data, as one buffer.
    img = bytearray(code_end + data_size)
    img[0:text["size"]] = elf.data(text)
    if ro:
        img[ro["addr"]:ro["addr"] + ro["size"]] = elf.data(ro)
    if data:
        img[code_end:code_end + data_size] = elf.data(data)

    def seg_of_addr(addr, size=4):
        if 0 <= addr and addr + size <= code_end:
            return "code"
        if rw_vma <= addr and addr + size <= rw_vma + rw_size:
            return "rw"
        return None

    def seg_of_shndx(ndx):
        if ndx == 0:
            return "undef"
        if ndx >= 0xFF00:
            return "abs"
        n = elf.sh[ndx]["name"]
        if n in (".text", ".rodata"):
            return "code"
        if n in (".data", ".bss"):
            return "rw"
        if n.startswith(".pex."):
            return "meta"
        return "other:" + n

    def where(sname, off):
        """'func+0x12' for messages."""
        best = None
        for s in elf.syms:
            if s["type"] in (1, 2) and s["shndx"] and s["shndx"] < 0xFF00:
                v = s["value"] & ~1 if s["type"] == 2 else s["value"]
                if v <= off < v + max(s["size"], 1) and (best is None or v > best[1]):
                    best = (s["name"], v)
        return f"{best[0]}+0x{off - best[1]:x}" if best else f"{sname}+0x{off:x}"

    ro_list, rw_list, errors = [], [], []
    if elf.machine == 243:
        errors.extend(collect_riscv(elf, img, code_end, rw_vma, rw_size, ro_list, rw_list))
    for rs in elf.sh:
        if elf.machine == 243:
            break
        if rs["type"] == 4:
            raise LinkError(f"{rs['name']}: RELA relocations are not expected on ARM")
        if rs["type"] != 9:     # SHT_REL
            continue
        tgt = elf.sh[rs["info"]]
        tname = tgt["name"]
        if tname.startswith(".pex.") or not (tgt["flags"] & 2):
            continue            # metadata and debug info: values are final already
        symtab = elf.sh[rs["link"]]
        place_seg = "code" if tname in (".text", ".rodata") else "rw" if tname == ".data" else None
        if place_seg is None:
            errors.append(f"relocations in unexpected section {tname}")
            continue
        for k in range(rs["size"] // 8):
            r_off, r_info = struct.unpack_from("<II", elf.d, rs["off"] + k * 8)
            typ, si = r_info & 0xFF, r_info >> 8
            # symbol (index into the symtab this REL section links to)
            nm, val, size, info, other, shndx = struct.unpack_from("<IIIBBH", elf.d, symtab["off"] + si * 16)
            sname = elf.cstr(elf.sh[symtab["link"]]["off"] + nm) or (
                elf.sh[shndx]["name"] if 0 < shndx < 0xFF00 else "?")
            bind = info >> 4
            sseg = seg_of_shndx(shndx)
            at = where(tname, r_off)
            if typ in IGNORE:
                continue
            if sseg == "undef":
                if bind == 2 and typ in ABS:
                    continue        # &weak_undefined == 0, no fix-up needed
                errors.append(f"{at}: {RNAME.get(typ, typ)} to undefined symbol '{sname}' -- not in the "
                              f"host API ({os.path.basename(a.api)}) and not defined by the module")
                continue
            if typ in WHY:
                errors.append(f"{at}: R_ARM_{RNAME[typ]} against '{sname}': {WHY[typ]}")
                continue
            if typ in PCREL:
                if sseg != place_seg:
                    errors.append(f"{at}: PC-relative R_ARM_{RNAME[typ]} from the {place_seg} segment to "
                                  f"'{sname}' in the {sseg} segment: the two are placed independently"
                                  + (" (data reached PC-relatively: is this file built with -fropi but "
                                     "without -frwpi?)" if sseg == "rw" else ""))
                continue
            if typ in SBREL:
                if model != "pic":
                    errors.append(f"{at}: static-base relative R_ARM_{RNAME[typ]} in a model A module; "
                                  "-frwpi needs MODEL pic")
                elif sseg != "rw":
                    errors.append(f"{at}: R_ARM_{RNAME[typ]} to '{sname}' which is not in RW")
                continue
            if typ in ABS:
                if sseg == "abs":
                    errors.append(f"{at}: absolute word refers to the absolute symbol '{sname}'")
                    continue
                if sseg not in ("code", "rw"):
                    errors.append(f"{at}: absolute word refers to '{sname}' in {sseg}")
                    continue
                if place_seg == "code":
                    w_off = r_off
                else:
                    w_off = code_end + (r_off - rw_vma)
                v = struct.unpack_from("<I", img, w_off)[0]
                if sseg == "rw":
                    nv = v - rw_vma
                    if not (0 <= nv <= rw_size):
                        errors.append(f"{at}: word 0x{v:08x} to '{sname}' lies outside RW")
                        continue
                    struct.pack_into("<I", img, w_off, nv)
                else:
                    if not (0 <= (v & ~1) <= code_end):
                        errors.append(f"{at}: word 0x{v:08x} to '{sname}' lies outside the code")
                        continue
                bit = TO_RW if sseg == "rw" else 0
                if place_seg == "code":
                    if model == "pic":
                        hint = ("make the table non-const (it then lives in RW and is fixed up at "
                                "load) or store offsets instead of pointers"
                                if tname == ".rodata" else "is this file built without -fropi?")
                        errors.append(f"{at}: absolute address of '{sname}' in read-only "
                                      f"{tname[1:]}: ROPI code must not contain one; {hint}")
                        continue
                    ro_list.append(r_off | bit)
                else:
                    rw_list.append((r_off - rw_vma) | bit)
                continue
            errors.append(f"{at}: unsupported relocation R_ARM_{RNAME.get(typ, str(typ))} against "
                          f"'{sname}'")
    if errors:
        raise LinkError("refusing to build the .pex:\n  " + "\n  ".join(errors))

    # ---- imports: the GOT slots the linker kept
    weak = set()
    if sec(".pex.weak"):
        for nm in elf.data(S[".pex.weak"]).split(b"\0"):
            if nm:
                weak.add(nm.decode())
    imports, groups = [], {}
    for s in elf.syms:
        if s["name"].startswith("__pex_got_") and s["shndx"] and s["shndx"] < 0xFF00:
            name = s["name"][len("__pex_got_"):]
            if name not in api_syms:
                raise LinkError(f"GOT slot for '{name}' which {a.api} does not define (stale stubs?)")
            g, f = api_syms[name]
            off = s["value"] - rw_vma
            if not (0 <= off < data_size):
                raise LinkError(f"GOT slot {name} not inside .data")
            flags = WEAK if name in weak else 0
            imports.append((fnv1a(name), g["id"], flags, off, name))
            gv = groups.setdefault(g["id"], [g["major"], 0, g["name"], True])
            gv[1] = max(gv[1], f.get("since", 0))
            gv[3] = gv[3] and bool(flags & WEAK)
    for w in weak:
        if w not in api_syms:
            raise LinkError(f"PEX_WEAK_IMPORT({w}): not a host API symbol")
    imports.sort(key=lambda t: t[3])

    # ---- exports
    exports = []
    if sec(".pex.exports"):
        b = elf.data(S[".pex.exports"])
        i = 0
        while i + 4 <= len(b):
            val = struct.unpack_from("<I", b, i)[0]
            e = b.index(b"\0", i + 4)
            name = b[i + 4:e].decode()
            i = al(e + 1, 4)
            if seg_of_addr(val & ~1, 1) != "code":
                raise LinkError(f"PEX_EXPORT({name}): not code")
            exports.append((fnv1a(name), val, 1, name))
    names = [e[3] for e in exports]
    if len(set(names)) != len(names):
        raise LinkError("duplicate PEX_EXPORT")
    if len({e[0] for e in exports}) != len(exports):
        raise LinkError("FNV-1a collision between exports: rename one")

    def fnlist(name):
        if not sec(name):
            return []
        b = elf.data(S[name])
        out = []
        for i in range(0, len(b), 4):
            v = struct.unpack_from("<I", b, i)[0]
            if v in (0, 0xFFFFFFFF):
                continue
            if a.target == "c5":
                if v & 3 or not (0 < v < code_end):
                    raise LinkError(f"{name}: entry 0x{v:08x} is not a 4-byte-aligned function in the code")
            elif seg_of_addr(v & ~1, 1) != "code" or not (v & 1):
                raise LinkError(f"{name}: entry 0x{v:08x} is not a Thumb function in the code")
            out.append(v)
        return out
    inits, finis = fnlist(".pex.init"), fnlist(".pex.fini")

    main = elf.sym("pex_main")
    entry = 0
    if main and main["shndx"] and main["shndx"] < 0xFF00:
        entry = main["value"]
        if a.target == "c5":
            if entry & 3 or entry == 0:
                raise LinkError("c5 pex_main must be 4-byte aligned and not at address 0 "
                                "(the c5 linker script puts a nop first)")
        elif not entry & 1:
            raise LinkError("pex_main is not a Thumb function")

    # ---- metadata
    meta = {"name": a.name or os.path.splitext(os.path.basename(a.elf))[0], "version": a.version,
            "tool": TOOL}
    if a.desc:
        meta["description"] = a.desc
    if sec(".pex.meta"):
        parts = elf.data(S[".pex.meta"]).split(b"\0")
        for k, v in zip(parts[0::2], parts[1::2]):
            if k:
                key = k.decode()
                if key not in META_TAGS:
                    raise LinkError(f"PEX_META_STR: unknown key '{key}' (known: {', '.join(META_TAGS)})")
                meta[key] = v.decode()
    if a.kind == "lua":
        meta.setdefault("lua_name", meta["name"])

    flags = 0
    if model == "pic":
        flags |= F["pic"]
    if a.kind == "app":
        flags |= F["app"]
    if a.kind == "lua" or a.lua:
        flags |= F["lua_module"]
    for fl in filter(None, (a.flags or "").split(",")):
        k = fl.strip().replace("-", "_")
        if k not in F or k == "pic":
            raise LinkError(f"unknown flag '{fl}'")
        flags |= F[k]
    if flags & F["reentrant"] and not flags & F["pic"]:
        raise LinkError("REENTRANT needs model B (pic): model A code is patched per instance")
    if model == "pic":
        flags |= F["xip_ok"]

    stack = a.stack_hint or 0
    for su in a.su or []:
        for line in open(su):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 2 and p[1].strip().isdigit():
                stack = max(stack, int(p[1]))

    vm = [int(x) for x in a.version.split(".")] + [0, 0]
    module_version = (vm[0] << 24) | (vm[1] << 16) | vm[2]

    # ---- sections
    secs = []

    reloc = struct.pack("<II", len(ro_list), len(rw_list)) + b"".join(
        struct.pack("<I", x) for x in sorted(ro_list)) + b"".join(struct.pack("<I", x) for x in sorted(rw_list))
    secs.append((SEC["RELOC"], 0, reloc))

    gl = sorted(groups.items())
    imp = struct.pack("<HH", len(gl), len(imports))
    for gid, (maj, mnr, _, allweak) in gl:
        imp += struct.pack("<HBB", gid, maj, mnr)
    for h, gid, fl, off, _ in imports:
        imp += struct.pack("<IHHI", h, gid, fl, off)
    secs.append((SEC["IMPORTS"], 0, imp))

    if exports:
        strtab = b""
        ent = b""
        for h, val, kind, name in exports:
            ent += struct.pack("<IIHHI", h, val, kind, len(name), len(strtab))
            strtab += name.encode() + b"\0"
        secs.append((SEC["EXPORTS"], 0, struct.pack("<I", len(exports)) + ent + strtab))
    if inits:
        secs.append((SEC["INIT"], 0, struct.pack("<I", len(inits)) + b"".join(struct.pack("<I", v) for v in inits)))
    if finis:
        secs.append((SEC["FINI"], 0, struct.pack("<I", len(finis)) + b"".join(struct.pack("<I", v) for v in finis)))

    m = b""
    for k, v in meta.items():
        vb = v.encode() if isinstance(v, str) else struct.pack("<I", v)
        m += struct.pack("<HH", META_TAGS[k], len(vb)) + vb
        m += b"\0" * (al(len(m), 4) - len(m))
    secs.append((SEC["META"], 0, m))

    if not a.no_debug:
        dbg = b""
        def rec(kind, val, name):
            nb = name.encode()[:255]
            r = struct.pack("<IHH", val, kind, len(nb)) + nb
            return r + b"\0" * (al(len(r), 4) - len(r))
        for h, gid, fl, off, name in imports:
            dbg += rec(1, h, name)
        for s in sorted(elf.syms, key=lambda s: s["value"]):
            if s["type"] == 2 and s["shndx"] and s["shndx"] < 0xFF00 and s["name"] and \
                    seg_of_shndx(s["shndx"]) == "code":
                dbg += rec(2, s["value"], s["name"])
        secs.append((SEC["DEBUG"], OPTIONAL, dbg))

    signed = a.sign
    nsec = len(secs) + (1 if signed else 0)
    off = HEADER_SIZE + nsec * 12
    table, body = b"", b""
    for typ, fl, payload in secs:
        table += struct.pack("<HHII", typ, fl, off + len(body), len(payload))
        body += payload + b"\0" * (al(len(payload), 4) - len(payload))
    image_off = al(off + len(body), 8)
    body += b"\0" * (image_off - off - len(body))
    sig_off = image_off + len(img)
    sig_off = al(sig_off, 4)
    file_size = sig_off + (68 if signed else 0)
    if signed:
        table += struct.pack("<HHII", SEC["SIG"], 0, sig_off, 68)

    build_id = hashlib.sha256(bytes(img)).digest()[:16]
    feats = FEATURES[a.target] if a.features is None else a.features
    hdr = MAGIC + struct.pack("<HHHHI", FORMAT_MAJOR, FORMAT_MINOR, HEADER_SIZE, TARGETS[a.target], file_size)
    hdr += struct.pack("<IHHII", feats, api["api"]["major"], api["api"]["minor"], flags, module_version)
    hdr += struct.pack("<IIIIIII", len(img), text_size, ro_size, data_size, bss_size, stack, entry)
    hdr += struct.pack("<IIII", nsec, HEADER_SIZE, image_off, 0) + build_id
    assert len(hdr) == HEADER_SIZE
    out = bytearray(hdr + table + body + bytes(img))
    out += b"\0" * (sig_off - len(out))
    struct.pack_into("<I", out, CRC_OFF, crc32_file(out))
    if signed:
        out += sign_bytes(bytes(out), a.key, a.key_id)
    assert len(out) == file_size
    info = dict(text=text_size, ro=ro_size, data=data_size, bss=bss_size, ro_relocs=len(ro_list),
                rw_relocs=len(rw_list), imports=len(imports), exports=len(exports), file=len(out))
    return bytes(out), info


def ota_sign_module(key):
    if key:
        os.environ["BTGW_OTA_KEY"] = os.path.abspath(os.path.expanduser(key))
    sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
    import ota_sign       # noqa: E402  -- the gateway's OTA key handling, reused (spec 10)
    return ota_sign


def sign_bytes(pre, key, key_id):
    """The SIG section for the bytes before it: alg, key id, raw r||s."""
    ota_sign = ota_sign_module(key)
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(pre)
        tn = t.name
    try:
        digest, sig = ota_sign.sign(tn)
    finally:
        os.unlink(tn)
    assert digest == hashlib.sha256(pre).digest()
    return struct.pack("<HH", 1, key_id) + sig


# ---------------------------------------------------------------- reading
def parse(buf):
    if len(buf) < HEADER_SIZE or buf[:4] != MAGIC:
        raise LinkError("not a .pex (bad magic)")
    h = {}
    h["format_major"], h["format_minor"], h["header_size"], h["target"], h["file_size"] = \
        struct.unpack_from("<HHHHI", buf, 4)
    (h["features"], h["api_major"], h["api_minor"], h["flags"], h["module_version"], h["image_size"],
     h["text_size"], h["ro_size"], h["data_size"], h["bss_size"], h["stack_hint"], h["entry"],
     h["section_count"], h["section_table_off"], h["image_off"], h["crc32"]) = \
        struct.unpack_from("<IHHIIIIIIIIIIIII", buf, 16)
    h["build_id"] = buf[76:92]
    secs = []
    for i in range(h["section_count"]):
        t, fl, off, size = struct.unpack_from("<HHII", buf, h["section_table_off"] + 12 * i)
        secs.append(dict(type=t, flags=fl, off=off, size=size, data=buf[off:off + size]))
    return h, secs


def sig_start(h, secs):
    s = [x for x in secs if x["type"] == SEC["SIG"]]
    return s[0]["off"] if s else h["file_size"]


def cmd_sign(a):
    buf = open(a.input, "rb").read()
    h, secs = parse(buf)
    if any(s["type"] == SEC["SIG"] for s in secs):
        raise LinkError("already signed")
    # Insert a SIG entry: the section table grows by 12 bytes, so rebuild offsets.
    n = h["section_count"]
    shift = 12
    out = bytearray(buf[:HEADER_SIZE])
    table = b""
    for s in secs:
        table += struct.pack("<HHII", s["type"], s["flags"], s["off"] + shift, s["size"])
    image_off = h["image_off"] + shift
    pad = al(image_off, 8) - image_off
    image_off += pad
    body = buf[HEADER_SIZE + 12 * n:h["image_off"]]
    img = buf[h["image_off"]:h["image_off"] + h["image_size"]]
    # sections keep their relative layout; any extra pad goes before the image
    sig_off = al(image_off + len(img), 4)
    table += struct.pack("<HHII", SEC["SIG"], 0, sig_off, 68)
    out += table + body + b"\0" * pad + img
    out += b"\0" * (sig_off - len(out))
    struct.pack_into("<I", out, 12, sig_off + 68)
    struct.pack_into("<I", out, 60, n + 1)
    struct.pack_into("<I", out, 68, image_off)
    struct.pack_into("<I", out, CRC_OFF, crc32_file(out))
    out += sign_bytes(bytes(out), a.key, a.key_id)
    open(a.output, "wb").write(out)
    print(f"{a.output}: signed ({len(out)} bytes)")


def verify_sig(pre, sig, pem):
    """openssl verify of raw r||s against the public half of pem."""
    r, s = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")

    def der_int(x):
        b = x.to_bytes(33, "big").lstrip(b"\0") or b"\0"
        if b[0] & 0x80:
            b = b"\0" + b
        return b"\x02" + bytes([len(b)]) + b
    body = der_int(r) + der_int(s)
    der = b"\x30" + bytes([len(body)]) + body
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "m"), "wb").write(hashlib.sha256(pre).digest())
        open(os.path.join(d, "s"), "wb").write(der)
        p = subprocess.run(["openssl", "pkeyutl", "-verify", "-inkey", pem, "-in", os.path.join(d, "m"),
                            "-sigfile", os.path.join(d, "s"), "-pkeyopt", "digest:sha256"],
                           capture_output=True, text=True)
        return p.returncode == 0


def dump(a):
    buf = open(a.input, "rb").read()
    h, secs = parse(buf)
    names = {}
    if a.api and os.path.exists(a.api):
        _, by = load_api(a.api)
        names = {fnv1a(n): n for n in by}
    t = h["target"]
    fl = [k for k, v in F.items() if h["flags"] & v]
    feats = [FEAT_NAMES[i] for i in range(4) if h["features"] & (1 << i)]
    mv = h["module_version"]
    print(f"{a.input}: {len(buf)} bytes")
    print(f"  format        {h['format_major']}.{h['format_minor']}  header {h['header_size']} bytes")
    print(f"  target        0x{t:04x} ({TARGET_NAMES.get(t, '?')})  features {','.join(feats) or '-'}")
    print(f"  api           {h['api_major']}.{h['api_minor']}")
    print(f"  flags         0x{h['flags']:x} ({','.join(fl) or '-'})  model {'B (pic)' if h['flags'] & 1 else 'A (reloc)'}")
    print(f"  version       {mv >> 24}.{(mv >> 16) & 255}.{mv & 0xffff}")
    print(f"  image         {h['image_size']} bytes: text {h['text_size']} ro {h['ro_size']} "
          f"data {h['data_size']} (+bss {h['bss_size']})  at file offset {h['image_off']}")
    print(f"  RAM           code {h['text_size'] + h['ro_size']}  RW {h['data_size'] + h['bss_size']}  "
          f"stack hint {h['stack_hint']}")
    print(f"  entry         {'0x%x' % h['entry'] if h['entry'] else '(none)'}")
    print(f"  build_id      {h['build_id'].hex()}")
    so = sig_start(h, secs)
    crc = crc32_file(buf[:so])
    print(f"  crc32         0x{h['crc32']:08x} ({'ok' if crc == h['crc32'] else 'BAD, computed 0x%08x' % crc})")
    if h["file_size"] != len(buf):
        print(f"  file_size     {h['file_size']} -- DOES NOT MATCH the file")
    dbg_imp, dbg_sym = {}, []
    for s in secs:
        if s["type"] == SEC["DEBUG"]:
            b, i = s["data"], 0
            while i + 8 <= len(b):
                val, kind, ln = struct.unpack_from("<IHH", b, i)
                nm = b[i + 8:i + 8 + ln].decode()
                i = al(i + 8 + ln, 4)
                if kind == 1:
                    dbg_imp[val] = nm
                else:
                    dbg_sym.append((val, nm))
    names.update(dbg_imp)
    print("  sections:")
    for s in secs:
        print(f"    {SEC_NAMES.get(s['type'], hex(s['type'])):8s} off {s['off']:6d} size {s['size']:6d}"
              f"{' optional' if s['flags'] & OPTIONAL else ''}")
        b = s["data"]
        if s["type"] == SEC["RELOC"]:
            nro, nrw = struct.unpack_from("<II", b, 0)
            print(f"      RO {nro} words, RW {nrw} words")
            if a.relocs:
                for k in range(nro + nrw):
                    v = struct.unpack_from("<I", b, 8 + 4 * k)[0]
                    print(f"        {'RO' if k < nro else 'RW'} +0x{v & 0x7fffffff:05x} -> "
                          f"{'RW' if v & TO_RW else 'code'}")
        elif s["type"] == SEC["IMPORTS"]:
            ng, ni = struct.unpack_from("<HH", b, 0)
            gs = [struct.unpack_from("<HBB", b, 4 + 4 * k) for k in range(ng)]
            print("      groups " + ", ".join(f"{g}:{mj}.{mn}" for g, mj, mn in gs))
            for k in range(ni):
                hh, gid, f2, off = struct.unpack_from("<IHHI", b, 4 + 4 * ng + 12 * k)
                print(f"      0x{hh:08x} g{gid:<2d} RW+0x{off:04x} {'weak ' if f2 & WEAK else ''}"
                      f"{names.get(hh, '?')}")
        elif s["type"] == SEC["EXPORTS"]:
            n = struct.unpack_from("<I", b, 0)[0]
            st = 4 + 16 * n
            for k in range(n):
                hh, val, kind, ln, no = struct.unpack_from("<IIHHI", b, 4 + 16 * k)
                print(f"      0x{hh:08x} @0x{val:05x} {b[st + no:st + no + ln].decode()}")
        elif s["type"] in (SEC["INIT"], SEC["FINI"]):
            n = struct.unpack_from("<I", b, 0)[0]
            print("      " + " ".join("0x%x" % struct.unpack_from("<I", b, 4 + 4 * k)[0] for k in range(n)))
        elif s["type"] == SEC["META"]:
            i = 0
            while i + 4 <= len(b):
                tag, ln = struct.unpack_from("<HH", b, i)
                v = b[i + 4:i + 4 + ln]
                i = al(i + 4 + ln, 4)
                print(f"      {META_NAMES.get(tag, tag)}: {v.decode(errors='replace')}")
        elif s["type"] == SEC["DEBUG"]:
            print(f"      {len(dbg_imp)} import names, {len(dbg_sym)} symbols")
        elif s["type"] == SEC["SIG"]:
            alg, kid = struct.unpack_from("<HH", b, 0)
            st = ""
            if a.verify_key:
                st = " -- signature " + ("VALID" if verify_sig(buf[:s["off"]], b[4:68], a.verify_key) else "INVALID")
            print(f"      alg {alg} (ECDSA P-256/SHA-256) key {kid}{st}")
    return 0


def main():
    argv = sys.argv[1:]
    if argv[:1] == ["--dump"]:
        argv[0] = "dump"
    ap = argparse.ArgumentParser(description="ELF -> .pex", formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    sp = ap.add_subparsers(dest="cmd", required=True)
    l = sp.add_parser("link")
    l.add_argument("elf")
    l.add_argument("-o", "--output", required=True)
    l.add_argument("--target", choices=["m33", "m0p", "c5"], required=True)
    l.add_argument("--model", choices=["reloc", "pic"], required=True)
    l.add_argument("--api", default=os.path.join(HERE, "..", "host_api.json"))
    l.add_argument("--name")
    l.add_argument("--version", default="0.0.0")
    l.add_argument("--kind", choices=["lib", "app", "lua"], default="lib")
    l.add_argument("--lua", action="store_true", help="also a Lua module (LUA_MODULE flag)")
    l.add_argument("--desc")
    l.add_argument("--flags")
    l.add_argument("--features", type=lambda x: int(x, 0))
    l.add_argument("--su", action="append")
    l.add_argument("--stack-hint", type=int)
    l.add_argument("--sign", action="store_true")
    l.add_argument("--key")
    l.add_argument("--key-id", type=int, default=1)
    l.add_argument("--no-debug", action="store_true")
    s = sp.add_parser("sign")
    s.add_argument("input")
    s.add_argument("-o", "--output", required=True)
    s.add_argument("--key")
    s.add_argument("--key-id", type=int, default=1)
    d = sp.add_parser("dump")
    d.add_argument("input")
    d.add_argument("--api", default=os.path.join(HERE, "..", "host_api.json"))
    d.add_argument("--verify-key")
    d.add_argument("--relocs", action="store_true")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "link":
            out, info = build(a)
            open(a.output, "wb").write(out)
            print(f"pexlink: {a.output}: {info['file']} bytes (text {info['text']} ro {info['ro']} "
                  f"data {info['data']} bss {info['bss']}; relocs RO {info['ro_relocs']} RW {info['rw_relocs']}; "
                  f"{info['imports']} imports, {info['exports']} exports)")
        elif a.cmd == "sign":
            cmd_sign(a)
        else:
            return dump(a)
    except LinkError as e:
        print(f"pexlink: error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
