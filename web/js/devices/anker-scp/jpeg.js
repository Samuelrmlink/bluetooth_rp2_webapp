// A baseline JPEG encoder that writes exactly the format the Anker app sends
// the A2687 -- measured on the app's own upload (a2687-re captures, 2026-10-01):
//
//   APP0 JFIF 1.01, 1:1 density; no EXIF, no ICC profile, no comments
//   DQT  one segment, both tables: the standard IJG tables scaled for quality 90
//   SOF0 baseline, 8-bit, 3 components (YCbCr), all 1x1 -- 4:4:4, no subsampling
//   DHT  one segment, the four standard Huffman tables (JPEG Annex K)
//   SOS  one scan, no restart markers
//
// The browser's own canvas.toBlob('image/jpeg') does not: it subsamples
// chroma 4:2:0, embeds an ICC profile and optimises the Huffman tables. The
// charger's decoder may cope with that, but nothing shows it does, so the
// window encodes with this instead and uses the canvas only to crop and scale.
// It is the classic AAN-DCT encoder (the one Dart's `image` package ports,
// which is what the app uses).

const ZZ = [0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42, 3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
  10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63];
const YQT = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
  18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99];
const UVQT = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
  ...new Array(32).fill(99)];
const AASF = [1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379];

const DC_NR = [0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0];
const DC_V = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
const AC_NR = [0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d];
const AC_V = [
  0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
  0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
  0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
  0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
  0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
  0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
  0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
  0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
  0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
  0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
  0xf9, 0xfa];
const DCC_NR = [0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0];
const ACC_NR = [0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77];
const ACC_V = [
  0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
  0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
  0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
  0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
  0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
  0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
  0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
  0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
  0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
  0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
  0xf9, 0xfa];

// value -> [code, length], built the canonical way from the counts.
function huff(nr, vals) {
  const t = [];
  let code = 0, k = 0;
  for (let len = 1; len <= 16; len++) {
    for (let j = 0; j < nr[len]; j++) t[vals[k++]] = [code++, len];
    code <<= 1;
  }
  return t;
}
const HT = { ydc: huff(DC_NR, DC_V), yac: huff(AC_NR, AC_V), cdc: huff(DCC_NR, DC_V), cac: huff(ACC_NR, ACC_V) };

// The quantisation tables (zigzag order, as written) and the matching
// DCT-domain divisors (natural order), for one quality.
export function tables(quality) {
  const q = Math.max(1, Math.min(100, Math.round(quality)));
  const sf = q < 50 ? Math.floor(5000 / q) : Math.floor(200 - q * 2);
  const y = new Uint8Array(64), uv = new Uint8Array(64);
  for (let i = 0; i < 64; i++) {
    y[ZZ[i]] = Math.max(1, Math.min(255, Math.floor((YQT[i] * sf + 50) / 100)));
    uv[ZZ[i]] = Math.max(1, Math.min(255, Math.floor((UVQT[i] * sf + 50) / 100)));
  }
  const fy = new Float64Array(64), fuv = new Float64Array(64);
  let k = 0;
  for (let r = 0; r < 8; r++) {
    for (let c = 0; c < 8; c++, k++) {
      fy[k] = 1 / (y[ZZ[k]] * AASF[r] * AASF[c] * 8);
      fuv[k] = 1 / (uv[ZZ[k]] * AASF[r] * AASF[c] * 8);
    }
  }
  return { y, uv, fy, fuv };
}

class Bits {
  constructor() { this.out = []; this.acc = 0; this.n = 0; }
  byte(b) { this.out.push(b & 0xFF); }
  word(w) { this.byte(w >> 8); this.byte(w); }
  bytes(a) { for (const b of a) this.byte(b); }
  put([code, len]) {
    this.acc = (this.acc << len) | code;
    this.n += len;
    while (this.n >= 8) {
      const b = (this.acc >> (this.n - 8)) & 0xFF;
      this.byte(b);
      if (b === 0xFF) this.byte(0);               // byte stuffing
      this.n -= 8;
      this.acc &= (1 << this.n) - 1;
    }
  }
  flush() { if (this.n > 0) this.put([(1 << (8 - this.n)) - 1, 8 - this.n]); }
}

