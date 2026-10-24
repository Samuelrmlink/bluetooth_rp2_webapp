import { test } from 'node:test';
import assert from 'node:assert/strict';
import { PEX_PART, SECTOR, partitionModel } from '../../js/editor/pexpart.js';

const live = {
  mounted: true, size: 262144, free: 0, largest: 0, seq: 42, moves: 0, rw_area: 23488,
  modules: [
    { name: 'iti', version: '1.0.0', model: 'pic', offset: 8192, size: 84896, code: 74060, rw: 7640, signed: true, loaded: false, xip_ready: true },
    { name: 'anker', version: '1.0.0', model: 'reloc', offset: 94208, size: 164596, code: 136400, rw: 23488, signed: true, loaded: true, xip_ready: true },
  ],
};

test('the partition tab is one sentinel path', () => {
  assert.equal(PEX_PART, 'pex://partition');
  assert.equal(SECTOR, 4096);
});

test('a full partition: index, then the two modules, and no free cell', () => {
  const m = partitionModel(live);
  assert.equal(m.sectors, 64);
  assert.equal(m.indexBytes, 8192);
  assert.equal(m.used, 262144 - 8192);
  assert.equal(m.free, 0);
  assert.equal(m.fragmented, false);
  assert.equal(m.cells.filter((c) => c.kind === 'index').length, 2);
  assert.equal(m.cells.filter((c) => c.kind === 'free').length, 0);
  assert.equal(m.cells[2].name, 'iti');
  assert.equal(m.cells[22].name, 'iti');
  assert.equal(m.cells[23].name, 'anker');
  assert.equal(m.cells[63].name, 'anker');
  assert.deepEqual(m.modules.map((x) => x.name), ['iti', 'anker']);
});

test('free space in more than one piece is fragmented', () => {
  const m = partitionModel({
    mounted: true, size: 32768, free: 12288, largest: 4096, rw_area: 0,
    modules: [{ name: 'a', offset: 8192, size: 4096 }],
  });
  assert.equal(m.fragmented, true);
  assert.equal(m.used, 32768 - 8192 - 12288);
  const kinds = m.cells.map((c) => c.kind === 'module' ? c.name : c.kind);
  assert.deepEqual(kinds, ['index', 'index', 'a', 'free', 'free', 'free', 'free', 'free']);
});
