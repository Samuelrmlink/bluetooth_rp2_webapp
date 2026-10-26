import { test } from 'node:test';
import assert from 'node:assert/strict';
import { applySelect, gestureOf } from '../../js/editor/select.js';

const rows = ['a', 'b', 'c', 'd', 'e'];

test('a plain click selects only that row and opens it', () => {
  const s = applySelect(rows, { cursor: 'a', anchor: 'a', picked: new Set(['a', 'c']) }, 'plain', 'd');
  assert.equal(s.cursor, 'd');
  assert.equal(s.anchor, 'd');
  assert.equal(s.open, true);
  assert.deepEqual([...s.picked], ['d']);
});

test('ctrl toggles the row and does not open it', () => {
  let s = applySelect(rows, { cursor: null, anchor: null, picked: new Set() }, 'plain', 'b');
  s = applySelect(rows, s, 'toggle', 'd');
  assert.equal(s.open, false);
  assert.equal(s.cursor, 'd');
  assert.equal(s.anchor, 'b');
  assert.deepEqual([...s.picked], ['b', 'd']);
  s = applySelect(rows, s, 'toggle', 'b');
  assert.deepEqual([...s.picked], ['d']);
  assert.equal(s.open, false);
});

test('shift replaces the set with the visible rows from the anchor through the cursor', () => {
  let s = applySelect(rows, { cursor: null, anchor: null, picked: new Set() }, 'plain', 'b');
  s = applySelect(rows, s, 'toggle', 'e');
  s = applySelect(rows, s, 'range', 'd');
  assert.equal(s.open, false);
  assert.equal(s.anchor, 'b');
  assert.deepEqual([...s.picked], ['b', 'c', 'd']);
  s = applySelect(rows, s, 'range', 'a');
  assert.deepEqual([...s.picked], ['a', 'b']);
});

test('a range with no anchor starts at the cursor', () => {
  const s = applySelect(rows, { cursor: 'c', anchor: null, picked: new Set(['e']) }, 'range', 'a');
  assert.deepEqual([...s.picked], ['a', 'b', 'c']);
  assert.equal(s.open, false);
});

test('shift is a range, ctrl or cmd toggles, and a bare click is plain', () => {
  assert.equal(gestureOf({ shiftKey: true, ctrlKey: true }), 'range');
  assert.equal(gestureOf({ ctrlKey: true }), 'toggle');
  assert.equal(gestureOf({ metaKey: true }), 'toggle');
  assert.equal(gestureOf({}), 'plain');
});
