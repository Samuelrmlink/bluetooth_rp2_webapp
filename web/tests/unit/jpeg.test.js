import { test } from 'node:test';
import assert from 'node:assert/strict';
import { encodeJpeg } from '../../js/devices/anker-scp/jpeg.js';

// The DQT and DHT segments of the Anker app's own upload to the A2687
// (a2687-re captures 2026-10-01, rebuilt from the decrypted snoop log).
const APP_DQT = '000302020302020303030304030304050805050404050a070706080c0a0c0c0b0a0b0b0d0e12100d0e110e0b0b1016101113141515150c0f1718161418121415140103040405040509050509140d0b0d1414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414';
const APP_DHT = '0000010501010101010100000000000000000102030405060708090a0b100002010303020403050504040000017d01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa0100030101010101010101010000000000000102030405060708090a0b1100020102040403040705040400010277000102031104052131061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a262728292a35363738393a434445464748494a535455565758595a636465666768696a737475767778797a82838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9fa';

function picture(w, h) {
  const data = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const p = (y * w + x) * 4;
    data[p] = x; data[p + 1] = y; data[p + 2] = (x * y) & 255; data[p + 3] = 255;
  }
  return { width: w, height: h, data };
}

function segments(b) {
  const out = {};
  let i = 2;
  while (i < b.length) {
    const m = b[i + 1], len = (b[i + 2] << 8) | b[i + 3];
    (out[m] ||= []).push(b.subarray(i + 4, i + 2 + len));
    if (m === 0xDA) break;
    i += 2 + len;
  }
  return out;
}
const hex = (u8) => Buffer.from(u8).toString('hex');

test('the charger\'s JPEG format: the app\'s own tables, byte for byte', () => {
  const j = encodeJpeg(picture(240, 240), 90);
  const s = segments(j);
  assert.equal(hex(s[0xDB][0]), APP_DQT, 'quality 90 quantisation tables');
  assert.equal(hex(s[0xC4][0]), APP_DHT, 'standard Huffman tables');
  assert.equal(s[0xDB].length, 1);
  assert.equal(s[0xC4].length, 1);
});

test('baseline 4:4:4, JFIF only, SOI to EOI', () => {
  const j = encodeJpeg(picture(240, 240), 90);
  const s = segments(j);
  assert.deepEqual([j[0], j[1], j.at(-2), j.at(-1)], [0xFF, 0xD8, 0xFF, 0xD9]);
  assert.equal(hex(s[0xE0][0]), '4a46494600010100000100010000');
  assert.equal(hex(s[0xC0][0]), '0800f000f003011100021101031101');   // 8-bit 240x240, all 1x1
  for (const m of [0xE1, 0xE2, 0xFE, 0xC2, 0xDD]) assert.equal(s[m], undefined, `no 0x${m.toString(16)}`);
});

test('a lower quality is smaller', () => {
  assert.ok(encodeJpeg(picture(240, 240), 60).length < encodeJpeg(picture(240, 240), 90).length);
});
