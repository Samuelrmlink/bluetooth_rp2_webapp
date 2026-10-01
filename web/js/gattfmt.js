// How a characteristic's value is shown and typed, what it permits, and how
// its UUID reads. DOM-free, so all of it is unit-tested (tests/unit/gattfmt.test.js).
//
// Values travel as lowercase hex (the firmware's form, both ways). A format
// is a view of those bytes:
//   hex   "64 00 ff"
//   dec   "100 0 255"   -- each byte in decimal: any length, exact both ways
//   str   UTF-8 text; bytes that are not printable UTF-8 appear as \xNN (and
//         \0 \n \r \t \\), and the same escapes are accepted when typing

import { shortUuid, uuidLabel } from './gattdb.js';

export const FORMATS = [
  ['hex', 'Hex'],
  ['dec', 'Decimal'],
  ['str', 'String'],
];

const toBytes = (hex) => {
  const h = String(hex || '').replace(/[^0-9a-f]/gi, '');
  const out = new Uint8Array(h.length >> 1);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(h.substr(i * 2, 2), 16);
  return out;
};
const toHex = (bytes) => [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');

const ESC_OUT = { 0: '\\0', 9: '\\t', 10: '\\n', 13: '\\r', 92: '\\\\' };

export function formatValue(hex, fmt) {
  const b = toBytes(hex);
  if (fmt === 'dec') return [...b].join(' ');
  if (fmt === 'str') {
    // Decode strictly so invalid sequences can be shown as the bytes they are.
    let out = '';
    let i = 0;
    while (i < b.length) {
      const c = b[i];
      const need = c >= 0xf0 && c <= 0xf4 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c <= 0xdf ? 2 : c < 0x80 ? 1 : 0;
      if (need > 1 && i + need <= b.length) {
        try {
          out += new TextDecoder('utf-8', { fatal: true }).decode(b.subarray(i, i + need));
          i += need;
          continue;
        } catch { /* not valid: fall through to \x */ }
      }
      if (need === 1 && c >= 0x20 && c !== 0x7f && c !== 92) out += String.fromCharCode(c);
      else if (ESC_OUT[c] !== undefined) out += ESC_OUT[c];
      else out += `\\x${c.toString(16).padStart(2, '0')}`;
      i += 1;
    }
    return out;
  }
  return [...b].map((x) => x.toString(16).padStart(2, '0')).join(' ');
}

// Typed text -> { ok, hex } or { ok: false, error }.
export function parseInput(text, fmt) {
  const t = String(text ?? '');
  if (fmt === 'dec') {
    const parts = t.trim() ? t.trim().split(/[\s,]+/) : [];
    const out = [];
    for (const p of parts) {
      if (!/^\d+$/.test(p) || Number(p) > 255) return { ok: false, error: `"${p}" is not a byte (0–255)` };
      out.push(Number(p));
    }
    return { ok: true, hex: toHex(out) };
  }
  if (fmt === 'str') {
    const out = [];
    const enc = new TextEncoder();
    for (let i = 0; i < t.length; i++) {
      if (t[i] !== '\\') { out.push(...enc.encode(t[i] === undefined ? '' : String.fromCodePoint(t.codePointAt(i)))); if (t.codePointAt(i) > 0xffff) i++; continue; }
      const n = t[i + 1];
      const simple = { 0: 0, n: 10, r: 13, t: 9, '\\': 92 };
      if (n in simple) { out.push(simple[n]); i++; continue; }
      if (n === 'x' && /^[0-9a-f]{2}$/i.test(t.slice(i + 2, i + 4))) { out.push(parseInt(t.slice(i + 2, i + 4), 16)); i += 3; continue; }
      return { ok: false, error: `unknown escape \\${n ?? ''} (use \\xNN, \\0, \\n, \\r, \\t, \\\\)` };
    }
    return { ok: true, hex: toHex(out) };
  }
  const h = t.replace(/0x/gi, '').replace(/[\s:,-]/g, '');
  if (!/^[0-9a-f]*$/i.test(h)) return { ok: false, error: 'hex digits only' };
  if (h.length % 2) return { ok: false, error: 'an odd number of hex digits' };
  return { ok: true, hex: h.toLowerCase() };
}

export const byteLength = (hex) => String(hex || '').replace(/[^0-9a-f]/gi, '').length >> 1;

// What a characteristic IS when we know it, so its value starts readable:
// strings from Device Information and GAP, a percentage as decimal.
const DEFAULTS = {
  '2a00': 'str', '2a24': 'str', '2a25': 'str', '2a26': 'str', '2a27': 'str', '2a28': 'str', '2a29': 'str',
  '2a19': 'dec',
};
export function defaultFormat(charUuid) {
  return DEFAULTS[shortUuid(charUuid)] || 'hex';
}

// The key a remembered format is stored under.
export const formatKey = (svcUuid, charUuid) => `${shortUuid(svcUuid)}/${shortUuid(charUuid)}`;

// Remember a choice, capped so the browser's store cannot grow without end:
// oldest first out (insertion order), and a re-chosen key moves to the end.
export function rememberFormat(map, key, fmt, cap = 500) {
  const next = { ...(map || {}) };
  delete next[key];
  next[key] = fmt;
  const keys = Object.keys(next);
  for (let i = 0; i < keys.length - cap; i++) delete next[keys[i]];
  return next;
}

const PERM_NAMES = [
  ['read', 'Read'],
  ['write', 'Write'],
  ['write_no_response', 'Write (no response)'],
  ['notify', 'Notify'],
  ['indicate', 'Indicate'],
  ['authenticated_signed_writes', 'Signed write'],
  ['broadcast', 'Broadcast'],
  ['extended', 'Extended properties'],
];
export function permissions(flags = []) {
  const f = new Set(flags);
  return PERM_NAMES.filter(([k]) => f.has(k)).map(([, label]) => label);
}

// Which controls a characteristic gets: only what it permits.
export function actionsFor(flags = []) {
  const f = new Set(flags);
  const write = f.has('write');
  const writeNoResp = f.has('write_no_response');
  return {
    read: f.has('read'),
    notify: f.has('notify'),
    indicate: f.has('indicate'),
    write: write || writeNoResp,
    // The "with response" choice exists only when both kinds are allowed;
    // otherwise the one that is allowed is used.
    chooseResponse: write && writeNoResp,
    response: write,
  };
}

// A 16-bit UUID (or a SIG-base 128-bit one, which is the same thing) always
// shows; a custom 128-bit one shows as its name when known, else abbreviated,
// unless `full` asks for the whole of it.
export function uuidDisplay(uuid, full = false) {
  const s = shortUuid(uuid);
  const is128 = s.length > 8;
  const label = uuidLabel(uuid);
  const name = label !== s ? label.slice(s.length).trim() : '';
  if (!is128) return { text: name ? `${s} ${name}` : s, title: s, is128 };
  const short = `${s.slice(0, 8)}…`;
  return { text: full ? s : (name || short), title: s, is128, name };
}
