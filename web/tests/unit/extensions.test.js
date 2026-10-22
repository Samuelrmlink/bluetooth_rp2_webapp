import { test } from 'node:test';
import assert from 'node:assert/strict';

// fs.js reads location when it loads: a page's, here.
globalThis.location = new URL('http://gw.local/');
const { extensionNames, loadExtensions, EXT_DIR } = await import('../../js/extensions.js');

test('extensions: folders only, safe names, in order', () => {
  assert.deepEqual(extensionNames([
    { name: 'my-window', dir: true }, { name: 'readme.txt', size: 3 }, { name: 'a_b', dir: true },
    { name: '../x', dir: true }, { name: 'sp ace', dir: true }, null,
  ]), ['a_b', 'my-window']);
  assert.deepEqual(extensionNames(undefined), []);
});

test('extensions: each index.js registers; a broken one is reported and skipped', async () => {
  const logs = [], registered = [];
  const app = { log: (src, text) => logs.push([src, text]) };
  const mods = {
    'http://gw.local/js/ext/good/index.js': { register: (a) => registered.push(a === app) },
    'http://gw.local/js/ext/noreg/index.js': {},
    'http://gw.local/js/ext/throws/index.js': { register: () => { throw new Error('boom'); } },
  };
  let listed;
  const loaded = await loadExtensions(app, {
    listDir: async (d) => { listed = d; return { ok: true, files: ['throws', 'noreg', 'good', 'missing'].map((name) => ({ name, dir: true })) }; },
    importModule: async (u) => { if (!mods[u]) throw new Error(`404 ${u}`); return mods[u]; },
  });
  assert.equal(listed, EXT_DIR);
  assert.deepEqual(loaded, ['good']);
  assert.deepEqual(registered, [true]);
  const errs = logs.filter(([s]) => s === '!').map(([, t]) => t);
  assert.equal(errs.length, 3);
  assert.match(errs.find((t) => t.startsWith('extension noreg')), /no register/);
  assert.match(errs.find((t) => t.startsWith('extension throws')), /boom/);
  assert.match(errs.find((t) => t.startsWith('extension missing')), /404/);
});

test('extensions: no js/ext/ or no device means nothing, quietly', async () => {
  const logs = [];
  const loaded = await loadExtensions({ log: (...a) => logs.push(a) }, {
    listDir: async () => ({ ok: false, error: 'not found', status: 404 }),
    importModule: async () => { throw new Error('must not import'); },
  });
  assert.deepEqual(loaded, []);
  assert.deepEqual(logs, []);
});
