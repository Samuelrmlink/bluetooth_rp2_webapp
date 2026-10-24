// pexlink -- ELF (linked with a PEX linker script) -> .pex
//
// A JavaScript port of pex/tools/pexlink.py `link` (tool string "pexlink 0.6").
// The browser must not shell out to host Python, and must not hold the OTA
// signing key: this module refuses --sign. Host signing stays
// `python3 pex/tools/pexlink.py sign`.
//
// The input is the same ELF pexlink.py accepts (ARM ELF32, --emit-relocs,
// the PEX linker script). On a deterministic link the bytes match.

export const TOOL = 'pexlink 0.6';
const MAGIC = [0x50, 0x45, 0x58, 0x1a];
const HEADER_SIZE = 92;
const CRC_OFF = 72;

const TARGETS = { m0p: 0x0001, m33: 0x0002, m23: 0x0003, m4f: 0x0004 };
const FEATURES = { m33: 0b101, m0p: 0 };
const F = {
  pic: 1 << 0, xip_ok: 1 << 1, ram_only: 1 << 2, lua_module: 1 << 3, app: 1 << 4,
  reentrant: 1 << 5, needs_sig: 1 << 6, unload_on_exit: 1 << 7,
};
const SEC = { RELOC: 1, IMPORTS: 2, EXPORTS: 3, META: 4, DEPS: 5, DEBUG: 6, INIT: 7, FINI: 8, SIG: 0xF0 };
const OPTIONAL = 1;
const TO_RW = 0x80000000;
const WEAK = 1;
const META_TAGS = {
  name: 1, description: 2, author: 3, url: 4, license: 5, lua_name: 6, min_ram: 7, version: 8, tool: 9,
};
const R = {
  NONE: 0, ABS32: 2, REL32: 3, THM_ABS5: 7, ABS8: 8, SBREL32: 9, THM_CALL: 10, THM_PC8: 11,
  TARGET1: 38, V4BX: 40, TARGET2: 41, PREL31: 42, MOVW_ABS_NC: 43, MOVT_ABS: 44,
  THM_MOVW_ABS_NC: 47, THM_MOVT_ABS: 48, THM_MOVW_PREL_NC: 49, THM_MOVT_PREL: 50,
  THM_JUMP19: 51, THM_JUMP6: 52, THM_ALU_PREL_11_0: 53, THM_PC12: 54, THM_JUMP24: 30,
  GOTOFF32: 24, BASE_PREL: 25, GOT_BREL: 26, THM_MOVW_BREL_NC: 87, THM_MOVT_BREL: 88,
  THM_MOVW_BREL: 89, GOT_PREL: 96, THM_JUMP11: 102, THM_JUMP8: 103,
  THM_ALU_ABS_G0_NC: 132, THM_ALU_ABS_G1_NC: 133, THM_ALU_ABS_G2_NC: 134, THM_ALU_ABS_G3: 135,
};
const RNAME = Object.fromEntries(Object.entries(R).map(([k, v]) => [v, k]));
const PCREL = new Set([R.REL32, R.THM_CALL, R.THM_PC8, R.THM_MOVW_PREL_NC, R.THM_MOVT_PREL,
  R.THM_JUMP19, R.THM_JUMP6, R.THM_ALU_PREL_11_0, R.THM_PC12, R.THM_JUMP24, R.THM_JUMP11, R.THM_JUMP8]);
