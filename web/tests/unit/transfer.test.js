import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  volume, storedSize, reclaimSize, transferNeed, fits, parseLfsFree, sdFreeBytes,
  measure, planTransfer, runTransfer, BLOCK, lfsFreeChunk,
} from '../../js/editor/transfer.js';

test('volume is the card only for sd and sd/...', () => {
  assert.equal(volume('sd'), 'sd');
  assert.equal(volume('sd/projects/blink/build/blink.m33.pex'), 'sd');
  assert.equal(volume('lua/a.lua'), 'lfs');
  assert.equal(volume('pex/anker.m33.pex'), 'lfs');
  assert.equal(volume(''), 'lfs');
  assert.equal(volume('sdcard/a'), 'lfs');
});

test('stored size rounds a new file up, and an inline file returns no block', () => {
  assert.equal(storedSize({ size: 0, alloc: 0 }, 'lfs'), 0);
  assert.equal(storedSize({ size: 10, alloc: 0 }, 'lfs'), BLOCK);
  assert.equal(storedSize({ size: 5000, alloc: 8192 }, 'lfs'), 8192);
  assert.equal(storedSize({ size: 1 }, 'sd'), BLOCK);
  assert.equal(storedSize({ size: 0 }, 'sd'), 0);
  assert.equal(storedSize({ size: 4097 }, 'sd'), BLOCK * 2);
  assert.equal(reclaimSize({ size: 10, alloc: 0 }, 'lfs'), 0);
  assert.equal(reclaimSize({ size: 5000, alloc: 8192 }, 'lfs'), 8192);
  assert.equal(reclaimSize({ size: 100 }, 'sd'), BLOCK);
});

test('a same-volume move needs nothing; a copy and a cross-volume move do', () => {
  const files = [{ size: 100, alloc: 0 }];
  assert.equal(transferNeed({ move: true, srcVol: 'lfs', destVol: 'lfs', files, dirCount: 1 }), 0);
  assert.equal(transferNeed({ move: true, srcVol: 'sd', destVol: 'sd', files, dirCount: 2 }), 0);
  assert.equal(transferNeed({ move: false, srcVol: 'lfs', destVol: 'lfs', files, dirCount: 0 }), BLOCK);
  const cross = transferNeed({ move: true, srcVol: 'sd', destVol: 'lfs', files, dirCount: 1 });
  assert.equal(cross, BLOCK + 2 * BLOCK);
  const card = transferNeed({ move: false, srcVol: 'lfs', destVol: 'sd', files: [{ size: 4097 }], dirCount: 1 });
  assert.equal(card, BLOCK + 2 * BLOCK);
  assert.equal(transferNeed({
    move: false, srcVol: 'sd', destVol: 'lfs', files: [{ size: 5000, alloc: 8192 }], dirCount: 0, reclaim: 8192,
  }), 0);
});

test('fits refuses a missing reading and a short volume, and ignores a zero need', () => {
  assert.deepEqual(fits(null, 0), { ok: true });
  assert.equal(fits(null, 10).code, 'free');
  assert.equal(fits(99, 100).code, 'space');
  assert.equal(fits(100, 100).ok, true);
});

test('LittleFS and SD free readings', () => {
  assert.equal(parseLfsFree('8192 1'), 8192);
  assert.equal(parseLfsFree('0 0'), null);
  assert.equal(parseLfsFree('nope'), null);
  assert.equal(sdFreeBytes({ ok: true, state: 'ok', free: 4096 }), 4096);
  assert.equal(sdFreeBytes({ ok: true, state: 'none', free: 4096 }), null);
  assert.equal(sdFreeBytes({ ok: false, state: 'ok', free: 4096 }), null);
  assert.ok(lfsFreeChunk().includes('file.media()'));
  assert.ok(!lfsFreeChunk().includes('fs.media'));
  assert.ok(lfsFreeChunk().length < 1024);
});

