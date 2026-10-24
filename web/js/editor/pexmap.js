// Map and Thumb listing for an unsigned .pex. The link address of a symbol
// is its value in the image (bit 0 set on a Thumb function). After install
// the SWD address is:
//   0x10400000 + partition_offset + image_off + (value & ~1)
// partition_offset is pex.install's offset (a 4 KB sector times 4096).
// image_off is the header field at byte 68. No probe is driven from here.

export const XIP_BASE = 0x10400000;

function hex(n) {
  return (n >>> 0).toString(16);
}

function hex8(n) {
  return (n >>> 0).toString(16).padStart(8, '0');
}

function u16(b, off) {
  return b[off] | (b[off + 1] << 8);
}

const REGS = ['r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12', 'sp', 'lr', 'pc'];

function rlist(mask, special) {
  const names = [];
  for (let i = 0; i < 8; i++) if (mask & (1 << i)) names.push(REGS[i]);
  if (special) names.push(special);
  return `{${names.join(', ')}}`;
}

/** One 16-bit Thumb instruction, or null when this halfword starts a 32-bit one. */
export function dis16(hw) {
  if ((hw & 0xF800) >= 0xE800) return null;
  const rd = hw & 7;
  const rm = (hw >> 3) & 7;
  const rn = (hw >> 6) & 7;
  const imm5 = (hw >> 6) & 31;
  const imm3 = (hw >> 6) & 7;

  if ((hw & 0xF800) === 0x0000) return `lsls ${REGS[rd]}, ${REGS[rm]}, #${imm5}`;
  if ((hw & 0xF800) === 0x0800) return `lsrs ${REGS[rd]}, ${REGS[rm]}, #${imm5 || 32}`;
  if ((hw & 0xF800) === 0x1000) return `asrs ${REGS[rd]}, ${REGS[rm]}, #${imm5 || 32}`;
  if ((hw & 0xFC00) === 0x1800) return `${(hw & 0x0200) ? 'subs' : 'adds'} ${REGS[rd]}, ${REGS[rn]}, ${REGS[rm]}`;
  if ((hw & 0xFC00) === 0x1C00) return `${(hw & 0x0200) ? 'subs' : 'adds'} ${REGS[rd]}, ${REGS[rn]}, #${imm3}`;
  if ((hw & 0xE000) === 0x2000) {
    const op = (hw >> 11) & 3;
    const name = ['movs', 'cmp', 'adds', 'subs'][op];
    return `${name} ${REGS[(hw >> 8) & 7]}, #${hw & 0xFF}`;
  }
  if ((hw & 0xFC00) === 0x4000) {
    const op = (hw >> 6) & 15;
    const name = ['ands', 'eors', 'lsls', 'lsrs', 'asrs', 'adcs', 'sbcs', 'rors', 'tst', 'rsbs', 'cmp', 'cmn', 'orrs', 'muls', 'bics', 'mvns'][op];
    return `${name} ${REGS[rd]}, ${REGS[rm]}`;
  }
  if ((hw & 0xFC00) === 0x4400) {
    const d = ((hw >> 4) & 8) | rd;
    const m = ((hw >> 3) & 15);
    const op = (hw >> 8) & 3;
    if (op === 3) return `${(hw & 0x80) ? 'blx' : 'bx'} ${REGS[m]}`;
    const name = ['add', 'cmp', 'mov'][op];
    return `${name} ${REGS[d]}, ${REGS[m]}`;
  }
  if ((hw & 0xF800) === 0x4800) return `ldr ${REGS[(hw >> 8) & 7]}, [pc, #${(hw & 0xFF) << 2}]`;
  if ((hw & 0xF200) === 0x5000) {
    const kind = (hw >> 9) & 7;
    const name = ['str', 'strh', 'strb', 'ldrsb', 'ldr', 'ldrh', 'ldrb', 'ldrsh'][kind];
    return `${name} ${REGS[rd]}, [${REGS[rn]}, ${REGS[rm]}]`;
  }
  if ((hw & 0xE000) === 0x6000) {
    const kind = (hw >> 11) & 3;
    const name = ['str', 'ldr', 'strb', 'ldrb'][kind];
    const sh = kind < 2 ? 2 : 0;
    return `${name} ${REGS[rd]}, [${REGS[rn]}, #${imm5 << sh}]`;
  }
  if ((hw & 0xF000) === 0x8000) {
    const name = (hw & 0x0800) ? 'ldrh' : 'strh';
    return `${name} ${REGS[rd]}, [${REGS[rn]}, #${imm5 << 1}]`;
  }
  if ((hw & 0xF000) === 0x9000) {
    const name = (hw & 0x0800) ? 'ldr' : 'str';
    return `${name} ${REGS[(hw >> 8) & 7]}, [sp, #${(hw & 0xFF) << 2}]`;
  }
  if ((hw & 0xF800) === 0xA000) return `adr ${REGS[(hw >> 8) & 7]}, #${(hw & 0xFF) << 2}`;
  if ((hw & 0xF800) === 0xA800) return `add ${REGS[(hw >> 8) & 7]}, sp, #${(hw & 0xFF) << 2}`;
  if ((hw & 0xFF00) === 0xB000) return `${(hw & 0x80) ? 'sub' : 'add'} sp, #${(hw & 0x7F) << 2}`;
  if ((hw & 0xF600) === 0xB400) {
    const pop = (hw & 0x0800) !== 0;
    return `${pop ? 'pop' : 'push'} ${rlist(hw & 0xFF, (hw & 0x100) ? (pop ? 'pc' : 'lr') : '')}`;
  }
  if ((hw & 0xF000) === 0xC000) {
    const name = (hw & 0x0800) ? 'ldmia' : 'stmia';
    return `${name} ${REGS[(hw >> 8) & 7]}!, ${rlist(hw & 0xFF, '')}`;
  }
  if ((hw & 0xFF00) === 0xDF00) return `svc #${hw & 0xFF}`;
  if ((hw & 0xF000) === 0xD000) {
    const cond = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', '', ''][(hw >> 8) & 15];
    if (!cond) return `udf #${hw & 0xFF}`;
    let imm = hw & 0xFF;
    if (imm & 0x80) imm -= 0x100;
    return `b${cond} #${(imm << 1) + 4}`;
  }
  if ((hw & 0xF800) === 0xE000) {
    let imm = hw & 0x7FF;
    if (imm & 0x400) imm -= 0x800;
    return `b #${(imm << 1) + 4}`;
  }
  return `.short 0x${hw.toString(16).padStart(4, '0')}`;
}

