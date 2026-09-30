import { test } from 'node:test';
import assert from 'node:assert/strict';
import { unitPath, arenaMarks, shownArena, scriptNameOf, shadowedBy } from '../../js/editor/status.js';

test('unitPath mirrors the firmware: scripts, modules, tiers', () => {
  assert.equal(unitPath({ kind: 'script', name: 'anker', tier: 'committed' }), 'lua/anker.lua');
  assert.equal(unitPath({ kind: 'script', name: 'anker', tier: 'staged' }), 'staged/anker.lua');
  assert.equal(unitPath({ kind: 'module', name: 'profiles.anker.models.A1761', tier: 'committed' }),
    'lua/profiles/anker/models/A1761.lua');
  assert.equal(unitPath({ kind: 'script', name: 'x', tier: 'draft' }), 'lua/x.lua');
});

test('arenaMarks: matching hash is loaded; changed, draft or missing file is stale', () => {
  const m = arenaMarks([
    { kind: 'script', name: 'anker', tier: 'committed', hash: '1ab3834f', now: '1ab3834f' },
    { kind: 'module', name: 'profiles.anker.run', tier: 'committed', hash: '81c681aa', now: '00000000' },
    { kind: 'script', name: 'probe', tier: 'draft', hash: 'aaaaaaaa', now: null },
    { kind: 'script', name: 'gone', tier: 'committed', hash: 'bbbbbbbb', now: null },
  ]);
  assert.equal(m.get('lua/anker.lua').state, 'loaded');
  assert.equal(m.get('lua/profiles/anker/run.lua').state, 'stale');
  assert.equal(m.get('lua/probe.lua').state, 'stale');
  assert.equal(m.get('lua/gone.lua').state, 'stale');
  assert.equal(m.get('lua/other.lua'), undefined);
});

test('shownArena: a draft identical to the saved file counts as loaded (F5, then Ctrl+S)', () => {
  const draft = { state: 'stale', tier: 'draft', hash: 'aaaaaaaa' };
  assert.equal(shownArena(draft, 'aaaaaaaa'), 'loaded', 'running exactly what is on flash');
  assert.equal(shownArena(draft, 'bbbbbbbb'), 'stale', 'the file was saved differently since');
  assert.equal(shownArena(draft, undefined), 'stale', 'file not open: nothing to compare with');
  assert.equal(shownArena({ state: 'stale', tier: 'committed', hash: 'x' }, 'x'), 'stale',
    'a committed unit is judged by the firmware, which read the file itself');
  assert.equal(shownArena({ state: 'loaded', tier: 'committed', hash: 'x' }, 'y'), 'loaded');
  assert.equal(shownArena(undefined, 'x'), undefined);
});

test('scriptNameOf: top-level scripts only', () => {
  assert.equal(scriptNameOf('lua/anker.lua'), 'anker');
  assert.equal(scriptNameOf('staged/anker.lua'), 'anker');
  assert.equal(scriptNameOf('lua/profiles/anker/run.lua'), null);
  assert.equal(scriptNameOf('cfg/wifi.json'), null);
});

test('shadowedBy: a draft or a staged copy beats the committed file', () => {
  const list = { scripts: [{ name: 'a', draft: true }], files: [{ name: 'b', tier: 'staged' }, { name: 'c', tier: 'committed' }] };
  assert.equal(shadowedBy('lua/a.lua', list), 'draft');
  assert.equal(shadowedBy('lua/b.lua', list), 'staged');
  assert.equal(shadowedBy('lua/c.lua', list), null);
  assert.equal(shadowedBy('staged/b.lua', list), null, 'saving the staged copy itself is not shadowed');
  assert.equal(shadowedBy('cfg/x.json', list), null);
});
