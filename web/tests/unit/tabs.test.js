import { test } from 'node:test';
import assert from 'node:assert/strict';
import { TabState } from '../../js/editor/tabs.js';

const rows = (t) => t.groups.map((g) => g.tabs.map((p, i) => (i === g.active ? `[${p}]` : p)).join(' '));

test('open adds a tab after the showing one; reopening just shows it', () => {
  const t = new TabState();
  t.open('a'); t.open('b'); t.open('c');
  assert.deepEqual(rows(t), ['a b [c]']);
  t.open('a');
  assert.deepEqual(rows(t), ['[a] b c']);
  t.open('d');
  assert.deepEqual(rows(t), ['a [d] b c']);
});

test('close shows the right neighbour, else the left; the last tab leaves nothing', () => {
  const t = new TabState();
  for (const p of ['a', 'b', 'c']) t.open(p);
  t.goto(2);
  t.close('b');
  assert.deepEqual(rows(t), ['a [c]']);
  t.close('c');
  assert.deepEqual(rows(t), ['[a]']);
  t.close('a');
  assert.equal(t.activePath(), null);
  assert.equal(t.close('zz'), false);
});

test('closing a tab left of the showing one keeps the same file showing', () => {
  const t = new TabState();
  for (const p of ['a', 'b', 'c']) t.open(p);
  t.close('a');
  assert.deepEqual(rows(t), ['b [c]']);
});

test('step wraps; goto is 1-based and ignores out of range; move swaps in place', () => {
  const t = new TabState();
  for (const p of ['a', 'b', 'c']) t.open(p);
  assert.equal(t.step(1), 'a');
  assert.equal(t.step(-1), 'c');
  assert.equal(t.goto(2), 'b');
  assert.equal(t.goto(9), 'b');
  assert.equal(t.move(1), true);
  assert.deepEqual(rows(t), ['a c [b]']);
  assert.equal(t.move(1), false);
});

test('split: the same file in a second half, focused; unsplit merges the tabs back', () => {
  const t = new TabState();
  t.open('a'); t.open('b');
  t.splitOpen();
  assert.equal(t.split, true);
  assert.equal(t.focus, 1);
  assert.deepEqual(rows(t), ['a [b]', '[b]']);
  t.open('c');
  assert.deepEqual(rows(t), ['a [b]', 'b [c]']);
  assert.equal(t.focusOther(), 0);
  t.unsplit();
  assert.deepEqual(rows(t), ['a [b] c']);
  assert.equal(t.split, false);
});

test('a half whose last tab closes goes away', () => {
  const t = new TabState();
  t.open('a');
  t.splitOpen('b');
  assert.deepEqual(rows(t), ['[a]', '[b]']);
  t.close('b');
  assert.equal(t.split, false);
  assert.deepEqual(rows(t), ['[a]']);
  assert.equal(t.focus, 0);
});

test('moveToOther: splits off a tab, and moving the last one back unsplits', () => {
  const t = new TabState();
  t.open('a'); t.open('b');
  assert.equal(t.moveToOther(), true);
  assert.deepEqual(rows(t), ['[a]', '[b]']);
  assert.equal(t.focus, 1);
  assert.equal(t.moveToOther(), true);
  assert.equal(t.split, false);
  assert.deepEqual(rows(t), ['a [b]']);
  // a single tab cannot be split off from nothing
  const u = new TabState();
  u.open('x');
  assert.equal(u.moveToOther(), false);
});

test('rename and delete reach every half; a rename onto an open name keeps one tab', () => {
  const t = new TabState();
  t.open('a.lua'); t.open('b');
  t.splitOpen('a.lua');
  t.rename('a.lua', 'a.lua.gz');
  assert.deepEqual(rows(t), ['a.lua.gz [b]', '[a.lua.gz]']);
  t.focusOther();
  t.open('c');
  t.rename('c', 'b');
  assert.deepEqual(rows(t), ['a.lua.gz [b]', '[a.lua.gz]']);
  t.closeEverywhere('a.lua.gz');
  assert.deepEqual(rows(t), ['[b]']);
  assert.ok(t.isOpen('b') && t.showing('b') && !t.isOpen('a.lua.gz'));
});

test('flip turns side by side into stacked and back', () => {
  const t = new TabState();
  assert.equal(t.orient, 'h');
  t.flip();
  assert.equal(t.orient, 'v');
});

test('toJSON / from: the tabs and split survive a reload, junk does not', () => {
  const t = new TabState();
  t.open('a'); t.open('b'); t.splitOpen('c', 'v');
  const u = TabState.from(JSON.parse(JSON.stringify(t)));
  assert.deepEqual(rows(u), rows(t));
  assert.equal(u.focus, 1);
  assert.equal(u.orient, 'v');
  assert.notEqual(u.groups[0].id, u.groups[1].id);
  assert.deepEqual(rows(TabState.from(null)), ['']);
  assert.deepEqual(rows(TabState.from({ groups: [{ tabs: ['x', 'x', 3], active: 7 }] })), ['[x]']);
});