test('measure walks a folder deepest first and a file is one entry', async () => {
  const tree = {
    lua: [{ name: 'a.lua', size: 10, alloc: 0 }, { name: 'p', dir: true }],
    'lua/p': [{ name: 'b.lua', size: 5000, alloc: 8192 }],
  };
  const folder = await measure(async (p) => ({ ok: true, block: 4096, files: tree[p] }), 'lua', { dir: true });
  assert.deepEqual(folder.files.map((f) => f.path), ['lua/a.lua', 'lua/p/b.lua']);
  assert.deepEqual(folder.dirs, ['lua/p', 'lua']);
  const file = await measure(async () => { throw new Error('no list'); }, 'lua/a.lua', { dir: false, size: 10, alloc: 0 });
  assert.deepEqual(file, { files: [{ path: 'lua/a.lua', size: 10, alloc: 0 }], dirs: [], block: 4096 });
});

function fileMeasured(path, size, alloc = 0) {
  return { files: [{ path, size, alloc }], dirs: [], block: 4096 };
}

test('plan: same-volume move is a rename and does not read free space', () => {
  const plan = planTransfer({
    src: 'lua/a.lua', dest: 'lua/b.lua', move: true, dir: false,
    measured: fileMeasured('lua/a.lua', 100, 0), free: null,
  });
  assert.equal(plan.ok, true);
  assert.equal(plan.action, 'rename');
  assert.equal(plan.need, 0);
  assert.equal(plan.replace, false);
});

test('plan: a move onto the other volume is a copy plus a delete, after a free reading', () => {
  const asked = planTransfer({
    src: 'sd/projects/blink/build/pico2_w/blink.m33.pex',
    dest: 'pex/blink.m33.pex',
    move: true,
    dir: false,
    measured: fileMeasured('sd/projects/blink/build/pico2_w/blink.m33.pex', 200),
    free: null,
  });
  assert.equal(asked.code, 'free');
  assert.equal(asked.destVol, 'lfs');
  const plan = planTransfer({
    src: 'sd/projects/blink/build/pico2_w/blink.m33.pex',
    dest: 'pex/blink.m33.pex',
    move: true,
    dir: false,
    measured: fileMeasured('sd/projects/blink/build/pico2_w/blink.m33.pex', 200),
    free: BLOCK,
  });
  assert.equal(plan.ok, true);
  assert.equal(plan.action, 'copy');
  assert.deepEqual(plan.remove, ['sd/projects/blink/build/pico2_w/blink.m33.pex']);
  assert.equal(plan.files[0].to, 'pex/blink.m33.pex');
});

test('plan: not enough space, a file over the LittleFS limit, and no merge', () => {
  const short = planTransfer({
    src: 'sd/big.bin', dest: 'var/big.bin', move: true, dir: false,
    measured: fileMeasured('sd/big.bin', 8000), free: 100,
  });
  assert.equal(short.code, 'space');
  assert.equal(short.need, 8192);
  const over = planTransfer({
    src: 'sd/huge.bin', dest: 'var/huge.bin', move: false, dir: false,
    measured: fileMeasured('sd/huge.bin', 1024 * 1024 + 1), free: 9e9,
  });
  assert.equal(over.code, 'limit');
  const merge = planTransfer({
    src: 'lua', dest: 'sd/lua', move: true, dir: true,
    measured: { files: [], dirs: ['lua'], block: 4096 },
    destHit: { dir: true, size: 0, alloc: 0 },
    free: 9e9,
  });
  assert.equal(merge.code, 'merge');
  const into = planTransfer({
    src: 'lua', dest: 'lua/nested', move: true, dir: true,
    measured: { files: [], dirs: ['lua'], block: 4096 }, free: null,
  });
  assert.equal(into.code, 'inside');
  const card = planTransfer({
    src: 'sd', dest: 'lua/sd', move: true, dir: true,
    measured: { files: [], dirs: ['sd'], block: 4096 }, free: null,
  });
  assert.equal(card.code, 'card');
});

test('plan: replacing a file reclaims its blocks, and a same-volume move will not replace', () => {
  const copy = planTransfer({
    src: 'lua/a.lua', dest: 'lua/b.lua', move: false, dir: false,
    measured: fileMeasured('lua/a.lua', 100, 4096),
    destHit: { dir: false, size: 100, alloc: 4096 },
    free: 0,
  });
  assert.equal(copy.ok, true, copy.error);
  assert.equal(copy.replace, true);
  assert.equal(copy.need, 0);
  const clash = planTransfer({
    src: 'lua/a.lua', dest: 'lua/b.lua', move: true, dir: false,
    measured: fileMeasured('lua/a.lua', 100, 4096),
    destHit: { dir: false, size: 100, alloc: 4096 },
    free: null,
  });
  assert.equal(clash.code, 'exists');
});

