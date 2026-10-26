import { test } from 'node:test';
import assert from 'node:assert/strict';
import { archivePlan, pruneCovered } from '../../js/editor/archive.js';
import { encodeTar, stripTarEnd, appendTar, parseTar } from '../../js/tar.js';

const isDir = (p) => p === 'lua' || p === 'sd' || p === 'sd/logs';

test('a child of a picked folder is not packed twice', () => {
  assert.deepEqual(pruneCovered(['lua', 'lua/a.lua', 'cfg/x.json'], isDir), ['lua', 'cfg/x.json']);
});

test('one flash folder is a GET, a chosen list is a POST, and the card is packed here', () => {
  assert.deepEqual(archivePlan(['lua'], isDir), { kind: 'get', folder: 'lua' });
  assert.deepEqual(archivePlan(['lua/a.lua', 'cfg/x.json'], isDir), { kind: 'post', flash: ['lua/a.lua', 'cfg/x.json'] });
  assert.deepEqual(archivePlan(['sd/logs/a.txt'], isDir), { kind: 'card', card: ['sd/logs/a.txt'] });
  assert.deepEqual(archivePlan(['lua', 'sd/logs/a.txt'], isDir), { kind: 'mix-get', folder: 'lua', card: ['sd/logs/a.txt'] });
  assert.equal(archivePlan(['cfg/x.json', 'sd/a.txt'], isDir).kind, 'mix-post');
});

test('a tar written here is the ustar the restore parser reads, and a mix strips the end blocks', () => {
  const hi = new TextEncoder().encode('hi');
  const arch = encodeTar([{ name: 'lua/a.txt', data: hi }], 1700000000);
  const parsed = parseTar(arch);
  assert.equal(parsed.entries.length, 1);
  assert.equal(parsed.entries[0].path, 'lua/a.txt');
  assert.equal(new TextDecoder().decode(parsed.entries[0].data), 'hi');
  const more = appendTar(arch, [{ name: 'sd/b.txt', data: new TextEncoder().encode('yo') }], 1700000000);
  const both = parseTar(more);
  assert.deepEqual(both.entries.map((e) => e.path), ['lua/a.txt', 'sd/b.txt']);
  assert.equal(stripTarEnd(arch).length, arch.length - 1024);
});