const SBREL = new Set([R.SBREL32, R.THM_MOVW_BREL_NC, R.THM_MOVT_BREL, R.THM_MOVW_BREL]);
const ABS = new Set([R.ABS32, R.TARGET1]);
const IGNORE = new Set([R.NONE, R.V4BX]);
const WHY = {
  [R.THM_MOVW_ABS_NC]: 'an absolute MOVW/MOVT address pair; drop -mslow-flash-data/-mpure-code (literal-pool words are what model A relocates), or build MODEL pic',
  [R.THM_MOVT_ABS]: 'an absolute MOVW/MOVT address pair; drop -mslow-flash-data/-mpure-code, or build MODEL pic',
  [R.MOVW_ABS_NC]: 'ARM-state code; PEX is Thumb only (-mthumb)',
  [R.MOVT_ABS]: 'ARM-state code; PEX is Thumb only (-mthumb)',
  [R.GOT_BREL]: 'a GOT reference; do not build with -fPIC/-fpic (model B is -fropi -frwpi, MODEL pic)',
  [R.GOT_PREL]: 'a GOT reference; do not build with -fPIC/-fpic (model B is -fropi -frwpi, MODEL pic)',
  [R.GOTOFF32]: 'a GOT-relative reference; do not build with -fPIC/-fpic',
  [R.BASE_PREL]: 'a GOT base reference; do not build with -fPIC/-fpic',
  [R.TARGET2]: 'C++ exception tables; build with -fno-exceptions -fno-unwind-tables',
  [R.PREL31]: 'unwind tables; build with -fno-unwind-tables -fno-asynchronous-unwind-tables',
  [R.THM_ALU_ABS_G0_NC]: 'Armv6-M execute-only absolute addressing; drop -mpure-code',
  [R.THM_ALU_ABS_G1_NC]: 'Armv6-M execute-only absolute addressing; drop -mpure-code',
  [R.THM_ALU_ABS_G2_NC]: 'Armv6-M execute-only absolute addressing; drop -mpure-code',
  [R.THM_ALU_ABS_G3]: 'Armv6-M execute-only absolute addressing; drop -mpure-code',
};

export class LinkError extends Error {
  constructor(message) { super(message); this.name = 'LinkError'; }
}

