import { test } from 'node:test';
import assert from 'node:assert/strict';
import { planDrop, dropDir, moveDest, treeOf, deleteTree, isVital } from '../../js/editor/dnd.js';

const f = (size = 3) => ({ size });

test('planDrop: files land in the target, a lone .tar is extracted, a .tar in a folder is a file', () => {
  const p = planDrop([
    { path: 'a.lua', file: f() },
    { path: 'backup.tar', file: f() },
    { path: 'site', dir: true },
    { path: 'site/old.tar', file: f() },
    { path: 'site/img', dir: true },
    { path: 'site/empty', dir: true },
    { path: 'site/img/x.png', file: f() },
  ], 'www', new Set(['www/a.lua']));
  assert.equal(p.tars.length, 1);
  assert.deepEqual(p.uploads.map((u) => u.dest), ['www/a.lua', 'www/site/old.tar', 'www/site/img/x.png']);
  assert.deepEqual(p.overwrites, ['www/a.lua']);
  assert.deepEqual(p.dirs, ['www/site/empty']);                  // the others are made by their files
});

test('planDrop: bad names, cache/ and oversize files are skipped with a reason', () => {
  const p = planDrop([
    { path: 'x'.repeat(130), file: f() },
    { path: 'big.bin', file: f(200 * 1024) },
    { path: 'cache', dir: true },
    { path: 'cache/y.luac', file: f() },
  ], '');
  assert.deepEqual(p.skipped.map((s) => s.why.split(' ')[0]), ['path', 'over', 'cache/', 'cache/']);
  assert.equal(p.uploads.length, 0);
});

test('dropDir: a folder row is the folder, a file row its folder, nothing the root', () => {
  assert.equal(dropDir({ dir: true, path: 'lua/profiles' }), 'lua/profiles');
  assert.equal(dropDir({ dir: false, path: 'lua/run.lua' }), 'lua');
  assert.equal(dropDir({ dir: false, path: 'index.html.gz' }), '');
  assert.equal(dropDir(null), '');
});

test('moveDest: into a folder; never into itself, its own subfolder, or where it already is', () => {
  assert.equal(moveDest('lua/a.lua', 'lua/profiles'), 'lua/profiles/a.lua');
  assert.equal(moveDest('lua/a.lua', ''), 'a.lua');
  assert.equal(moveDest('lua/a.lua', 'lua'), null);
  assert.equal(moveDest('lua', 'lua/profiles'), null);
  assert.equal(moveDest('lua', 'lua'), null);
  assert.equal(moveDest('www', 'lua'), 'lua/www');
});

// A little filesystem: paths -> true (files) and a set of folders.
function fakeFs(files, dirs) {
  const fs = new Set(files), ds = new Set(dirs);
  const list = async (p) => {
    if (!ds.has(p)) return { ok: false, error: 'not found' };
    const names = new Map();
    for (const x of [...fs, ...ds]) {
      if (!x.startsWith(p + '/')) continue;
      const rest = x.slice(p.length + 1);
      const name = rest.split('/')[0];
      names.set(name, rest.includes('/') || ds.has(`${p}/${name}`));
    }
    return { ok: true, files: [...names].map(([name, dir]) => (dir ? { name, dir: true } : { name, size: 1 })) };
  };
  const removed = [];
  const remove = async (p) => {
    if (fs.has(p)) { fs.delete(p); removed.push(p); return { ok: true }; }
    if (ds.has(p)) {
      if ([...fs, ...ds].some((x) => x.startsWith(p + '/'))) return { ok: false, status: 409, error: 'folder is not empty' };
      ds.delete(p); removed.push(p); return { ok: true };
    }
    return { ok: false, status: 404, error: 'not found' };
  };
  return { list, remove, removed, fs, ds };
}

test('deleteTree: everything under the folder, files first, folders deepest first', async () => {
  const x = fakeFs(['w/a', 'w/s/b', 'w/s/t/c', 'keep/k'], ['w', 'w/s', 'w/s/t', 'w/e', 'keep']);
  const seen = [];
  const r = await deleteTree(x.list, x.remove, 'w', (d, n) => seen.push(`${d}/${n}`));
  assert.deepEqual(r, { files: 3, dirs: 4 });
  assert.deepEqual([...x.fs], ['keep/k']);
  assert.deepEqual([...x.ds], ['keep']);
  assert.equal(seen.at(-1), '7/7');
  // every folder went after everything in it
  for (const d of ['w/s/t', 'w/s', 'w']) {
    const at = x.removed.indexOf(d);
    assert.ok(x.removed.every((p, i) => !p.startsWith(d + '/') || i < at), d);
  }
});

test('deleteTree: a refusal stops it and says how far it got', async () => {
  const x = fakeFs(['w/a', 'w/b'], ['w']);
  const remove = async (p) => (p === 'w/b' ? { ok: false, status: 500, error: 'flash error' } : x.remove(p));
  await assert.rejects(deleteTree(x.list, remove, 'w'), /w\/b: flash error \(1 of 3 removed\)/);
});

test('treeOf lists deepest first; isVital names the folders that ask for the name typed', async () => {
  const x = fakeFs(['a/b/c'], ['a', 'a/b']);
  assert.deepEqual(await treeOf(x.list, 'a'), { files: [{ path: 'a/b/c', size: 1 }], dirs: ['a/b', 'a'] });
  assert.equal(isVital('cfg'), true);
  assert.equal(isVital('lua'), true);
  assert.equal(isVital('lua/profiles'), false);
  assert.equal(isVital('www'), false);
});
