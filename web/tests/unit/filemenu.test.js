import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fileMenuItems } from '../../js/editor/filemenu.js';

const ids = (items) => items.map((i) => i.id);

test('a folder: new file, upload, refresh, rename', () => {
  assert.deepEqual(ids(fileMenuItems({ dir: true, path: 'lua' })), ['new', 'upload', 'refresh', 'rename']);
});

test('a plain file: open, download, rename, delete (last, marked)', () => {
  const m = fileMenuItems({ dir: false, path: 'cfg/bt.json' });
  assert.deepEqual(ids(m), ['open', 'download', 'rename', 'delete']);
  assert.equal(m.at(-1).danger, true);
});

test('a script gets start / stop / commit / revert; the open file no Open', () => {
  const m = fileMenuItems({ dir: false, path: 'lua/anker.lua' }, { isOpen: true, script: 'anker' });
  assert.deepEqual(ids(m), ['start', 'stop', 'commit', 'revert', 'download', 'rename', 'delete']);
  assert.equal(m[0].label, 'Start anker');
});
