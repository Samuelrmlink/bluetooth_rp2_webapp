// FNV-1a, 32-bit, over the UTF-8 bytes of a string -- the same function the
// firmware uses in src/lua/lrt.c, so the editor can compare its buffer with
// what script.loaded says was compiled. Detects change; authenticates nothing.
const enc = new TextEncoder();

export function fnv1a(text) {
  const b = typeof text === 'string' ? enc.encode(text) : text;
  let h = 0x811c9dc5;
  for (let i = 0; i < b.length; i++) {
    h ^= b[i];
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h.toString(16).padStart(8, '0');
}
