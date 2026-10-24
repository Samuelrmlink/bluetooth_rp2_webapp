import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fileMenuItems } from '../../js/editor/filemenu.js';

const ids = (items) => items.map((i) => i.id);

test('a folder: new file, upload, refresh, rename', () => {
  assert.deepEqual(ids(fileMenuItems({ dir: true, path: 'lua' })), ['new', 'mkdir', 'upload', 'untar', 'tar', 'size', 'refresh', 'rename', 'delete']);
});

test('a Pexfile lists its build actions before Open', () => {
  const builds = [
    { id: 'pex-build', target: 'pico2_w', label: 'Build pico2_w' },
    { id: 'pex-brun', target: 'pico2_w', label: 'Build and run pico2_w' },
    { id: 'pex-build-all', label: 'Build all' },
  ];
  const m = fileMenuItems({ dir: false, path: 'sd/projects/blink/Pexfile', name: 'Pexfile' }, { builds });
  assert.deepEqual(ids(m).slice(0, 4), ['pex-build', 'pex-brun', 'pex-build-all', 'open']);
  assert.equal(m[0].label, 'Build pico2_w');
});

test('a plain file: open, download, rename, delete (last, marked)', () => {
  const m = fileMenuItems({ dir: false, path: 'cfg/bt.json' });
  assert.deepEqual(ids(m), ['open', 'download', 'rename', 'delete']);
  assert.equal(m.at(-1).danger, true);
});

test('a script gets start / stop / commit / revert; the open file no Open', () => {
  const m = fileMenuItems({ dir: false, path: 'lua/anker.lua' }, { isOpen: true, script: 'anker' });
  assert.deepEqual(ids(m), ['start', 'stop', 'commit', 'revert', 'download', 'compress', 'rename', 'delete']);
  assert.equal(m[0].label, 'Start anker');
});

test('a PEX app gets start / stop / install, and uninstall only when it is in the partition', () => {
  const m = fileMenuItems({ dir: false, path: 'pex/anker.m33.pex' }, { pex: 'anker' });
  assert.deepEqual(ids(m), ['open', 'pex-start', 'pex-stop', 'pex-install', 'pex-partition', 'download', 'rename', 'delete']);
  assert.equal(m.find((i) => i.id === 'pex-start').label, 'Start anker');
  assert.equal(m.find((i) => i.id === 'pex-stop').label, 'Stop anker');
  const installed = fileMenuItems({ dir: false, path: 'pex/anker.m33.pex' }, { pex: 'anker', installed: true });
  assert.ok(ids(installed).includes('pex-uninstall'));
  assert.equal(installed.find((i) => i.id === 'pex-uninstall').label, 'Uninstall anker');
});

import { bytes, fileInfo, dirInfo, folderSize, sizeInfo } from '../../js/editor/filemenu.js';

test('a folder: Delete when empty, Delete with everything in it otherwise; Calculate size keeps the menu open', () => {
  assert.equal(fileMenuItems({ dir: true }, { empty: false }).at(-1).label, 'Delete folder and everything in it…');
  const m = fileMenuItems({ dir: true }, { empty: true });
  assert.equal(m.at(-1).id, 'delete');
  assert.equal(m.find((i) => i.id === 'size').keep, true);
});

test('sizes and a file\'s flash use, inline included', () => {
  assert.equal(bytes(812), '812 B');
  assert.equal(bytes(4096), '4.0 KB');
  assert.equal(fileInfo({ size: 30700, alloc: 32768 }), '30.0 KB · 32.0 KB on flash');
  assert.equal(fileInfo({ size: 120, alloc: 0 }), '120 B · stored inline in its folder (no blocks of its own)');
  assert.equal(fileInfo({ size: 120 }), '120 B');                       // older firmware: no alloc
});

test('a folder\'s line: what is directly in it', () => {
  assert.equal(dirInfo(null), 'counting…');
  assert.equal(dirInfo([]), 'empty');
  assert.equal(dirInfo([{ dir: false }, { dir: false }, { dir: true }]), '2 files, 1 folder');
});

