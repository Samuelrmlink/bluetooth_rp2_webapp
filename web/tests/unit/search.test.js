import { test } from 'node:test';
import assert from 'node:assert/strict';
import { compileQuery, searchText, searchable, searchFiles } from '../../js/editor/search.js';

test('compileQuery: literal by default, case-insensitive; options; bad regex is an error', () => {
  assert.ok(compileQuery({ q: 'a.b' }).re.test('xA.Bx'));
  assert.equal(compileQuery({ q: 'a.b' }).re.test('axb'), false);
  assert.equal(compileQuery({ q: 'A', matchCase: true }).re.test('a'), false);
  assert.ok(compileQuery({ q: 'a.b', regex: true }).re.test('axb'));
  assert.equal(compileQuery({ q: 'log', word: true }).re.test('dialog'), false);
  assert.ok(compileQuery({ q: 'log', word: true }).re.test('log.info'));
  assert.match(compileQuery({ q: '(', regex: true }).error, /group|paren/i);
  assert.ok(compileQuery({ q: 'x*', regex: true }).error);     // matches the empty string
  assert.equal(compileQuery({ q: '' }).error, '');
});

test('searchText: line, column, offset, length and the whole line', () => {
  const t = 'local a = 1\nlocal b = a + a\n-- end';
  const m = searchText(t, compileQuery({ q: 'a', word: true }).re);
  assert.deepEqual(m.map((x) => [x.line, x.col]), [[1, 6], [2, 10], [2, 14]]);
  assert.equal(m[1].text, 'local b = a + a');
  assert.equal(t.slice(m[2].at, m[2].at + m[2].len), 'a');
  assert.equal(searchText(t, compileQuery({ q: 'END' }).re)[0].text, '-- end');
  assert.equal(searchText('aaaa', compileQuery({ q: 'a' }).re, 3).length, 3);
});

test('searchable: skips bytecode, binaries, backups, archives, pictures, and cache/ ota/ var/', () => {
  assert.equal(searchable('lua/run.lua'), true);
  assert.equal(searchable('lua/run.lua.gz'), true);
  assert.equal(searchable('js/app.js.gz'), true);
  assert.equal(searchable('cache/lua/run.luac'), false);
  assert.equal(searchable('var/wg_epoch.bin'), false);
  assert.equal(searchable('ota/x.json'), false);
  assert.equal(searchable('ota/x.json', 'ota'), true);         // asked for by name
  assert.equal(searchable('js/app.js.gz.bak'), false);
  assert.equal(searchable('www/dot.png'), false);
  assert.equal(searchable('www/logo.svg.gz'), true);           // SVG is text
});

test('searchFiles: walks, skips, reports per file, stops when cancelled', async () => {
  const fs = {
    '': [{ name: 'lua', dir: true }, { name: 'cache', dir: true }, { name: 'cfg', dir: true }],
    lua: [{ name: 'a.lua', size: 1 }, { name: 'b.lua', size: 1 }, { name: 'sub', dir: true }],
    'lua/sub': [{ name: 'c.lua.gz', size: 1 }],
    cache: [{ name: 'x.luac', size: 1 }],
    cfg: [{ name: 'bin.json', size: 1 }],
  };
  const texts = { 'lua/a.lua': 'print("needle")\n', 'lua/b.lua': 'nothing', 'lua/sub/c.lua.gz': 'needle needle', 'cfg/bin.json': 'needle\0' };
  const list = async (d) => (fs[d] ? { ok: true, files: fs[d] } : { ok: false, error: 'nope' });
  const reads = [];
  const read = async (p) => { reads.push(p); return { ok: true, text: texts[p] }; };
  const found = [];
  const r = await searchFiles({ list, read, re: compileQuery({ q: 'needle' }).re, onFile: (p, m) => found.push([p, m.length]) });
  assert.deepEqual(found, [['lua/a.lua', 1], ['lua/sub/c.lua.gz', 2]]);   // binary cfg skipped
  assert.deepEqual(r, { files: 2, searched: 4, matches: 3, truncated: false, errors: [] });
  assert.ok(!reads.includes('cache/x.luac'));
  // a folder of its own
  const only = [];
  await searchFiles({ list, read, root: 'lua/sub', re: compileQuery({ q: 'needle' }).re, onFile: (p) => only.push(p) });
  assert.deepEqual(only, ['lua/sub/c.lua.gz']);
  // cancelled after the first file
  let n = 0;
  const c = await searchFiles({ list, read: async (p) => { n++; return read(p); }, re: compileQuery({ q: 'needle' }).re, onFile: () => {}, isCancelled: () => n >= 1 });
  assert.equal(c.searched, 1);
});