test('plan: a folder copy maps children and counts directory metadata', () => {
  const asked = planTransfer({
    src: 'lua', dest: 'sd/backup', move: false, dir: true,
    measured: {
      files: [{ path: 'lua/a.lua', size: 10, alloc: 0 }, { path: 'lua/p/b.lua', size: 5000, alloc: 8192 }],
      dirs: ['lua/p', 'lua'],
      block: 4096,
    },
    free: null,
  });
  assert.equal(asked.code, 'free');
  const plan = planTransfer({
    src: 'lua', dest: 'sd/backup', move: false, dir: true,
    measured: {
      files: [{ path: 'lua/a.lua', size: 10, alloc: 0 }, { path: 'lua/p/b.lua', size: 5000, alloc: 8192 }],
      dirs: ['lua/p', 'lua'],
      block: 4096,
    },
    free: BLOCK * 5,
  });
  assert.equal(plan.ok, true, plan.error);
  assert.deepEqual(plan.mkdirs, ['sd/backup', 'sd/backup/p']);
  assert.deepEqual(plan.files.map((f) => f.to), ['sd/backup/a.lua', 'sd/backup/p/b.lua']);
  // Two directories, a 10-byte file (one cluster), and a 5000-byte file (two).
  assert.equal(plan.need, BLOCK * 5);
});

test('run: a rename does not read or write, and a failed copy leaves the source', async () => {
  const renamed = [];
  const plan = planTransfer({
    src: 'lua/a.lua', dest: 'cfg/a.lua', move: true, dir: false,
    measured: fileMeasured('lua/a.lua', 4, 0), free: null,
  });
  const moved = await runTransfer(plan, {
    rename: async (from, to) => { renamed.push([from, to]); return { ok: true }; },
    read: async () => { throw new Error('read'); },
    write: async () => { throw new Error('write'); },
    mkdir: async () => ({ ok: true }),
    remove: async () => { throw new Error('remove'); },
  });
  assert.equal(moved.ok, true);
  assert.deepEqual(renamed, [['lua/a.lua', 'cfg/a.lua']]);

  const copy = planTransfer({
    src: 'sd/a.bin', dest: 'var/a.bin', move: true, dir: false,
    measured: fileMeasured('sd/a.bin', 4), free: BLOCK,
  });
  const removed = [];
  const stopped = await runTransfer(copy, {
    read: async () => ({ ok: true, bytes: new Uint8Array([1, 2, 3, 4]) }),
    write: async () => ({ ok: false, error: 'no space' }),
    mkdir: async () => ({ ok: true }),
    remove: async (p) => { removed.push(p); return { ok: true }; },
    rename: async () => ({ ok: true }),
  });
  assert.equal(stopped.ok, false);
  assert.equal(stopped.partial, true);
  assert.equal(stopped.copied, undefined);
  assert.deepEqual(removed, []);
  assert.match(stopped.error, /var\/a\.bin/);
});

test('run: the source is removed only after the copy, deepest first', async () => {
  const plan = planTransfer({
    src: 'lua', dest: 'sd/backup', move: true, dir: true,
    measured: {
      files: [{ path: 'lua/a.lua', size: 2, alloc: 0 }],
      dirs: ['lua'],
      block: 4096,
    },
    free: BLOCK * 3,
  });
  const order = [];
  const done = await runTransfer(plan, {
    mkdir: async (p) => { order.push(`mkdir ${p}`); return { ok: true }; },
    read: async (p) => { order.push(`read ${p}`); return { ok: true, bytes: new Uint8Array([1, 2]) }; },
    write: async (p) => { order.push(`write ${p}`); return { ok: true }; },
    remove: async (p) => { order.push(`remove ${p}`); return { ok: true }; },
    rename: async () => { throw new Error('rename'); },
  });
  assert.equal(done.ok, true);
  assert.deepEqual(order, [
    'mkdir sd/backup',
    'read lua/a.lua',
    'write sd/backup/a.lua',
    'remove lua/a.lua',
    'remove lua',
  ]);
});