test('Calculate size walks every subfolder and counts their metadata', async () => {
  const tree = {
    lua: [{ name: 'a.lua', size: 5000, alloc: 8192 }, { name: 'tiny', size: 10, alloc: 0 }, { name: 'p', dir: true }],
    'lua/p': [{ name: 'b.lua', size: 100, alloc: 0 }],
  };
  const t = await folderSize(async (p) => ({ ok: true, block: 4096, files: tree[p] }), 'lua');
  assert.deepEqual([t.files, t.dirs, t.bytes, t.alloc], [3, 1, 5110, 8192 + 2 * 2 * 4096]);
  assert.equal(sizeInfo(t), "5.0 KB in 3 files and 1 subfolder · 24.0 KB on flash, counting each folder's 8.0 KB of metadata");
});

test('compressed files: Store uncompressed; plain ones Store compressed; never cfg/ or a bitmap', () => {
  assert.ok(ids(fileMenuItems({ dir: false, path: 'lua/run.lua.gz' })).includes('decompress'));
  assert.ok(ids(fileMenuItems({ dir: false, path: 'js/app.js.gz' })).includes('decompress'));
  assert.ok(ids(fileMenuItems({ dir: false, path: 'lua/run.lua' })).includes('compress'));
  const cfg = ids(fileMenuItems({ dir: false, path: 'cfg/wifi.json' }));
  assert.ok(!cfg.includes('compress') && !cfg.includes('decompress'));
  const jpg = ids(fileMenuItems({ dir: false, path: 'ss/a.jpg' }));
  assert.ok(!jpg.includes('compress') && !jpg.includes('decompress'));
  assert.ok(ids(fileMenuItems({ dir: false, path: 'css/app.css' })).includes('compress'));
  assert.ok(!ids(fileMenuItems({ dir: false, path: 'notes.txt' })).includes('compress'));
  assert.ok(!ids(fileMenuItems({ dir: false, path: 'src/main.c' })).includes('compress'));
  assert.ok(!ids(fileMenuItems({ dir: false, path: 'lua/profiles/x.lua' })).includes('compress'));
  assert.ok(ids(fileMenuItems({ dir: false, path: 'pex/anker.m33.pex.gz' })).includes('decompress'));
  assert.ok(ids(fileMenuItems({ dir: false, path: 'notes.txt.gz' })).includes('decompress'));
  assert.ok(!ids(fileMenuItems({ dir: false, path: 'ss/a.jpg.gz' })).includes('decompress'));
});

test('Restore .bak only when there is one', () => {
  assert.ok(!ids(fileMenuItems({ dir: false, path: 'js/app.js.gz' })).includes('restore-bak'));
  assert.ok(ids(fileMenuItems({ dir: false, path: 'js/app.js.gz' }, { hasBak: true })).includes('restore-bak'));
});

test('a compressed file says so on its info line', () => {
  assert.match(fileInfo({ path: 'lua/x.lua.gz', size: 900, alloc: 4096 }), /^compressed \(gzip\) · 900 B · 4\.0 KB on flash$/);
});

test('the SD card: its own folder has no rename/delete and links to its settings; tar is flash-only', () => {
  const card = ids(fileMenuItems({ dir: true, path: 'sd' }, { card: true }));
  assert.ok(card.includes('sd-settings') && card.includes('mkdir'));
  assert.ok(!card.includes('delete') && !card.includes('rename') && !card.includes('tar'));
  const sub = ids(fileMenuItems({ dir: true, path: 'sd/logs' }));
  assert.ok(sub.includes('delete') && sub.includes('rename') && !sub.includes('tar'));
  assert.ok(!ids(fileMenuItems({ dir: false, path: 'sd/notes.txt' })).includes('compress'));
  assert.match(sizeInfo({ files: 2, dirs: 1, bytes: 2048, alloc: 0, block: 0 }, 'sd/logs'), /on the SD card$/);
});
