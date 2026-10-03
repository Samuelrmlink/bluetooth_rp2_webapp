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

import { dropZone, tabIndexAt } from '../../js/editor/tabs.js';

test('insert: a tab dragged within its row lands where dropped', () => {
  const t = new TabState();
  for (const p of ['a', 'b', 'c', 'd']) t.open(p);
  t.insert('a', 0, 3);                         // after c: index 3 counts a's old place
  assert.deepEqual(rows(t), ['b c [a] d']);
  t.insert('d', 0, 0);
  assert.deepEqual(rows(t), ['[d] b c a']);
  t.insert('new', 0, 99);
  assert.deepEqual(rows(t), ['d b c a [new]']);
});

test('moveTab: into the other half at a place; the emptied half closes', () => {
  const t = new TabState();
  t.open('a'); t.open('b');
  t.splitOpen('c');
  t.moveTab('a', 0, 1, 0);
  assert.deepEqual(rows(t), ['[b]', '[a] c']);
  assert.equal(t.focus, 1);
  t.moveTab('b', 0, 1, 1);
  assert.equal(t.split, false);
  assert.deepEqual(rows(t), ['a [b] c']);
  assert.equal(t.focus, 0);
});

test('splitTo: a side makes the split there; a lone tab shows twice, one of several moves', () => {
  const t = new TabState();
  t.open('a');
  t.splitTo('left', 'a', 0);                   // the only tab: both halves show it
  assert.equal(t.orient, 'h');
  assert.deepEqual(rows(t), ['[a]', '[a]']);
  assert.equal(t.focus, 0);
  const u = new TabState();
  u.open('a'); u.open('b');
  u.splitTo('bottom', 'b', 0);                 // one of two: it moves down
  assert.equal(u.orient, 'v');
  assert.deepEqual(rows(u), ['[a]', '[b]']);
  assert.equal(u.focus, 1);
  u.splitTo('top', 'f.lua');                   // split already: a file opens in that half
  assert.deepEqual(rows(u), ['a [f.lua]', '[b]']);
  u.splitTo('right', 'a', 0);                  // a tab from the first half moves across
  assert.deepEqual(rows(u), ['[f.lua]', 'b [a]']);
});

test('resize: the focused half grows; kept between 15% and 85%; saved', () => {
  const t = new TabState();
  t.open('a'); t.splitOpen('a');
  assert.equal(t.focus, 1);
  t.resize(0.1);
  assert.equal(t.ratio, 0.4);
  t.focusOther();
  t.resize(0.2);
  assert.equal(t.ratio, 0.6);
  t.resize(1);
  assert.equal(t.ratio, 0.85);
  t.setRatio(-3);
  assert.equal(t.ratio, 0.15);
  assert.equal(TabState.from(JSON.parse(JSON.stringify(t))).ratio, 0.15);
  assert.equal(TabState.from({ groups: [{ tabs: ['x'] }], ratio: 'big' }).ratio, 0.5);
});

test('dropZone: edges split, the middle opens; split already: always the middle', () => {
  const r = { left: 0, top: 0, width: 400, height: 200 };
  assert.equal(dropZone(r, 20, 100, false), 'left');
  assert.equal(dropZone(r, 390, 100, false), 'right');
  assert.equal(dropZone(r, 200, 10, false), 'top');
  assert.equal(dropZone(r, 200, 190, false), 'bottom');
  assert.equal(dropZone(r, 200, 100, false), 'center');
  assert.equal(dropZone(r, 20, 100, true), 'center');
});

test('tabIndexAt: before the first tab whose middle is past the pointer', () => {
  const boxes = [{ left: 0, width: 100 }, { left: 100, width: 100 }, { left: 200, width: 60 }];
  assert.equal(tabIndexAt(boxes, 10), 0);
  assert.equal(tabIndexAt(boxes, 60), 1);
  assert.equal(tabIndexAt(boxes, 240), 3);
  assert.equal(tabIndexAt([], 5), 0);
});
