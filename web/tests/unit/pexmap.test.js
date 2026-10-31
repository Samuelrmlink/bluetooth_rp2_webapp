import { test } from 'node:test';
import assert from 'node:assert/strict';
import { dis16, renderDis, renderMap, installMapNote, buildIdOf, imageOffOf, XIP_BASE } from '../../js/editor/pexmap.js';

test('thumb listing names a known push, and the map records the install formula', () => {
  assert.equal(dis16(0xb580), 'push {r7, lr}');
  assert.equal(dis16(0x4770), 'bx lr');
  const text = new Uint8Array([0x80, 0xb5, 0x70, 0x47]);
  const dis = renderDis(text, [{ name: 'pex_main', value: 1 }]);
  assert.match(dis, /<pex_main>:/);
  assert.match(dis, /push \{r7, lr\}/);
  const map = renderMap({
    tool: 'pexlink 1.1', name: 'hello', arch: 'm33', imageOff: 0x80,
    text: 4, ro: 0, data: 0, bss: 0, symbols: [{ name: 'pex_main', value: 1, kind: 'T' }],
  });
  assert.match(map, /0x10400000 \+ partition_offset \+ image_off/);
  assert.match(map, /00000001 T pex_main/);
  const note = installMapNote({ offset: 94208, imageOff: 0x80, symbol: { name: 'pex_main', value: 1 } });
  const swd = (XIP_BASE + 94208 + 0x80 + (1 & ~1)) >>> 0;
  assert.match(note, new RegExp(`swd 0x${swd.toString(16)}`));
  const header = new Uint8Array(92);
  header[68] = 0x80;
  header[76] = 0xab;
  header[77] = 0xcd;
  assert.equal(imageOffOf(header), 0x80);
  assert.equal(buildIdOf(header).slice(0, 4), 'abcd');
});