function fnv1a(s) {
  let h = 0x811C9DC5;
  const b = new TextEncoder().encode(s);
  for (let i = 0; i < b.length; i++) {
    h ^= b[i];
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h >>> 0;
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    t[i] = c >>> 0;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

function crc32File(buf) {
  const b = new Uint8Array(buf);
  b[CRC_OFF] = b[CRC_OFF + 1] = b[CRC_OFF + 2] = b[CRC_OFF + 3] = 0;
  return crc32(b);
}

const al = (x, a) => (x + a - 1) & ~(a - 1);
const u32 = (n) => n >>> 0;
const cmpNum = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

function cstr(d, off) {
  let e = off;
  while (e < d.length && d[e] !== 0) e++;
  return new TextDecoder('utf-8', { fatal: false }).decode(d.subarray(off, e));
}

class Elf {
  constructor(bytes) {
    const d = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    this.d = d;
    if (d.length < 52 || d[0] !== 0x7f || d[1] !== 0x45 || d[2] !== 0x4c || d[3] !== 0x46 || d[4] !== 1 || d[5] !== 1) {
      throw new LinkError('not a little-endian ELF32 file');
    }
    const v = new DataView(d.buffer, d.byteOffset, d.byteLength);
    this.machine = v.getUint16(18, true);
    if (this.machine !== 40) throw new LinkError('not an ARM ELF');
    const shoff = v.getUint32(32, true);
    const shentsize = v.getUint16(46, true);
    const shnum = v.getUint16(48, true);
    const shstrndx = v.getUint16(50, true);
    this.sh = [];
    for (let i = 0; i < shnum; i++) {
      const o = shoff + i * shentsize;
      this.sh.push({
        i,
        name_off: v.getUint32(o, true),
        type: v.getUint32(o + 4, true),
        flags: v.getUint32(o + 8, true),
        addr: v.getUint32(o + 12, true),
        off: v.getUint32(o + 16, true),
        size: v.getUint32(o + 20, true),
        link: v.getUint32(o + 24, true),
        info: v.getUint32(o + 28, true),
        align: v.getUint32(o + 32, true),
        entsize: v.getUint32(o + 36, true),
      });
    }
    const strtab = this.sh[shstrndx];
    for (const s of this.sh) s.name = cstr(d, strtab.off + s.name_off);
    this.by_name = Object.create(null);
    for (const s of this.sh) this.by_name[s.name] = s;
    this.syms = [];
    for (const s of this.sh) {
      if (s.type !== 2) continue;
      const st = this.sh[s.link];
      const n = (s.size / 16) | 0;
      for (let k = 0; k < n; k++) {
        const o = s.off + k * 16;
        const info = d[o + 12];
        this.syms.push({
          name: cstr(d, st.off + v.getUint32(o, true)),
          value: v.getUint32(o + 4, true),
          size: v.getUint32(o + 8, true),
          bind: info >> 4,
          type: info & 15,
          other: d[o + 13],
          shndx: v.getUint16(o + 14, true),
        });
      }
    }
    this.v = v;
  }

  data(s) {
    if (s.type === 8) return new Uint8Array(s.size);
    return this.d.subarray(s.off, s.off + s.size);
  }

  sym(name) {
    for (const s of this.syms) if (s.name === name) return s;
    return null;
  }
}

function loadApi(api) {
  const byName = Object.create(null);
  for (const g of api.groups) {
    for (const f of g.functions || []) byName[f.name] = [g, f];
  }
  return byName;
}

function u32le(buf, off) {
  return (buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24)) >>> 0;
}

function putU32(buf, off, val) {
  const x = val >>> 0;
  buf[off] = x & 255;
  buf[off + 1] = (x >> 8) & 255;
  buf[off + 2] = (x >> 16) & 255;
  buf[off + 3] = (x >> 24) & 255;
}

function concat(parts) {
  let n = 0;
  for (const p of parts) n += p.length;
  const o = new Uint8Array(n);
  let i = 0;
  for (const p of parts) { o.set(p, i); i += p.length; }
  return o;
}

function pack(fmt, args) {
  // fmt is a string of <I <H <B only, little-endian, no alignment gaps.
  const bytes = [];
  let ai = 0;
  for (const ch of fmt) {
    const v = args[ai++] >>> 0;
    if (ch === 'B') bytes.push(v & 255);
    else if (ch === 'H') bytes.push(v & 255, (v >> 8) & 255);
    else if (ch === 'I') bytes.push(v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255);
    else throw new LinkError('bad pack ' + ch);
  }
  return Uint8Array.from(bytes);
}

function pad4(b) {
  const n = al(b.length, 4) - b.length;
  if (!n) return b;
  const o = new Uint8Array(b.length + n);
  o.set(b, 0);
  return o;
}

/**
 * Link an ARM ELF into an unsigned .pex.
 * opts: { target:'m33', model:'reloc'|'pic', api: object, name, version, kind,
 *         desc, flags, features, noDebug, stackHint, lua, elfName }
 * Returns { bytes: Uint8Array, info }.
 */
export async function linkElf(elfBytes, opts = {}) {
  if (opts.sign) throw new LinkError('the browser linker does not sign; run python3 pex/tools/pexlink.py sign on the host');
  const elf = new Elf(elfBytes);
  const model = opts.model;
  const target = opts.target;
  if (target !== 'm33' && target !== 'm0p') throw new LinkError('target must be m33 or m0p');
  if (model !== 'reloc' && model !== 'pic') throw new LinkError('model must be reloc or pic');
  const api = opts.api;
  if (!api || !api.groups) throw new LinkError('host API JSON is required');
  const apiSyms = loadApi(api);
  const apiLabel = opts.apiName || 'host_api.json';
  const S = elf.by_name;
  const known = new Set(['.text', '.rodata', '.data', '.bss', '.pex.exports', '.pex.weak', '.pex.meta', '.pex.init', '.pex.fini']);
  for (const s of elf.sh) {
    if ((s.flags & 2) && s.size && !known.has(s.name)) {
      throw new LinkError(`unexpected allocated section ${s.name} (${s.size} bytes): the PEX linker script only places .text/.rodata/.data/.bss -- link with pex/sdk/ld/pex_${target}_${model}.ld`);
    }
  }
  const sec = (name) => (S[name] && S[name].size ? S[name] : null);
  const text = sec('.text');
  if (!text || text.addr !== 0) throw new LinkError('.text must exist and start at address 0');
  const ro = sec('.rodata');
  const data = sec('.data');
  const rwSym = elf.sym('__pex_rw_start');
  if (!rwSym) throw new LinkError('no __pex_rw_start: not linked with a PEX linker script');
  const rwVma = rwSym.value >>> 0;
  const textSize = ro ? ro.addr : al(text.size, 4);
  let codeEnd = ro ? (ro.addr + ro.size) : textSize;
  codeEnd = al(codeEnd, 4);
  const roSize = codeEnd - textSize;
  const dataSize = data ? data.size : 0;
  if (data && data.addr !== rwVma) throw new LinkError('.data does not start at __pex_rw_start');
  const endSym = elf.sym('__pex_rw_end');
  const rwEnd = endSym ? endSym.value >>> 0 : rwVma + dataSize;
  const bssSize = rwEnd - rwVma - dataSize;
  if (model === 'reloc' && rwVma < codeEnd) throw new LinkError('model A: RW overlaps the code');
  const rwSize = dataSize + bssSize;

  const img = new Uint8Array(codeEnd + dataSize);
  img.set(elf.data(text).subarray(0, text.size), 0);
  if (ro) img.set(elf.data(ro).subarray(0, ro.size), ro.addr);
  if (data) img.set(elf.data(data).subarray(0, dataSize), codeEnd);

  const segOfShndx = (ndx) => {
    if (ndx === 0) return 'undef';
    if (ndx >= 0xFF00) return 'abs';
    const n = elf.sh[ndx].name;
    if (n === '.text' || n === '.rodata') return 'code';
    if (n === '.data' || n === '.bss') return 'rw';
    if (n.startsWith('.pex.')) return 'meta';
    return 'other:' + n;
  };
  const segOfAddr = (addr, size = 4) => {
    if (addr >= 0 && addr + size <= codeEnd) return 'code';
    if (addr >= rwVma && addr + size <= rwVma + rwSize) return 'rw';
    return null;
  };
  const where = (sname, off) => {
    let best = null;
    for (const s of elf.syms) {
      if ((s.type === 1 || s.type === 2) && s.shndx && s.shndx < 0xFF00) {
        const v = s.type === 2 ? (s.value & ~1) : s.value;
        if (v <= off && off < v + Math.max(s.size, 1) && (best === null || v > best[1])) best = [s.name, v];
      }
    }
    return best ? `${best[0]}+0x${(off - best[1]).toString(16)}` : `${sname}+0x${off.toString(16)}`;
  };

  const roList = [];
  const rwList = [];
  const errors = [];
  for (const rs of elf.sh) {
    if (rs.type === 4) throw new LinkError(`${rs.name}: RELA relocations are not expected on ARM`);
    if (rs.type !== 9) continue;
    const tgt = elf.sh[rs.info];
    const tname = tgt.name;
    if (tname.startsWith('.pex.') || !(tgt.flags & 2)) continue;
    const symtab = elf.sh[rs.link];
    const place = tname === '.text' || tname === '.rodata' ? 'code' : tname === '.data' ? 'rw' : null;
    if (!place) { errors.push(`relocations in unexpected section ${tname}`); continue; }
    const nrel = (rs.size / 8) | 0;
    for (let k = 0; k < nrel; k++) {
      const o = rs.off + k * 8;
      const rOff = elf.v.getUint32(o, true);
      const rInfo = elf.v.getUint32(o + 4, true);
      const typ = rInfo & 0xFF;
      const si = rInfo >>> 8;
      const so = symtab.off + si * 16;
      const nm = elf.v.getUint32(so, true);
      const info = elf.d[so + 12];
      const shndx = elf.v.getUint16(so + 14, true);
      let sname = cstr(elf.d, elf.sh[symtab.link].off + nm);
      if (!sname) sname = (shndx > 0 && shndx < 0xFF00) ? elf.sh[shndx].name : '?';
      const bind = info >> 4;
      const sseg = segOfShndx(shndx);
      const at = where(tname, rOff);
      if (IGNORE.has(typ)) continue;
      if (sseg === 'undef') {
        if (bind === 2 && ABS.has(typ)) continue;
        errors.push(`${at}: ${RNAME[typ] ?? typ} to undefined symbol '${sname}' -- not in the host API (${apiLabel.split('/').pop()}) and not defined by the module`);
        continue;
      }
      if (WHY[typ]) {
        errors.push(`${at}: R_ARM_${RNAME[typ]} against '${sname}': ${WHY[typ]}`);
        continue;
      }
      if (PCREL.has(typ)) {
        if (sseg !== place) {
          errors.push(`${at}: PC-relative R_ARM_${RNAME[typ]} from the ${place} segment to '${sname}' in the ${sseg} segment: the two are placed independently` +
            (sseg === 'rw' ? ' (data reached PC-relatively: is this file built with -fropi but without -frwpi?)' : ''));
        }
        continue;
      }
      if (SBREL.has(typ)) {
        if (model !== 'pic') errors.push(`${at}: static-base relative R_ARM_${RNAME[typ]} in a model A module; -frwpi needs MODEL pic`);
        else if (sseg !== 'rw') errors.push(`${at}: R_ARM_${RNAME[typ]} to '${sname}' which is not in RW`);
        continue;
      }
      if (ABS.has(typ)) {
        if (sseg === 'abs') { errors.push(`${at}: absolute word refers to the absolute symbol '${sname}'`); continue; }
        if (sseg !== 'code' && sseg !== 'rw') { errors.push(`${at}: absolute word refers to '${sname}' in ${sseg}`); continue; }
        const wOff = place === 'code' ? rOff : codeEnd + (rOff - rwVma);
        const val = u32le(img, wOff);
        if (sseg === 'rw') {
          const nv = (val - rwVma) >>> 0;
          if (!(nv >= 0 && nv <= rwSize)) { errors.push(`${at}: word 0x${val.toString(16)} to '${sname}' lies outside RW`); continue; }
          putU32(img, wOff, nv);
        } else if (!((val & ~1) >= 0 && (val & ~1) <= codeEnd)) {
          errors.push(`${at}: word 0x${val.toString(16)} to '${sname}' lies outside the code`);
          continue;
        }
        const bit = sseg === 'rw' ? TO_RW : 0;
        if (place === 'code') {
          if (model === 'pic') {
            const hint = tname === '.rodata'
              ? 'make the table non-const (it then lives in RW and is fixed up at load) or store offsets instead of pointers'
              : 'is this file built without -fropi?';
            errors.push(`${at}: absolute address of '${sname}' in read-only ${tname.slice(1)}: ROPI code must not contain one; ${hint}`);
            continue;
          }
          roList.push((rOff | bit) >>> 0);
        } else {
          rwList.push(((rOff - rwVma) | bit) >>> 0);
        }
        continue;
      }
      errors.push(`${at}: unsupported relocation R_ARM_${RNAME[typ] ?? String(typ)} against '${sname}'`);
    }
  }
  if (errors.length) throw new LinkError('refusing to build the .pex:\n  ' + errors.join('\n  '));

  const weak = new Set();
  if (sec('.pex.weak')) {
    const raw = elf.data(S['.pex.weak']);
    let i = 0;
    while (i < raw.length) {
      let e = i;
      while (e < raw.length && raw[e] !== 0) e++;
      if (e > i) weak.add(new TextDecoder().decode(raw.subarray(i, e)));
      i = e + 1;
    }
  }
  const imports = [];
  const groups = new Map();
  for (const s of elf.syms) {
    if (!s.name.startsWith('__pex_got_') || !s.shndx || s.shndx >= 0xFF00) continue;
    const name = s.name.slice('__pex_got_'.length);
    if (!apiSyms[name]) throw new LinkError(`GOT slot for '${name}' which ${apiLabel} does not define (stale stubs?)`);
    const [g, f] = apiSyms[name];
    const off = (s.value - rwVma) >>> 0;
    if (!(off >= 0 && off < dataSize)) throw new LinkError(`GOT slot ${name} not inside .data`);
    const flags = weak.has(name) ? WEAK : 0;
    imports.push({ h: fnv1a(name), gid: g.id, flags, off, name });
    let gv = groups.get(g.id);
    if (!gv) { gv = [g.major, 0, g.name, true]; groups.set(g.id, gv); }
    gv[1] = Math.max(gv[1], f.since || 0);
    gv[3] = gv[3] && !!(flags & WEAK);
  }
  for (const w of weak) {
    if (!apiSyms[w]) throw new LinkError(`PEX_WEAK_IMPORT(${w}): not a host API symbol`);
  }
  imports.sort((a, b) => cmpNum(a.off, b.off));

  const exports = [];
  if (sec('.pex.exports')) {
    const b = elf.data(S['.pex.exports']);
    let i = 0;
    while (i + 4 <= b.length) {
      const val = u32le(b, i);
      let e = i + 4;
      while (e < b.length && b[e] !== 0) e++;
      const name = new TextDecoder().decode(b.subarray(i + 4, e));
      i = al(e + 1, 4);
      if (segOfAddr(val & ~1, 1) !== 'code') throw new LinkError(`PEX_EXPORT(${name}): not code`);
      exports.push({ h: fnv1a(name), val, kind: 1, name });
    }
  }
  const expNames = exports.map((e) => e.name);
  if (new Set(expNames).size !== expNames.length) throw new LinkError('duplicate PEX_EXPORT');
  if (new Set(exports.map((e) => e.h)).size !== exports.length) throw new LinkError('FNV-1a collision between exports: rename one');

  const fnlist = (name) => {
    if (!sec(name)) return [];
    const b = elf.data(S[name]);
    const out = [];
    for (let i = 0; i + 4 <= b.length; i += 4) {
      const v = u32le(b, i);
      if (v === 0 || v === 0xFFFFFFFF) continue;
      if (segOfAddr(v & ~1, 1) !== 'code' || !(v & 1)) throw new LinkError(`${name}: entry 0x${v.toString(16)} is not a Thumb function in the code`);
      out.push(v);
    }
    return out;
  };
  const inits = fnlist('.pex.init');
  const finis = fnlist('.pex.fini');

  const main = elf.sym('pex_main');
  let entry = 0;
  if (main && main.shndx && main.shndx < 0xFF00) {
    entry = main.value >>> 0;
    if (!(entry & 1)) throw new LinkError('pex_main is not a Thumb function');
  }

  const elfName = opts.elfName || opts.name || 'module';
  const versionStr = opts.version || '0.0.0';
  const meta = { name: opts.name || elfName.replace(/\.[^.]+$/, '').split('/').pop(), version: versionStr, tool: TOOL };
  if (opts.desc) meta.description = opts.desc;
  if (sec('.pex.meta')) {
    const parts = [];
    const raw = elf.data(S['.pex.meta']);
    let i = 0;
    while (i < raw.length) {
      let e = i;
      while (e < raw.length && raw[e] !== 0) e++;
      parts.push(new TextDecoder().decode(raw.subarray(i, e)));
      i = e + 1;
    }
    for (let i = 0; i + 1 < parts.length; i += 2) {
      const key = parts[i];
      if (!key) continue;
      if (!(key in META_TAGS)) throw new LinkError(`PEX_META_STR: unknown key '${key}' (known: ${Object.keys(META_TAGS).join(', ')})`);
      meta[key] = parts[i + 1];
    }
  }
  if (opts.kind === 'lua') {
    if (!('lua_name' in meta)) meta.lua_name = meta.name;
  }

  let flags = 0;
  if (model === 'pic') flags |= F.pic;
  if (opts.kind === 'app') flags |= F.app;
  if (opts.kind === 'lua' || opts.lua) flags |= F.lua_module;
  for (const fl of String(opts.flags || '').split(',').filter(Boolean)) {
    const k = fl.trim().replaceAll('-', '_');
    if (!(k in F) || k === 'pic') throw new LinkError(`unknown flag '${fl}'`);
    flags |= F[k];
  }
  if ((flags & F.reentrant) && !(flags & F.pic)) throw new LinkError('REENTRANT needs model B (pic): model A code is patched per instance');
  if (model === 'pic') flags |= F.xip_ok;

  let stack = opts.stackHint || 0;
  for (const su of opts.su || []) {
    for (const line of String(su).split('\n')) {
      const p = line.split('\t');
      if (p.length >= 2 && /^\d+$/.test(p[1].trim())) stack = Math.max(stack, parseInt(p[1].trim(), 10));
    }
  }
  const vm = String(versionStr).split('.').map((x) => parseInt(x, 10) || 0);
  while (vm.length < 3) vm.push(0);
  const moduleVersion = ((vm[0] << 24) | (vm[1] << 16) | (vm[2] & 0xffff)) >>> 0;

  const secs = [];
  roList.sort(cmpNum);
  rwList.sort(cmpNum);
  let reloc = pack('II', [roList.length, rwList.length]);
  reloc = concat([reloc, ...roList.map((x) => pack('I', [x])), ...rwList.map((x) => pack('I', [x]))]);
  secs.push([SEC.RELOC, 0, reloc]);

  const gl = [...groups.entries()].sort((a, b) => cmpNum(a[0], b[0]));
  let imp = pack('HH', [gl.length, imports.length]);
  for (const [gid, [maj, mnr]] of gl) imp = concat([imp, pack('HBB', [gid, maj, mnr])]);
  for (const im of imports) imp = concat([imp, pack('IHHI', [im.h, im.gid, im.flags, im.off])]);
  secs.push([SEC.IMPORTS, 0, imp]);

  if (exports.length) {
    let strtab = new Uint8Array(0);
    let ent = new Uint8Array(0);
    for (const e of exports) {
      const nb = new TextEncoder().encode(e.name);
      ent = concat([ent, pack('IIHHI', [e.h, e.val, e.kind, nb.length, strtab.length])]);
      const z = new Uint8Array(nb.length + 1);
      z.set(nb, 0);
      strtab = concat([strtab, z]);
    }
    secs.push([SEC.EXPORTS, 0, concat([pack('I', [exports.length]), ent, strtab])]);
  }
  if (inits.length) secs.push([SEC.INIT, 0, concat([pack('I', [inits.length]), ...inits.map((v) => pack('I', [v]))])]);
  if (finis.length) secs.push([SEC.FINI, 0, concat([pack('I', [finis.length]), ...finis.map((v) => pack('I', [v]))])]);

  let m = new Uint8Array(0);
  for (const [k, val] of Object.entries(meta)) {
    const vb = typeof val === 'string' ? new TextEncoder().encode(val) : pack('I', [val]);
    const rec = concat([pack('HH', [META_TAGS[k], vb.length]), vb]);
    m = concat([m, rec]);
    m = pad4(m);
  }
  secs.push([SEC.META, 0, m]);

  if (!opts.noDebug) {
    let dbg = new Uint8Array(0);
    const rec = (kind, val, name) => {
      const nb = new TextEncoder().encode(name).subarray(0, 255);
      let r = concat([pack('IHH', [val >>> 0, kind, nb.length]), nb]);
      r = pad4(r);
      return r;
    };
    for (const im of imports) dbg = concat([dbg, rec(1, im.h, im.name)]);
    const codeSyms = elf.syms.filter((s) => s.type === 2 && s.shndx && s.shndx < 0xFF00 && s.name && segOfShndx(s.shndx) === 'code');
    codeSyms.sort((a, b) => cmpNum(a.value, b.value));
    for (const s of codeSyms) dbg = concat([dbg, rec(2, s.value, s.name)]);
    secs.push([SEC.DEBUG, OPTIONAL, dbg]);
  }

  const nsec = secs.length;
  const off0 = HEADER_SIZE + nsec * 12;
  let body = new Uint8Array(0);
  let table = new Uint8Array(0);
  for (const [typ, fl, payload] of secs) {
    table = concat([table, pack('HHII', [typ, fl, off0 + body.length, payload.length])]);
    body = concat([body, pad4(payload)]);
  }
  const imageOff = al(off0 + body.length, 8);
  const gap = imageOff - off0 - body.length;
  body = concat([body, new Uint8Array(gap)]);
  const sigOff = al(imageOff + img.length, 4);
  const fileSize = sigOff;
  const feats = opts.features == null ? FEATURES[target] : (opts.features >>> 0);
  let hdr = Uint8Array.from(MAGIC);
  hdr = concat([hdr, pack('HHHHI', [1, 0, HEADER_SIZE, TARGETS[target], fileSize])]);
  hdr = concat([hdr, pack('IHHII', [feats, api.api.major, api.api.minor, flags >>> 0, moduleVersion])]);
  hdr = concat([hdr, pack('IIIIIII', [img.length, textSize, roSize >>> 0, dataSize >>> 0, bssSize >>> 0, stack >>> 0, entry])]);
  hdr = concat([hdr, pack('IIII', [nsec, HEADER_SIZE, imageOff, 0])]);
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', img));
  hdr = concat([hdr, digest.subarray(0, 16)]);
  if (hdr.length !== HEADER_SIZE) throw new LinkError(`header ${hdr.length} != ${HEADER_SIZE}`);
  let out = concat([hdr, table, body, img]);
  if (out.length < sigOff) out = concat([out, new Uint8Array(sigOff - out.length)]);
  putU32(out, CRC_OFF, crc32File(out));
  if (out.length !== fileSize) throw new LinkError('file size mismatch');
  const info = {
    text: textSize, ro: roSize, data: dataSize, bss: bssSize,
    roRelocs: roList.length, rwRelocs: rwList.length,
    imports: imports.length, exports: exports.length, file: out.length,
  };
  return { bytes: out, info };
}