// Forward DCT (AAN, float) in place, then quantise into zigzag order.
function fdctQuant(d, f, out) {
  for (let s = 0; s < 2; s++) {
    const step = s ? 8 : 1, line = s ? 1 : 8;
    for (let i = 0; i < 8; i++) {
      const o = i * line;
      const d0 = d[o], d1 = d[o + step], d2 = d[o + 2 * step], d3 = d[o + 3 * step];
      const d4 = d[o + 4 * step], d5 = d[o + 5 * step], d6 = d[o + 6 * step], d7 = d[o + 7 * step];
      const t0 = d0 + d7, t7 = d0 - d7, t1 = d1 + d6, t6 = d1 - d6;
      const t2 = d2 + d5, t5 = d2 - d5, t3 = d3 + d4, t4 = d3 - d4;
      let t10 = t0 + t3; const t13 = t0 - t3;
      let t11 = t1 + t2; let t12 = t1 - t2;
      d[o] = t10 + t11;
      d[o + 4 * step] = t10 - t11;
      const z1 = (t12 + t13) * 0.707106781;
      d[o + 2 * step] = t13 + z1;
      d[o + 6 * step] = t13 - z1;
      t10 = t4 + t5; t11 = t5 + t6; t12 = t6 + t7;
      const z5 = (t10 - t12) * 0.382683433;
      const z2 = 0.541196100 * t10 + z5;
      const z4 = 1.306562965 * t12 + z5;
      const z3 = t11 * 0.707106781;
      const z11 = t7 + z3, z13 = t7 - z3;
      d[o + 5 * step] = z13 + z2;
      d[o + 3 * step] = z13 - z2;
      d[o + step] = z11 + z4;
      d[o + 7 * step] = z11 - z4;
    }
  }
  for (let i = 0; i < 64; i++) {
    const v = d[i] * f[i];
    out[ZZ[i]] = v > 0 ? Math.floor(v + 0.5) : Math.ceil(v - 0.5);
  }
}

function category(v) {
  let a = Math.abs(v), n = 0;
  while (a) { n++; a >>= 1; }
  return n;
}
function amplitude(v, n) { return [v < 0 ? v + (1 << n) - 1 : v, n]; }

function block(bits, du, dc, hdc, hac) {
  const diff = du[0] - dc;
  const c = category(diff);
  bits.put(hdc[c]);
  if (c) bits.put(amplitude(diff, c));
  let end = 63;
  while (end > 0 && du[end] === 0) end--;
  let i = 1;
  while (i <= end) {
    let run = 0;
    while (du[i] === 0 && i <= end) { run++; i++; }
    while (run >= 16) { bits.put(hac[0xF0]); run -= 16; }
    const n = category(du[i]);
    bits.put(hac[(run << 4) + n]);
    bits.put(amplitude(du[i], n));
    i++;
  }
  if (end !== 63) bits.put(hac[0x00]);           // end of block
  return du[0];
}

/**
 * Encode RGBA pixels ({ width, height, data } -- an ImageData) as the
 * charger's JPEG. Alpha is ignored; draw onto an opaque background first.
 * Returns a Uint8Array.
 */
export function encodeJpeg({ width, height, data }, quality = 90) {
  const t = tables(quality);
  const bits = new Bits();
  bits.word(0xFFD8);
  bits.word(0xFFE0); bits.word(16);
  bits.bytes([0x4A, 0x46, 0x49, 0x46, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0]);
  bits.word(0xFFDB); bits.word(132);
  bits.byte(0); bits.bytes(t.y);
  bits.byte(1); bits.bytes(t.uv);
  bits.word(0xFFC0); bits.word(17); bits.byte(8);
  bits.word(height); bits.word(width);
  bits.bytes([3, 1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1]);
  bits.word(0xFFC4); bits.word(418);
  for (const [cls, nr, v] of [[0x00, DC_NR, DC_V], [0x10, AC_NR, AC_V], [0x01, DCC_NR, DC_V], [0x11, ACC_NR, ACC_V]]) {
    bits.byte(cls);
    bits.bytes(nr.slice(1));
    bits.bytes(v);
  }
  bits.word(0xFFDA); bits.word(12);
  bits.bytes([3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 0x3F, 0]);

  const Y = new Float64Array(64), U = new Float64Array(64), V = new Float64Array(64);
  const du = new Int32Array(64);
  let dcy = 0, dcu = 0, dcv = 0;
  for (let by = 0; by < height; by += 8) {
    for (let bx = 0; bx < width; bx += 8) {
      for (let k = 0; k < 64; k++) {
        const x = Math.min(bx + (k & 7), width - 1);
        const y = Math.min(by + (k >> 3), height - 1);
        const p = (y * width + x) * 4;
        const r = data[p], g = data[p + 1], b = data[p + 2];
        Y[k] = 0.29900 * r + 0.58700 * g + 0.11400 * b - 128;
        U[k] = -0.16874 * r - 0.33126 * g + 0.50000 * b;
        V[k] = 0.50000 * r - 0.41869 * g - 0.08131 * b;
      }
      fdctQuant(Y, t.fy, du); dcy = block(bits, du, dcy, HT.ydc, HT.yac);
      fdctQuant(U, t.fuv, du); dcu = block(bits, du, dcu, HT.cdc, HT.cac);
      fdctQuant(V, t.fuv, du); dcv = block(bits, du, dcv, HT.cdc, HT.cac);
    }
  }
  bits.flush();
  bits.word(0xFFD9);
  return Uint8Array.from(bits.out);
}