function dis32(hw1, hw2) {
  if ((hw1 & 0xF800) === 0xF000 && (hw2 & 0xD000) === 0xD000) {
    const s = (hw1 >> 10) & 1;
    const j1 = (hw2 >> 13) & 1;
    const j2 = (hw2 >> 11) & 1;
    const i1 = 1 - (j1 ^ s);
    const i2 = 1 - (j2 ^ s);
    let imm = (s << 24) | (i1 << 23) | (i2 << 22) | (((hw1 & 0x3FF) << 12) | ((hw2 & 0x7FF) << 1));
    if (s) imm |= 0xFE000000;
    imm = (imm | 0) + 4;
    const blx = (hw2 & 0x1000) === 0;
    return `${blx ? 'blx' : 'bl'} #${imm}`;
  }
  return `.inst.w 0x${((hw1 << 16) | hw2).toString(16).padStart(8, '0')}`;
}

/**
 * Thumb listing of the text image. `symbols` are {name, value} with the
 * Thumb bit still set. Addresses are image offsets.
 */
export function renderDis(text, symbols = []) {
  const bytes = text instanceof Uint8Array ? text : new Uint8Array(0);
  const at = new Map();
  for (const s of symbols) {
    const off = (s.value & ~1) >>> 0;
    if (!at.has(off)) at.set(off, []);
    at.get(off).push(s.name);
  }
  const lines = [];
  for (let i = 0; i + 1 < bytes.length;) {
    for (const name of at.get(i) || []) lines.push(`${hex8(i)} <${name}>:`);
    const hw = u16(bytes, i);
    if ((hw & 0xF800) >= 0xE800 && i + 3 < bytes.length) {
      const hw2 = u16(bytes, i + 2);
      const raw = bytes.subarray(i, i + 4);
      const hexb = [...raw].map((b) => b.toString(16).padStart(2, '0')).join('');
      lines.push(`  ${hex8(i)}:\t${hexb} \t${dis32(hw, hw2)}`);
      i += 4;
    } else {
      const raw = bytes.subarray(i, i + 2);
      const hexb = [...raw].map((b) => b.toString(16).padStart(2, '0')).join('');
      lines.push(`  ${hex8(i)}:\t${hexb}     \t${dis16(hw) || '.short'}`);
      i += 2;
    }
  }
  if (bytes.length & 1) lines.push(`  ${hex8(bytes.length - 1)}:\t${bytes[bytes.length - 1].toString(16).padStart(2, '0')}       \t.byte`);
  return `${lines.join('\n')}\n`;
}

export function renderMap({ tool, name, arch, imageOff, text, ro, data, bss, symbols }) {
  const lines = [
    `# ${tool}  ${name}  ${arch}`,
    `# image_off 0x${hex(imageOff)}`,
    `# text ${text}  ro ${ro}  data ${data}  bss ${bss}`,
    '# SWD address after install:',
    '#   0x10400000 + partition_offset + image_off + (value & ~1)',
    '# partition_offset is the offset pex.install reports.',
    '# value is the link address. Bit 0 is the Thumb bit.',
  ];
  const sorted = [...symbols].sort((a, b) => ((a.value >>> 0) - (b.value >>> 0)) || a.name.localeCompare(b.name));
  for (const s of sorted) lines.push(`${hex8(s.value)} ${s.kind || 'T'} ${s.name}`);
  return `${lines.join('\n')}\n`;
}

/** Lines appended to the map once pex.install has reported an offset. */
export function installMapNote({ offset, imageOff, symbol = null }) {
  const base = (XIP_BASE + (offset >>> 0) + (imageOff >>> 0)) >>> 0;
  const lines = [
    '',
    '# installed',
    `partition_offset 0x${hex(offset >>> 0)}`,
    `image_off 0x${hex(imageOff >>> 0)}`,
  ];
  if (symbol) {
    const swd = (base + (symbol.value & ~1)) >>> 0;
    lines.push(`# ${symbol.name} value 0x${hex(symbol.value)} swd 0x${hex(swd)}`);
  } else {
    lines.push(`# code base 0x${hex(base)}`);
  }
  return `${lines.join('\n')}\n`;
}

export function imageOffOf(bytes) {
  if (!bytes || bytes.length < 72) return 0;
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(68, true);
}

export function buildIdOf(bytes) {
  if (!bytes || bytes.length < 92) return '';
  return [...bytes.subarray(76, 92)].map((b) => b.toString(16).padStart(2, '0')).join('');
}
