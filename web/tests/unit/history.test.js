import { test } from 'node:test';
import assert from 'node:assert/strict';
import { History, diff } from '../../js/editor/history.js';

// Type `chars` into `text` at `at`, one keystroke at a time, `gap` ms apart.
function type(h, text, at, chars, t0 = 0, gap = 50) {
  for (let i = 0; i < chars.length; i++) {
    const after = text.slice(0, at) + chars[i] + text.slice(at);
    h.record(text, after, [at, at], [at + 1, at + 1], t0 + i * gap);
    text = after;
    at++;
  }
  return text;
}

test('diff finds the one changed region', () => {
  assert.deepEqual(diff('abcdef', 'abXYef'), { at: 2, del: 'cd', ins: 'XY' });
  assert.deepEqual(diff('aaa', 'aaaa'), { at: 3, del: '', ins: 'a' });
  assert.deepEqual(diff('abc', 'abc'), { at: 3, del: '', ins: '' });
});

test('a typed word is one undo step', () => {
  const h = new History();
  const t = type(h, 'x ', 2, 'hello');
  assert.equal(h.length, 1);
  assert.equal(h.undo(t).text, 'x ');
});

test('words are separate steps, like an editor, not one per keystroke', () => {
  const h = new History();
  let t = type(h, '', 0, 'one two');
  assert.equal(h.length, 2, '"one" then " two"');
  t = h.undo(t).text;
  assert.equal(t, 'one');
  assert.equal(h.undo(t).text, '');
});

test('a pause ends a typing run', () => {
  const h = new History();
  let t = type(h, '', 0, 'ab', 0);
  t = type(h, t, 2, 'cd', 5000);
  assert.equal(h.length, 2);
});

test('a newline ends a step; a paste is its own step', () => {
  const h = new History();
  let t = type(h, '', 0, 'ab\ncd');
  assert.ok(h.length >= 2);
  const before = t;
  t = t + 'PASTED';
  h.record(before, t, [5, 5], [11, 11], 99999);
  assert.equal(h.undo(t).text, before);
});

test('backspacing merges too', () => {
  const h = new History();
  let t = 'abcdef';
  for (let i = 0; i < 3; i++) {
    const at = t.length - 1;
    const after = t.slice(0, -1);
    h.record(t, after, [at + 1, at + 1], [at, at], i * 50);
    t = after;
  }
  assert.equal(h.length, 1);
  assert.equal(h.undo(t).text, 'abcdef');
});

test('undo restores the selection before; redo reapplies and restores after', () => {
  const h = new History();
  h.record('abc', 'aXc', [1, 2], [2, 2]);
  const u = h.undo('aXc');
  assert.deepEqual(u, { text: 'abc', sel: [1, 2] });
  assert.deepEqual(h.redo(u.text), { text: 'aXc', sel: [2, 2] });
});

test('a new change after undo clears redo', () => {
  const h = new History();
  h.record('a', 'ab', [1, 1], [2, 2]);
  const t = h.undo('ab').text;
  h.record(t, 'aZ', [1, 1], [2, 2], 9999);
  assert.equal(h.redo('aZ'), null);
});

test('vim: an insert session squashes into one step', () => {
  const h = new History();
  const start = h.length;
  let t = type(h, 'x', 1, 'one two three', 0, 1500);   // slow typing: many steps
  assert.ok(h.length > 1);
  h.squashSince(start, 'x', t, [1, 1], [t.length, t.length]);
  assert.equal(h.length, 1);
  assert.equal(h.undo(t).text, 'x');
});

test('seal keeps separate commands separate even when contiguous', () => {
  const h = new History();
  h.record('', 'a', [0, 0], [1, 1], 0);
  h.seal();
  h.record('a', 'ab', [1, 1], [2, 2], 10);
  assert.equal(h.length, 2);
});
